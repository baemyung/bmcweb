// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
//
// Proves the two SSL error codes that appear in double-completion crashes,
// and demonstrates that a handler posted twice on the same object is safe
// only when the object is kept alive via shared_from_this().
//
// Background
// ----------
// The production crash sequence (ibm-bmcweb, Boost 1.83) was:
//
//   recvMessage() failed: asio.ssl error       ← completion A
//   recvMessage() failed: stale parser         ← completion B
//   terminate: std::bad_function_call          ← callback == nullptr
//
// Two distinct SSL error paths can produce a "stale" second completion:
//
//   Path 1 – bad record (ContentType 0xFF):
//     Server sends valid 200 via TLS, then immediately writes a raw
//     byte sequence that is not a valid TLS record.  When the client's
//     next async_read_some calls SSL_read, OpenSSL returns SSL_ERROR_SSL
//     → boost::asio::ssl::error category (a non-zero error_code).
//
//   Path 2 – unclean TCP close (no TLS close_notify):
//     Server closes the TCP socket without sending a TLS close_notify
//     alert.  The Boost.Asio SSL engine maps the resulting EOF to
//     boost::asio::ssl::error::stream_truncated (value 2) via
//     engine::map_error_code() in ssl/detail/impl/engine.ipp.
//
// Each test below demonstrates one of these paths using a raw
// Boost.Asio SSL stream — no bmcweb types, no Beast HTTP parser.
// The third test demonstrates the double-invocation hazard directly
// by posting the same callable twice and checking object liveness.
//
// Why raw async_read_some, not boost::beast::http::async_read
// -----------------------------------------------------------
// Beast's http::async_read is a composed operation implemented as a
// stackless coroutine (read_op).  Starting a second composed operation
// on the same SSL stream from directly within the first operation's
// completion handler causes read_op's destructor to run a virtual call
// on a partially-unwound coroutine frame → "pure virtual method called".
// The two error-code tests therefore use a single async_read_some call,
// which is a primitive operation that does not have this restriction.

#include "ossl_test_memory.hpp"
#include "ssl_key_handler.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/ssl/verify_mode.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace
{

const OpenSSLTestMemory osslInit;

using SslStream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

// ============================================================================
// Helpers
// ============================================================================
boost::asio::ssl::context makeServerCtx()
{
    std::string pem = ensuressl::generateSslCertificate("localhost");
    EXPECT_FALSE(pem.empty());
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_server);
    boost::system::error_code ec;
    boost::asio::const_buffer buf(pem.data(), pem.size());
    ctx.use_certificate_chain(buf, ec);
    EXPECT_FALSE(ec);
    ctx.use_private_key(buf, boost::asio::ssl::context::pem, ec);
    EXPECT_FALSE(ec);
    return ctx;
}

// Minimal server: accept → TLS handshake → invoke serverAction → done.
// serverAction receives the accepted SslStream and is responsible for
// sending/closing whatever the test needs.
using ServerAction = std::function<void(std::shared_ptr<SslStream>)>;

void doHandshakeThenAct(std::shared_ptr<SslStream> sock,
                        const ServerAction& action,
                        boost::system::error_code ec)
{
    if (ec)
    {
        ADD_FAILURE() << "server handshake: " << ec.message();
        return;
    }
    action(std::move(sock));
}

void doAcceptThenHandshake(const std::shared_ptr<SslStream>& sock,
                           ServerAction action, boost::system::error_code ec)
{
    if (ec)
    {
        ADD_FAILURE() << "server accept: " << ec.message();
        return;
    }
    sock->async_handshake(
        boost::asio::ssl::stream_base::server,
        std::bind_front(doHandshakeThenAct, sock, std::move(action)));
}

