// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
//
// Integration test: stale-handler safety in http/http_client.hpp.
//
// Root cause
// ----------
// The production crash (commit a74ef929) showed:
//
//   recvMessage() failed: asio.ssl error from https://...
//   recvMessage() failed: stale parser from https://...
//   terminate: std::bad_function_call
//
// "stale parser" is boost::beast::http::error::stale_parser — returned by
// Beast's basic_parser when async_read is called on a parser whose
// is_done() is already true.
//
// The crash sequence on a ConnectionInfo with the UNSAFE
// bind_front(&afterRead, this, shared_from_this()) pattern:
//
//  1. recvMessage() emplaces parser, posts async_read(sslConn, handler1).
//     Beast's composed async_read drives an inner async_read_some loop.
//
//  2. Server sends HTTP/1.1 200 keep-alive then immediately sends a bad
//     SSL record on the same connection.
//     Beast sees the good response bytes first; its inner async_read_some
//     loop issues a second async_read_some to read the rest (even though
//     the parser finishes on the first chunk).  That second async_read_some
//     is still in flight as an outstanding async op.
//
//  3. handler1 fires (ec=ok, response complete):
//       afterRead → callback(keepAlive=true, connId, res)
//       → ConnectionPool::afterSendData → resHandler(200)
//       → sendNext() → conn->callback = nullptr
//       → sendMessage() for request 2
//       → recvMessage() → parser.emplace() → async_read(sslConn, handler2)
//
//  4. The outstanding inner async_read_some from step 2 now completes
//     against handler1's composed operation.  Beast tries to feed the bad
//     SSL record bytes into the (now re-emplaced) parser → stale_parser OR
//     SSL_ERROR_SSL → handler1 fires a second time with ec = asio.ssl error
//     (or beast::http::error::stale_parser, shown in the crash log).
//
//  5. handler1 second fire: ec is not operation_aborted, not stream_truncated
//     → waitAndRetry() → retryCount 1 >= maxRetryAttempts 1
//     → callback(false, connId, res) — callback is nullptr → bad_function_call.
//
//  With the SAFE bind_front(&afterRead, shared_from_this()) pattern:
//    - `this` is kept alive by the shared_ptr call target.
//    - callback == nullptr → `if (callback)` check prevents bad_function_call.
//    (The `if (callback)` guard was added by a74ef929 as a band-aid; our
//     fix removes the need for it by ensuring `this` is always valid, but
//     the null-callback path must still be handled safely.)
//
// What this test does
// -------------------
// One server, one port, two queued requests.
//
//  Server (single connection, keep-alive):
//    1. Accept + TLS handshake.
//    2. Read request 1 headers.
//    3. Write HTTP/1.1 200 OK Connection:keep-alive via TLS.
//       → handler1(ec=ok) fires, sendNext sends request 2,
//         recvMessage posts handler2.
//    4. Immediately write a bad TLS record (invalid ContentType 0xff)
//       on the raw TCP socket — bypassing the TLS layer.
//       → The inner async_read_some still pending from handler1's
//         composed async_read receives the garbage → SSL_ERROR_SSL
//         → handler1 fires a second time.
//    5. Close the TCP socket.
//
//  handler1 second fire (asio.ssl error or stale_parser):
//    - ec != operation_aborted, != stream_truncated → waitAndRetry()
//    - retryCount 1 >= maxRetryAttempts 1
//    - callback(false, connId, res) — callback is nullptr at this point
//    - SAFE pattern: object alive, crash avoided.
//    - UNSAFE pattern: `this` may be dangling → UB / bad_function_call.
//
// Assertions:
//   statusCodes contains 200 (request 1 succeeded).
//   No crash proves the stale handler fired safely on a live object.
//
// [1] Beast basic_parser stale_parser:
//     https://github.com/boostorg/beast/blob/develop/
//     include/boost/beast/http/impl/basic_parser.ipp#L91
// [2] Boost.Asio engine::map_error_code():
//     https://github.com/boostorg/asio/blob/boost-1.90.0/
//     include/boost/asio/ssl/detail/impl/engine.ipp#L244

#include "http/http_client.hpp"
#include "http/http_response.hpp"
#include "ossl_test_memory.hpp"
#include "ossl_wrappers.hpp"
#include "ssl_key_handler.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>
#include <boost/url/url.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace crow
{

// One-time OpenSSL memory initialisation (same pattern as mutual_tls.cpp).
static const OpenSSLTestMemory osslInit;

// ============================================================================
// makeServerSslContext — self-signed cert+key in memory, no filesystem.
// ============================================================================
static boost::asio::ssl::context makeServerSslContext()
{
    std::string certAndKey = ensuressl::generateSslCertificate("localhost");
    EXPECT_FALSE(certAndKey.empty());

    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
    boost::system::error_code ec;

    boost::asio::const_buffer buf(certAndKey.data(), certAndKey.size());
    ctx.use_certificate_chain(buf, ec);
    EXPECT_FALSE(ec) << "use_certificate_chain: " << ec.message();

    ctx.use_private_key(buf, boost::asio::ssl::context::pem, ec);
    EXPECT_FALSE(ec) << "use_private_key: " << ec.message();

    return ctx;
}

// ============================================================================
// Server pipeline — one connection, keep-alive then bad record.
// ============================================================================
using SslSocket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

// Step 3+4: after writing the 200 keep-alive response via TLS, inject a
// bad TLS record directly on the raw TCP socket then close.
//
// The client's handler1 composed async_read has an inner async_read_some
// still in flight.  That in-flight op receives the garbage bytes →
// SSL_ERROR_SSL → handler1 fires a second time with asio.ssl error.
static void serverAfterWrite(
    std::shared_ptr<SslSocket> sock, std::shared_ptr<std::string> /*respBuf*/,
    boost::system::error_code /*ec*/, std::size_t /*n*/)
{
    // TLS record with ContentType 0xff — invalid, causes SSL_ERROR_SSL on
    // the peer's next read attempt.
    static constexpr std::string_view badRecord = "\xff\x03\x03\x00\x05XXXXX";
    boost::system::error_code writeEc;
    boost::asio::write(sock->next_layer(), boost::asio::buffer(badRecord),
                       writeEc);

    // TCP close — causes stream_truncated if the SSL error alone is not
    // enough to unblock the inner async_read_some.
    boost::system::error_code closeEc;
    sock->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                closeEc);
    sock->next_layer().close(closeEc);
}

