// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
//
// Integration test: SSL double-completion in http/http_client.hpp.
//
// Root cause
// ----------
// The production crash (commit a74ef929) showed:
//
//   recvMessage() failed: asio.ssl error from https://...
//   recvMessage() failed: stale parser from https://...
//   terminate: std::bad_function_call
//
// Two concurrent afterRead completions fired on the same ConnectionInfo:
//
//  1. Request 1 sent over a keep-alive connection.  recvMessage() posts
//     async_read(parser1).  Server sends HTTP 200 keep-alive.
//     async_read(parser1) completes with success → afterRead fires →
//     callback(keepAlive=true) → sendNext() → sendMessage() for
//     request 2 → recvMessage() posts async_read(parser2).
//
//  2. Server then sends garbage bytes on the raw TCP socket (bypassing
//     TLS) and closes.
//     async_read(parser2) gets two completions:
//       Completion A: SSL_ERROR_SSL (garbage record) → asio.ssl error
//         → afterRead(error) → waitAndRetry, retryCount → 1.
//       Completion B: TCP EOF → stream_truncated
//         → afterRead(stream_truncated), parser2->is_done() false
//         → waitAndRetry, retryCount 1 >= maxRetryAttempts 1
//         → callback(false, 502).
//
//  3. With the unsafe bind_front(&afterRead, this, shared_from_this())
//     pattern, `this` is the raw call target.  Between Completion A and
//     Completion B the last shared_ptr could drop, leaving `this`
//     dangling for Completion B — use-after-free / bad_function_call.
//
//  4. The safe bind_front(&afterRead, shared_from_this()) pattern keeps
//     the object alive for as long as any completion handler exists,
//     making both invocations safe.
//
// What this test does
// -------------------
// Spin up a real TLS server on 127.0.0.1 (self-signed cert in memory).
// Two requests are queued.  The server:
//   1. Accepts + TLS handshake.
//   2. Reads request 1 headers.
//   3. Writes HTTP/1.1 200 keep-alive Content-Length:0 (via TLS).
//      → async_read(parser1) completes, sendNext sends request 2,
//        recvMessage posts async_read(parser2).
//   4. Writes garbage bytes directly on the raw TCP socket.
//      → SSL_ERROR_SSL on async_read(parser2) → Completion A.
//   5. Closes the TCP socket.
//      → stream_truncated on async_read(parser2) → Completion B.
//
// With maxRetryAttempts = 1:
//   Completion A: retryCount 0 < 1 → increments to 1, posts retry timer.
//   Completion B: retryCount 1 >= 1 → calls resHandler(502 Bad Gateway).
//
// Assertions:
//   statusCodes == {200, 502}
//   200 proves request 1 succeeded on the keep-alive connection.
//   502 proves the double-completion on request 2 exhausted the retries.
//   No crash proves safe bind_front kept `this` valid for both
//   async_read completions on the same ConnectionInfo.
//
// [1] Boost.Asio engine::map_error_code():
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
// Server pipeline.
//
// Step 4: after the keep-alive 200 is written via TLS, inject garbage
// bytes directly onto the raw TCP socket then close.
//
// At this point the client has already dispatched async_read(parser1)
// successfully and posted async_read(parser2) for request 2.  The
// garbage+close hits async_read(parser2):
//   Completion A: SSL_ERROR_SSL → asio.ssl error → waitAndRetry
//   Completion B: TCP EOF     → stream_truncated → waitAndRetry
// Two waitAndRetry calls with maxRetryAttempts=1 exhaust the limit on
// the second call → resHandler(502).
// ============================================================================
using SslSocket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

static void serverAfterWrite(
    std::shared_ptr<SslSocket> sock, std::shared_ptr<std::string> /*buf*/,
    boost::system::error_code /*ec*/, std::size_t /*n*/)
{
    // Garbage bytes on the raw socket → SSL_ERROR_SSL on the client's
    // async_read(parser2) → Completion A (asio.ssl error).
    static constexpr std::string_view garbage = "\xff\xff\xff\xff\xff\xff";
    boost::system::error_code writeEc;
    boost::asio::write(sock->next_layer(), boost::asio::buffer(garbage),
                       writeEc);

    // TCP close → stream_truncated on async_read(parser2) → Completion B.
    boost::system::error_code closeEc;
    sock->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                closeEc);
    sock->next_layer().close(closeEc);
}