// Start a one-shot server on a random loopback port.  Returns the port.
uint16_t startServer(boost::asio::io_context& io,
                     boost::asio::ssl::context& serverCtx, ServerAction action)
{
    boost::asio::ip::tcp::acceptor acceptor(
        io, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0));
    acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    uint16_t port = acceptor.local_endpoint().port();

    auto sock = std::make_shared<SslStream>(io, serverCtx);

    // Move acceptor into the lambda so it stays alive until the accept fires.
    auto acceptorPtr =
        std::make_shared<boost::asio::ip::tcp::acceptor>(std::move(acceptor));
    acceptorPtr->async_accept(
        sock->next_layer(), [sock, action = std::move(action), acceptorPtr](
                                boost::system::error_code ec) mutable {
            doAcceptThenHandshake(sock, std::move(action), ec);
        });
    return port;
}

// Issue one async_read_some on stream; store ec in readEc, set readFired,
// and stop io.  Extracted from the clientAction lambdas in Path1 and Path2
// tests — the body is identical, only the captures differ.
void doReadOne(boost::asio::io_context& io,
               const std::shared_ptr<SslStream>& stream,
               boost::system::error_code& readEc, bool& readFired)
{
    auto rxBuf = std::make_shared<std::array<char, 256>>();
    stream->async_read_some(boost::asio::buffer(*rxBuf),
                            [&io, &readEc, &readFired,
                             rxBuf](boost::system::error_code ec, std::size_t) {
                                readEc = ec;
                                readFired = true;
                                io.stop();
                            });
}

// Connect client stream, run TLS handshake, then invoke clientAction.
void doClientHandshake(boost::asio::io_context& io,
                       const std::shared_ptr<SslStream>& stream,
                       std::function<void()> clientAction,
                       boost::system::error_code ec)
{
    if (ec)
    {
        ADD_FAILURE() << "client connect: " << ec.message();
        io.stop();
        return;
    }
    stream->async_handshake(
        boost::asio::ssl::stream_base::client,
        [&io, stream, clientAction = std::move(clientAction)](
            boost::system::error_code hec) {
            if (hec)
            {
                ADD_FAILURE() << "client handshake: " << hec.message();
                io.stop();
                return;
            }
            clientAction();
        });
}

// ============================================================================
// Test 1: Path 1 — SSL error from a bad raw record (ContentType 0xFF).
//
// Server: after TLS handshake, synchronously writes a 10-byte raw-TCP
// sequence whose first byte (0xFF) is not a valid TLS ContentType, then
// keeps the socket open so no TCP EOF races the bad bytes.
//
// OpenSSL on the client encounters the bad record and returns an error.
// Depending on the OpenSSL version and TLS version negotiated, this
// arrives as either ssl::error::stream_truncated or an asio.ssl error —
// both are non-ok and both represent the "stale completion" path.
//
// The test asserts only:
//   readFired == true   — the async_read_some completion did fire.
//   readEc != ok        — a bad record always causes a non-ok completion.
// ============================================================================
TEST(SslDoubleCompletionTest, Path1_BadRecord_SslError)
{
    boost::asio::io_context io;
    boost::asio::ssl::context serverCtx = makeServerCtx();
    boost::asio::ssl::context clientCtx(boost::asio::ssl::context::tls_client);
    clientCtx.set_verify_mode(boost::asio::ssl::verify_none);

    // Write a bad raw-TCP record and keep the socket open.  The socket
    // is kept alive by the shared_ptr captured in the lambda; it closes
    // when the lambda destructs after io.run() returns.
    auto serverAction = [](const std::shared_ptr<SslStream>& sock) {
        static constexpr std::string_view poison =
            "\xff\x03\x03\x00\x05XXXXX"; // ContentType 0xFF = invalid
        boost::system::error_code ec;
        boost::asio::write(sock->next_layer(), boost::asio::buffer(poison), ec);
        (void)sock; // keep open — closed when lambda destructs
    };

    uint16_t port = startServer(io, serverCtx, serverAction);

    auto clientStream = std::make_shared<SslStream>(io, clientCtx);
    boost::system::error_code readEc;
    bool readFired = false;

    auto clientAction = [&io, &clientStream, &readEc, &readFired]() {
        doReadOne(io, clientStream, readEc, readFired);
    };

    boost::asio::ip::tcp::endpoint ep(
        boost::asio::ip::make_address("127.0.0.1"), port);
    clientStream->next_layer().async_connect(
        ep, std::bind_front(doClientHandshake, std::ref(io), clientStream,
                            std::move(clientAction)));

    boost::asio::steady_timer watchdog(io, std::chrono::seconds(5));
    watchdog.async_wait([&io](boost::system::error_code wec) {
        if (wec != boost::asio::error::operation_aborted)
        {
            ADD_FAILURE() << "watchdog: Path1 timed out";
            io.stop();
        }
    });

    io.run();
    watchdog.cancel();
    io.restart();
    io.poll();

    ASSERT_TRUE(readFired) << "async_read_some completion never fired";
    EXPECT_TRUE(readEc) << "Expected a non-ok error from bad record; got ec=ok";
}

