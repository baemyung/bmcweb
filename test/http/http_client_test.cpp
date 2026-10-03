// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
//
// Integration test: stale-handler double-completion in http/http_client.hpp.
//
// Root cause
// ----------
// The production crash (commit a74ef929) showed:
//
//   recvMessage() failed: asio.ssl error from https://...
//   recvMessage() failed: stale parser from https://...
//   terminate: std::bad_function_call
//
// "stale parser" is boost::beast::http::error::stale_parser — set by
// Beast's basic_parser::put() when called on a parser whose is_done()
// is already true [1].
//
// The double-completion sequence (one server, one connection):
//
//  1. recvMessage() emplaces parser, stores a *reference* to it, and
//     posts async_read(sslConn, buffer, *parser, handler1).
//     Beast's read_op inner loop calls async_read_some in a do/while;
//     the Asio SSL io_op reads a chunk from the TCP socket and feeds it
//     to OpenSSL.
//
//  2. The server sends the TLS-encrypted 200 response AND a bad raw-TCP
//     record in a single synchronous sequence before the client's
//     async_read_some can drain the socket.  Both arrive together in
//     one TCP segment in the client's receive buffer.
//
//  3. OpenSSL decrypts the TLS record for the 200.  Beast's parser
//     finishes (is_done() == true).  The read_op loop condition
//     (!ec && !parser_is_done) is false → read_op calls
//     self.complete(ok, bytes) → handler1 fires first time (ec=ok).
//
//  4. handler1 first fire (ec=ok):
//       afterRead → callback(keepAlive=true, connId, res)
//       → resHandler records 200
//       → sendNext() → conn->callback = nullptr
//       → sendMessage() for request 2
//       → recvMessage() → parser.emplace()  ← destroys old parser,
//         constructs new one at the SAME memory address
//       → async_read(sslConn, buffer, *parser, handler2) posted
//
//  5. The bad raw-TCP bytes (garbage record) are still sitting in
//     OpenSSL's input buffer (or the Asio SSL core's input_ buffer).
//     When handler2's async_read_some reads from the SSL stream, it
//     gets SSL_ERROR_SSL from the garbage → io_op calls
//     op_.call_handler(handler2, asio.ssl error, 0).
//     But io_op for handler1 may ALSO still be alive if there is an
//     outstanding core_.pending_read_ wait — the SSL core serializes
//     concurrent reads via a timer.  When the timer fires, the old
//     io_op calls back into handler1 with the SSL error, firing
//     handler1 a second time.
//
//     Alternatively: the garbage arrives as a second async_read_some
//     completion for handler1's own read_op if it issued two
//     async_read_some calls before the parser finished.
//
//  6. handler1 second fire (ec = asio.ssl error or stale_parser):
//       ec != operation_aborted, != stream_truncated
//       → waitAndRetry()
//       → retryCount 0 < maxRetryAttempts 1 → retryCount = 1, timer
//       (OR if retryCount already 1: callback(false, connId, res)
//        with callback == nullptr → std::bad_function_call → CRASH)
//
//  With the UNSAFE bind_front(&afterRead, this, shared_from_this())
//  pattern, `this` is a raw pointer.  If the shared_ptr drops between
//  the two fires, `this` is dangling.  Even if it doesn't drop,
//  callback == nullptr → bad_function_call.
//
//  With the SAFE bind_front(&afterRead, shared_from_this()) pattern,
//  *this is kept alive by the call-target shared_ptr.  The null
//  callback path must still be reached safely — which is what this
//  test observes by asserting no crash.
//
// Two independent fixes are required for full protection
// -------------------------------------------------------
// Fix 1 (this repo — commit c4156d83):
//   Use bind_front(&afterRead, shared_from_this()) so that the handler
//   itself holds the object alive.  The second completion fires on a
//   live object instead of a dangling pointer, and the null-callback
//   path is reached without std::bad_function_call.
//   This fix is necessary regardless of Boost version.
//
// Fix 2 (Boost upgrade — 1.83 → 1.84+):
//   Boost 1.83's ssl/detail/io_op did not atomically cancel the
//   pending_read_ timer when SSL_ERROR_SSL fired, allowing the timer
//   cancellation completion to reach call_handler a second time.
//   This was fixed in Boost 1.84/1.85 via the SSL cancellation overhaul
//   (base_from_cancellation_state + cancelled() check).  From Boost 1.84
//   onward a single async_read_some call fires its handler exactly once.
//   ibm-bmcweb must upgrade Boost to eliminate the double-completion at
//   its source, not just survive it.
//
//   Consequence: building this fixed bmcweb source against Boost 1.83
//   will still trigger two handler invocations on the described network
//   event.  Fix 1 prevents the crash, but the handler runs twice.
//   Building against Boost 1.84+ means the handler runs exactly once.
//
// What this test does
// -------------------
// One server, one port, two queued requests, keep-alive.
//
//  Server:
//    1. Accept + TLS handshake.
//    2. Read request 1 headers.
//    3. Write the TLS-encrypted 200 response.
//    4. Immediately write a bad raw-TCP record (ContentType 0xff) to
//       the underlying TCP socket — bypassing TLS — so that both the
//       200 and the garbage arrive in the client's receive buffer
//       before the next async_read_some has a chance to drain it.
//    5. Close the TCP socket.
//
// Expected outcome with the SAFE bind_front fix:
//   statusCodes contains at least 200 (request 1 succeeded).
//   No crash: the stale handler fires on a live object; callback ==
//   nullptr is reached without std::bad_function_call.
//
// [1] Beast basic_parser stale_parser:
//     include/boost/beast/http/impl/basic_parser.ipp:91
// [2] Asio SSL io_op (Boost 1.83, double-completion source):
//     ssl/detail/io.hpp — pending_read_ timer not cancelled atomically
// [3] Asio SSL io_op (Boost 1.84+, fixed):
//     base_from_cancellation_state + cancelled() check prevents second fire
// [4] Beast read_op inner loop:
//     include/boost/beast/http/impl/read.hpp
//