// Step 2: after reading request 1 headers, write HTTP 200 keep-alive.
static void serverAfterReadRequest(
    std::shared_ptr<SslSocket> sock,
    std::shared_ptr<boost::asio::streambuf> /*reqBuf*/,
    boost::system::error_code /*ec*/, std::size_t /*n*/)
{
    auto respBuf = std::make_shared<std::string>(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "Connection: keep-alive\r\n"
        "\r\n");
    boost::asio::async_write(*sock, boost::asio::buffer(*respBuf),
                             std::bind_front(serverAfterWrite, sock, respBuf));
}

// Step 1b: after TLS handshake, drain request 1 headers.
static void serverAfterHandshake(std::shared_ptr<SslSocket> sock,
                                 boost::system::error_code ec)
{
    ASSERT_FALSE(ec) << "server handshake: " << ec.message();
    auto reqBuf = std::make_shared<boost::asio::streambuf>();
    boost::asio::async_read_until(
        *sock, *reqBuf, "\r\n\r\n",
        std::bind_front(serverAfterReadRequest, sock, reqBuf));
}

// Step 1a: TCP accept, start TLS handshake.
static void serverAfterAccept(std::shared_ptr<SslSocket> sock,
                              boost::system::error_code ec)
{
    ASSERT_FALSE(ec) << "server accept: " << ec.message();
    sock->async_handshake(boost::asio::ssl::stream_base::server,
                          std::bind_front(serverAfterHandshake, sock));
}

// ============================================================================
// Test: StaleHandler_AfterSuccessfulRead_DoesNotCrash
//
// One connection, keep-alive.  Server sends 200 then immediately injects
// a bad TLS record.  Beast's inner async_read_some for handler1 receives
// the bad record after handler1's first (successful) completion; it fires
// handler1 a second time with asio.ssl error or stale_parser.
//
// handler1 second fire: retryCount 1 >= maxRetryAttempts 1 →
// callback(false, connId, res) — callback is nullptr (nulled by sendNext).
//
// SAFE pattern: shared_from_this() as call target keeps `this` alive;
// null callback is reached without a crash.
//
// Asserts statusCodes contains 200.  No crash is the primary invariant.
// ============================================================================
TEST(HttpClientTest, StaleHandler_AfterSuccessfulRead_DoesNotCrash)
{
    boost::asio::io_context io;
    boost::asio::ssl::context serverCtx = makeServerSslContext();

    boost::asio::ip::tcp::acceptor acceptor(
        io, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0));
    acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    uint16_t port = acceptor.local_endpoint().port();

    auto sock = std::make_shared<SslSocket>(io, serverCtx);
    acceptor.async_accept(sock->next_layer(),
                          std::bind_front(serverAfterAccept, sock));

    // maxRetryAttempts = 1, retryIntervalSecs = 0:
    //   handler1 first fire (ok):      retryCount stays 0.
    //   handler1 second fire (ssl err): retryCount 0 < 1 → increments to 1,
    //                                   posts retry timer (fires immediately).
    //   Retry timer: shutdownConn → tries to reconnect (no server → fails).
    //   OR: retryCount 1 >= 1 → callback(502) with nullptr → crash avoided.
    auto policy = std::make_shared<ConnectionPolicy>();
    policy->maxRetryAttempts = 1;
    policy->retryPolicyAction = "TerminateAfterRetries";
    policy->retryIntervalSecs = std::chrono::seconds(0);

    HttpClient client(io, policy);

    std::vector<int> statusCodes;
    auto resHandler = [&statusCodes](Response& res) {
        statusCodes.push_back(static_cast<int>(res.result()));
    };

    boost::urls::url dest(std::format("https://127.0.0.1:{}", port));
    boost::beast::http::fields headers;

    // Two requests queued.  Request 1 completes with 200; request 2 is
    // sent on the keep-alive connection before the bad record arrives.
    client.sendDataWithCallback("", dest,
                                ensuressl::VerifyCertificate::NoVerify, headers,
                                boost::beast::http::verb::get, resHandler);
    client.sendDataWithCallback("", dest,
                                ensuressl::VerifyCertificate::NoVerify, headers,
                                boost::beast::http::verb::get, resHandler);

    io.run_for(std::chrono::seconds(5));

    // At least one resHandler call (the 200 from request 1).
    // No crash is the primary invariant: proves the stale handler from
    // request 1's async_read fired safely on a live ConnectionInfo
    // (shared_from_this() as bind_front call target kept it alive).
    ASSERT_FALSE(statusCodes.empty())
        << "Expected at least one resHandler call; got none";

    EXPECT_EQ(statusCodes[0], static_cast<int>(boost::beast::http::status::ok))
        << "Request 1 must succeed with HTTP 200";
}

} // namespace crow