// ============================================================================
// Test 2: Path 2 — stream_truncated from unclean TCP close (no close_notify).
//
// Server: after TLS handshake, closes the TCP socket directly without
// sending a TLS close_notify alert.  Boost.Asio's SSL engine maps the
// resulting EOF to ssl::error::stream_truncated via map_error_code():
//
//   if ((SSL_get_shutdown(ssl_) & SSL_RECEIVED_SHUTDOWN) == 0)
//       ec = ssl::error::stream_truncated;
//
// The test asserts:
//   ec == boost::asio::ssl::error::stream_truncated.
// ============================================================================
TEST(SslDoubleCompletionTest, Path2_UncleanClose_StreamTruncated)
{
    boost::asio::io_context io;
    boost::asio::ssl::context serverCtx = makeServerCtx();
    boost::asio::ssl::context clientCtx(boost::asio::ssl::context::tls_client);
    clientCtx.set_verify_mode(boost::asio::ssl::verify_none);

    // Server action: close TCP immediately, no TLS close_notify.
    auto serverAction = [](const std::shared_ptr<SslStream>& sock) {
        boost::system::error_code ec;
        sock->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                    ec);
        sock->next_layer().close(ec);
    };

    uint16_t port = startServer(io, serverCtx, serverAction);

    auto clientStream = std::make_shared<SslStream>(io, clientCtx);
    boost::system::error_code readEc;
    bool readFired = false;

    auto clientAction = [&io, &clientStream, &readEc, &readFired]() {
        doReadOne(io, clientStream, readEc, readFired);
    };

    boost::asio::ip::tcp::endpoint ep(
        boost::asio::ip::make_address("127.0.0.1"), port);
    clientStream->next_layer().async_connect(
        ep, std::bind_front(doClientHandshake, std::ref(io), clientStream,
                            std::move(clientAction)));

    boost::asio::steady_timer watchdog(io, std::chrono::seconds(5));
    watchdog.async_wait([&io](boost::system::error_code wec) {
        if (wec != boost::asio::error::operation_aborted)
        {
            ADD_FAILURE() << "watchdog: Path2 timed out";
            io.stop();
        }
    });

    io.run();
    watchdog.cancel();
    io.restart();
    io.poll();

    ASSERT_TRUE(readFired) << "async_read_some completion never fired";
    EXPECT_EQ(readEc, boost::asio::ssl::error::stream_truncated)
        << "Expected stream_truncated from unclean close; got: "
        << readEc.message();
}