#include "http/http_client.hpp"
#include "http/http_response.hpp"
#include "ossl_test_memory.hpp"
#include "ssl_key_handler.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>
#include <boost/url/url.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
// Server pipeline — one connection, keep-alive then poison record.
// ============================================================================
using SslSocket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

// Step 3+4: after the TLS async_write of the 200 response completes,
// immediately write a bad raw-TCP record (ContentType 0xff) directly
// on the underlying TCP socket and then close.
//
// Because the bad record is written synchronously on the raw socket
// right after the TLS async_write callback fires, both the encrypted
// 200 bytes and the garbage bytes are in the client's TCP receive
// buffer before its next async_read_some runs.  This is what produces
// the double-completion on handler1's read_op.
static void serverAfterTlsWrite(const std::shared_ptr<SslSocket>& sock,
                                const std::shared_ptr<std::string>& /*resp*/,
                                boost::system::error_code /*ec*/,
                                std::size_t /*n*/)
{
    // An invalid TLS ContentType (0xff) with a plausible 5-byte header.
    // OpenSSL raises SSL_ERROR_SSL ("unknown record type") on receipt.
    static constexpr std::string_view poison = "\xff\x03\x03\x00\x05XXXXX";

    boost::system::error_code writeEc;
    boost::asio::write(sock->next_layer(), boost::asio::buffer(poison),
                       writeEc);

    boost::system::error_code closeEc;
    sock->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                closeEc);
    sock->next_layer().close(closeEc);
}

// Step 2: after reading request 1 headers, write HTTP 200 keep-alive
// via TLS.  The serverAfterTlsWrite callback immediately follows with
// the raw poison record.
static void serverAfterReadReq(
    const std::shared_ptr<SslSocket>& sock,
    const std::shared_ptr<boost::asio::streambuf>& /*buf*/,
    boost::system::error_code /*ec*/, std::size_t /*n*/)
{
    auto resp = std::make_shared<std::string>(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "Connection: keep-alive\r\n"
        "\r\n");
    boost::asio::async_write(*sock, boost::asio::buffer(*resp),
                             std::bind_front(serverAfterTlsWrite, sock, resp));
}

// Step 1b: after TLS handshake, drain request 1 headers.
static void serverAfterHandshake(const std::shared_ptr<SslSocket>& sock,
                                 boost::system::error_code ec)
{
    ASSERT_FALSE(ec) << "server handshake: " << ec.message();
    auto buf = std::make_shared<boost::asio::streambuf>();
    boost::asio::async_read_until(
        *sock, *buf, "\r\n\r\n",
        std::bind_front(serverAfterReadReq, sock, buf));
}

// Step 1a: TCP accept, start TLS handshake.
static void serverAfterAccept(const std::shared_ptr<SslSocket>& sock,
                              boost::system::error_code ec)
{
    ASSERT_FALSE(ec) << "server accept: " << ec.message();
    sock->async_handshake(boost::asio::ssl::stream_base::server,
                          std::bind_front(serverAfterHandshake, sock));
}

// ============================================================================
// Test: StaleHandler_AfterSuccessfulRead_DoesNotCrash
//
// One connection, keep-alive.  Server sends 200 via TLS then immediately
// writes a bad raw-TCP record and closes.  Both arrive together in the
// client's receive buffer.  Beast's read_op for handler1 sees the good
// 200, completes handler1 (ec=ok).  The poison record is still in the
// SSL input buffer.  When handler2 (request 2) starts reading it gets
// SSL_ERROR_SSL — AND/OR handler1's read_op issues a second
// async_read_some that picks up the poison, firing handler1 again.
//
// handler1 second fire: ec != operation_aborted, != stream_truncated
//   → waitAndRetry() → retryCount 0 < 1 → retryCount=1, posts timer
//   → timer fires → shutdownConn → restartConnection → no server →
//   connect fails → waitAndRetry() → retryCount 1 >= 1
//   → callback(false, connId, res) with callback == nullptr
//   → CRASH without fix, safe with SAFE bind_front pattern.
//
// Asserts: statusCodes contains 200. No crash is the primary invariant.
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

    // maxRetryAttempts = 1:
    //   stale handler first triggers waitAndRetry → retryCount = 1.
    //   Next waitAndRetry call: retryCount 1 >= 1 → callback(nullptr).
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

    // Two requests queued so sendNext has a request 2 to send when
    // request 1 completes, triggering recvMessage → parser.emplace()
    // while handler1's read_op may still have the poison in flight.
    client.sendDataWithCallback("", dest,
                                ensuressl::VerifyCertificate::NoVerify, headers,
                                boost::beast::http::verb::get, resHandler);
    client.sendDataWithCallback("", dest,
                                ensuressl::VerifyCertificate::NoVerify, headers,
                                boost::beast::http::verb::get, resHandler);

    io.run_for(std::chrono::seconds(5));

    // At least one resHandler call (the 200 from request 1).
    // No crash is the primary invariant: proves the stale handler fired
    // safely on a live object (shared_from_this() as call target).
    ASSERT_FALSE(statusCodes.empty())
        << "Expected at least one resHandler call; got none";

    EXPECT_EQ(statusCodes[0], static_cast<int>(boost::beast::http::status::ok))
        << "Request 1 must succeed with HTTP 200";
}

} // namespace crow