// Step 3: write HTTP/1.1 200 keep-alive so the client reuses the
// connection for request 2, posting async_read(parser2) before the
// garbage arrives in step 4.
static void serverAfterReadRequest(
    std::shared_ptr<SslSocket> sock,
    std::shared_ptr<boost::asio::streambuf> /*buf*/,
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

// Step 2: after TLS handshake, drain request 1 headers.
static void serverAfterHandshake(std::shared_ptr<SslSocket> sock,
                                 boost::system::error_code ec)
{
    ASSERT_FALSE(ec) << "server handshake: " << ec.message();
    auto reqBuf = std::make_shared<boost::asio::streambuf>();
    boost::asio::async_read_until(
        *sock, *reqBuf, "\r\n\r\n",
        std::bind_front(serverAfterReadRequest, sock, reqBuf));
}

// Step 1: after TCP accept, start TLS handshake.
static void serverAfterAccept(std::shared_ptr<SslSocket> sock,
                              boost::system::error_code ec)
{
    ASSERT_FALSE(ec) << "accept: " << ec.message();
    sock->async_handshake(boost::asio::ssl::stream_base::server,
                          std::bind_front(serverAfterHandshake, sock));
}

// ============================================================================
// Test: DoubleCompletion_KeepAlive_GarbageRecord_BothCompletionsFire
//
// Two requests are queued.  Request 1 succeeds (200 OK) over a
// keep-alive connection.  The server then injects garbage + closes,
// hitting the async_read for request 2 twice:
//   Completion A: asio.ssl error → waitAndRetry (retryCount → 1)
//   Completion B: stream_truncated → waitAndRetry (retryCount 1 >= 1)
//     → resHandler(502 Bad Gateway)
//
// statusCodes == {200, 502}:
//   200 — request 1 succeeded.
//   502 — double-completion on request 2 exhausted the retry limit.
//
// No crash proves safe bind_front kept `this` valid for both
// Completion A and Completion B on the same ConnectionInfo.
// ============================================================================
TEST(HttpClientTest,
     DoubleCompletion_KeepAlive_GarbageRecord_BothCompletionsFire)
{
    boost::asio::io_context io;

    boost::asio::ssl::context serverCtx = makeServerSslContext();

    boost::asio::ip::tcp::acceptor acceptor(
        io, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0));
    acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

    uint16_t port = acceptor.local_endpoint().port();

    auto serverSocket = std::make_shared<SslSocket>(io, serverCtx);
    acceptor.async_accept(serverSocket->next_layer(),
                          std::bind_front(serverAfterAccept, serverSocket));

    // maxRetryAttempts = 1:
    //   Completion A: retryCount 0 < 1 → increments, posts retry timer.
    //   Completion B: retryCount 1 >= 1 → resHandler(502).
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

    // Enqueue two requests.  The pool reuses the connection for request 2
    // after request 1 completes with keep-alive.
    client.sendDataWithCallback("", dest,
                                ensuressl::VerifyCertificate::NoVerify, headers,
                                boost::beast::http::verb::get, resHandler);
    client.sendDataWithCallback("", dest,
                                ensuressl::VerifyCertificate::NoVerify, headers,
                                boost::beast::http::verb::get, resHandler);

    io.run_for(std::chrono::seconds(5));

    // statusCodes[0] == 200: request 1 succeeded on the keep-alive
    //   connection — confirms the first async_read completed cleanly.
    // statusCodes[1] == 502: the double-completion (Completion A then B)
    //   on request 2's async_read exhausted the retry limit.
    // No crash confirms safe bind_front kept `this` valid for both
    // completions on the same ConnectionInfo object.
    ASSERT_EQ(statusCodes.size(), 2U)
        << "Expected two resHandler calls; got " << statusCodes.size();

    EXPECT_EQ(statusCodes[0], static_cast<int>(boost::beast::http::status::ok))
        << "Request 1 must succeed with HTTP 200";

    EXPECT_EQ(statusCodes[1],
              static_cast<int>(boost::beast::http::status::bad_gateway))
        << "Request 2 must fail with 502 after double-completion exhausts "
           "the retry limit";
}

} // namespace crow