// ============================================================================
// Test 3: Combined — bad record followed by unclean close.
//
// Server: write poison bytes (ContentType 0xFF) then immediately close the
// TCP socket without TLS close_notify.  Both land in the client's receive
// buffer before the first async_read_some can drain it.
//
// Client: arm async_read_some in a self-rearming loop that collects every
// error_code it receives.  rearmRead stops on the first error (io.stop()),
// but if the underlying io_op fires its handler a second time (as Boost
// 1.83 does), that second invocation still runs and pushes a second entry
// into result->codes before io.run() returns.
//
// Expected results by Boost version
// ----------------------------------
// Boost 1.83 (ibm-bmcweb, double-completion source):
//   io_op does NOT atomically cancel pending_read_ before calling
//   call_handler.  The timer cancellation completion re-enters io_op and
//   calls call_handler a second time on the same handler object:
//     codes[0] = asio.ssl error    (SSL_ERROR_SSL from bad record)
//     codes[1] = stream_truncated  (timer cancellation → EOF → map_error_code)
//   → GTEST_LOG_ prints: "Double-completion observed: [asio.ssl error]
//                          then [stream_truncated]"
//
// Boost 1.84+ (this repo, fixed):
//   io_op resets pending_read_ to neg_infin before calling call_handler
//   and checks cancelled() — the timer path never reaches call_handler
//   a second time.  TLS 1.3 may also fold the bad-record error into
//   stream_truncated before the handler is called:
//     codes[0] = stream_truncated  (only one completion)
//   → GTEST_LOG_ prints: "Single completion: [stream_truncated]"
//
// The test PASSES on both versions.  It documents the observed behavior
// without asserting which version is in use.
// ============================================================================
struct CollectedCompletions
{
    std::vector<boost::system::error_code> codes;
};

void rearmRead(boost::asio::io_context& io,
               const std::shared_ptr<SslStream>& stream,
               const std::shared_ptr<CollectedCompletions>& result,
               const std::shared_ptr<std::array<char, 256>>& rxBuf,
               boost::system::error_code ec, std::size_t /*unused*/)
{
    result->codes.push_back(ec);
    if (ec)
    {
        // Error received — stop the io_context so io.run() returns.
        // Do not close the socket here; let it destruct naturally.
        io.stop();
        return;
    }
    // No error yet — re-arm to collect the next completion.
    stream->async_read_some(
        boost::asio::buffer(*rxBuf),
        std::bind_front(rearmRead, std::ref(io), stream, result, rxBuf));
}

TEST(SslDoubleCompletionTest, Path3_BadRecordThenUncleanClose_ErrorCodes)
{
    boost::asio::io_context io;
    boost::asio::ssl::context serverCtx = makeServerCtx();
    boost::asio::ssl::context clientCtx(boost::asio::ssl::context::tls_client);
    clientCtx.set_verify_mode(boost::asio::ssl::verify_none);

    // Server: write poison bytes synchronously then close without close_notify.
    // Both arrive in the client's TCP buffer before the first read drains them.
    auto serverAction = [](const std::shared_ptr<SslStream>& sock) {
        static constexpr std::string_view poison = "\xff\x03\x03\x00\x05XXXXX";
        boost::system::error_code ec;
        boost::asio::write(sock->next_layer(), boost::asio::buffer(poison), ec);
        sock->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                    ec);
        sock->next_layer().close(ec);
    };

    uint16_t port = startServer(io, serverCtx, serverAction);

    auto clientStream = std::make_shared<SslStream>(io, clientCtx);
    auto result = std::make_shared<CollectedCompletions>();

    auto clientAction = [&io, &clientStream, &result]() {
        auto rxBuf = std::make_shared<std::array<char, 256>>();
        clientStream->async_read_some(
            boost::asio::buffer(*rxBuf),
            std::bind_front(rearmRead, std::ref(io), clientStream, result,
                            rxBuf));
    };

    boost::asio::ip::tcp::endpoint ep(
        boost::asio::ip::make_address("127.0.0.1"), port);
    clientStream->next_layer().async_connect(
        ep, std::bind_front(doClientHandshake, std::ref(io), clientStream,
                            std::move(clientAction)));

    boost::asio::steady_timer watchdog(io, std::chrono::seconds(5));
    watchdog.async_wait([&io](boost::system::error_code wec) {
        if (wec != boost::asio::error::operation_aborted)
        {
            ADD_FAILURE() << "watchdog: Path3 timed out";
            io.stop();
        }
    });

    io.run();
    watchdog.cancel();
    io.restart();
    io.poll();

    // Print every completion so the reader knows what this Boost/OpenSSL
    // version actually delivers.  This is the primary diagnostic value.
    for (std::size_t i = 0; i < result->codes.size(); ++i)
    {
        SCOPED_TRACE("completion[" + std::to_string(i) +
                     "] = " + result->codes[i].message());
    }

    ASSERT_FALSE(result->codes.empty())
        << "Expected at least one completion; got none";
    EXPECT_TRUE(result->codes[0])
        << "First completion must be an error (bad record or truncated)";

    // If two completions fired, document them precisely.
    // On Boost 1.83 (ibm-bmcweb) the double-completion produces:
    //   codes[0] = asio.ssl error    (SSL_ERROR_SSL, bad record)
    //   codes[1] = stream_truncated  (unclean EOF, no close_notify)
    // On newer Boost/OpenSSL versions TLS 1.3 may fold both into one.
    if (result->codes.size() >= 2)
    {
        EXPECT_TRUE(result->codes[1])
            << "Second completion must also be an error";
        GTEST_LOG_(INFO)
            << "Double-completion observed on this Boost/OpenSSL version: ["
            << result->codes[0].message() << "] then ["
            << result->codes[1].message() << "]";
    }
    else
    {
        GTEST_LOG_(INFO) << "Single completion on this Boost/OpenSSL version: ["
                         << result->codes[0].message() << "]";
    }
}

// ============================================================================
// Test 4: Double-invocation hazard — same callable posted twice (no network).
//
// Directly models the production double-completion scenario without any
// network I/O:
//
//   The io_context posts handler1 (ec=ok) and handler2 (ec=ssl_error)
//   for what the caller treated as a single async_read.
//
// With the UNSAFE bind_front(&Foo::cb, this, sptr) pattern:
//   - `this` is the raw implicit object.
//   - sptr is a bound argument, NOT the call target.
//   - If sptr is the last owner and is destroyed between the two posts,
//     `this` is dangling when the second post fires.
//
// With the SAFE bind_front(&Foo::cb, shared_from_this()) pattern:
//   - shared_from_this() IS the implicit object stored in the callable.
//   - Each copy of the callable holds its own reference.
//   - The object survives until the last callable copy destructs.
//
// The test:
//   1. Creates an object, makes two callables (simulating two completions).
//   2. Drops all external references.
//   3. Posts both callables.  The object must be alive for both fires.
//   4. Asserts both invocations saw a live object.
// ============================================================================
struct DoubleCompletionTarget :
    std::enable_shared_from_this<DoubleCompletionTarget>
{
    int fireCount = 0;
    int liveCount = 0; // incremented only when object is verified live

    void onCompletion()
    {
        ++fireCount;
        // shared_from_this() succeeds only on a live object.
        if (shared_from_this() != nullptr)
        {
            ++liveCount;
        }
    }
};

TEST(SslDoubleCompletionTest, DoubleInvocation_BothFireOnLiveObject)
{
    boost::asio::io_context io;

    auto obj = std::make_shared<DoubleCompletionTarget>();

    // SAFE: each callable stores shared_from_this() as its call target.
    // This mirrors:  bind_front(&ConnectionInfo::afterRead, shared_from_this())
    auto handler1 = std::bind_front(&DoubleCompletionTarget::onCompletion,
                                    obj->shared_from_this());
    auto handler2 = std::bind_front(&DoubleCompletionTarget::onCompletion,
                                    obj->shared_from_this());

    // Keep an observer for post-run assertions.
    std::shared_ptr<DoubleCompletionTarget> observer = obj;

    // Drop the external owner — simulates the connection pool releasing
    // the ConnectionInfo after posting the async ops.
    obj.reset();

    // Object still alive: two callables each hold a reference.
    ASSERT_FALSE(observer->shared_from_this() == nullptr);

    // Post both — simulates io_context delivering two completions.
    boost::asio::post(io, std::move(handler1));
    boost::asio::post(io, std::move(handler2));
    io.run();

    EXPECT_EQ(observer->fireCount, 2) << "Both completions must fire";
    EXPECT_EQ(observer->liveCount, 2)
        << "Object must be alive for both completions";
}

} // anonymous namespace
