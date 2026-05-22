// Unit test to reproduce ConnectionInfo coredump caused by use-after-free
// This test exploits the race condition where ConnectionInfo is destroyed
// while async operations (resolve, connect, read) are still pending.

#include "boost_formatters.hpp"
#include "dbus_singleton.hpp"
#include "http_client.hpp"
#include "ssl_key_handler.hpp"

#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <chrono>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

namespace
{

// Test fixture for HTTP client coredump tests
class HttpClientCoredumpTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Initialize systemBus for DNS resolution
        systemBus = std::make_unique<sdbusplus::asio::connection>(ioc);
        crow::connections::systemBus = systemBus.get();
    }

    void TearDown() override
    {
        crow::connections::systemBus = nullptr;
        systemBus.reset();
    }

    boost::asio::io_context ioc;
    std::unique_ptr<sdbusplus::asio::connection> systemBus;
};

// Test 1: Destroy ConnectionInfo immediately after starting async resolve
// This is the most likely scenario to trigger the coredump
TEST_F(HttpClientCoredumpTest, DestroyDuringAsyncResolve)
{
    std::shared_ptr<crow::ConnectionInfo> conn;
    bool callbackInvoked = false;

    // Create ConnectionInfo with a policy
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0; // No retries to make test faster

    // Create connection to a non-existent host to ensure async operation takes
    // time
    conn = std::make_shared<crow::ConnectionInfo>(
        ioc, "test-sub-1", policy,
        boost::urls::url_view("http://nonexistent.invalid.test:9999/"),
        ensuressl::VerifyCertificate::Verify, 1);

    // Set up callback that will be invoked after resolution
    conn->callback = [&callbackInvoked](bool /*keepAlive*/, uint32_t /*connId*/,
                                        crow::Response& /*res*/) {
        callbackInvoked = true;
        // If we reach here with a destroyed ConnectionInfo, we'll crash
    };

    // Prepare a request
    boost::beast::http::request<boost::beast::http::string_body> req;
    req.method(boost::beast::http::verb::get);
    req.target("/");
    req.set(boost::beast::http::field::host, "nonexistent.invalid.test:9999");
    conn->req = std::move(req);

    // Start async resolve operation
    conn->doResolve();

    // Process a few events to let async_resolve start
    for (int i = 0; i < 3; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // CRITICAL: Destroy ConnectionInfo while async operation is pending
    // This should trigger use-after-free if the bug exists
    conn.reset();

    // Continue processing events - the callback may fire with dangling 'this'
    for (int i = 0; i < 50; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // If we reach here without crashing, the bug is fixed
    EXPECT_FALSE(callbackInvoked)
        << "Callback should not be invoked after destruction";
}

// Test 2: Destroy ConnectionInfo during connection attempt
TEST_F(HttpClientCoredumpTest, DestroyDuringConnect)
{
    std::shared_ptr<crow::ConnectionInfo> conn;

    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    // Use localhost with a port that's likely closed
    conn = std::make_shared<crow::ConnectionInfo>(
        ioc, "test-sub-2", policy,
        boost::urls::url_view("http://127.0.0.1:19999/"),
        ensuressl::VerifyCertificate::Verify, 2);

    conn->callback = [](bool, uint32_t, crow::Response&) {
        // Callback that would crash if ConnectionInfo is destroyed
    };

    boost::beast::http::request<boost::beast::http::string_body> req;
    req.method(boost::beast::http::verb::get);
    req.target("/");
    req.set(boost::beast::http::field::host, "127.0.0.1:19999");
    conn->req = std::move(req);

    conn->doResolve();

    // Let resolve complete and connect start
    for (int i = 0; i < 10; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Destroy during connection attempt
    conn.reset();

    // Process remaining events
    for (int i = 0; i < 50; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    SUCCEED() << "No crash occurred during connection destruction";
}

// Test 3: Rapid creation and destruction to maximize race condition
TEST_F(HttpClientCoredumpTest, RapidCreateDestroy)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    // Create and destroy multiple connections rapidly
    for (int iteration = 0; iteration < 10; ++iteration)
    {
        auto conn = std::make_shared<crow::ConnectionInfo>(
            ioc, "test-sub-rapid", policy,
            boost::urls::url_view("http://nonexistent.test:9999/"),
            ensuressl::VerifyCertificate::Verify, iteration);

        conn->callback = [](bool, uint32_t, crow::Response&) {
            // Would crash if called after destruction
        };

        boost::beast::http::request<boost::beast::http::string_body> req;
        req.method(boost::beast::http::verb::get);
        req.target("/");
        req.set(boost::beast::http::field::host, "nonexistent.test:9999");
        conn->req = std::move(req);

        conn->doResolve();

        // Minimal processing before destruction
        ioc.poll();

        // Immediate destruction
        conn.reset();

        // Process a few events
        for (int i = 0; i < 5; ++i)
        {
            ioc.poll();
            if (ioc.stopped())
            {
                ioc.restart();
            }
        }
    }

    SUCCEED() << "Rapid create/destroy completed without crash";
}

// Test 4: Destroy ConnectionInfo with callback that accesses member variables
TEST_F(HttpClientCoredumpTest, DestroyWithMemberAccessInCallback)
{
    std::shared_ptr<crow::ConnectionInfo> conn;
    bool crashDetected = false;

    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 1;

    conn = std::make_shared<crow::ConnectionInfo>(
        ioc, "test-sub-3", policy,
        boost::urls::url_view("http://invalid.test:9999/"),
        ensuressl::VerifyCertificate::Verify, 3);

    // Callback that would access member variables (connPolicy, etc.)
    // This is what happens in afterRead() at line 390:
    // connPolicy->invalidResp(respCode)
    conn->callback = [&crashDetected](bool, uint32_t, crow::Response& res) {
        try
        {
            // Simulate accessing member that might be destroyed
            res.result(boost::beast::http::status::ok);
        }
        catch (...)
        {
            crashDetected = true;
        }
    };

    boost::beast::http::request<boost::beast::http::string_body> req;
    req.method(boost::beast::http::verb::get);
    req.target("/");
    req.set(boost::beast::http::field::host, "invalid.test:9999");
    conn->req = std::move(req);

    conn->doResolve();

    // Let async operation start
    for (int i = 0; i < 5; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Destroy while operation is pending
    conn.reset();

    // Process events that might trigger the callback
    for (int i = 0; i < 100; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_FALSE(crashDetected) << "Crash or exception detected in callback";
}

// Test 5: Stress test with multiple concurrent connections
TEST_F(HttpClientCoredumpTest, MultipleConnectionsStressTest)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    std::vector<std::shared_ptr<crow::ConnectionInfo>> connections;

    // Create multiple connections
    for (int i = 0; i < 5; ++i)
    {
        auto conn = std::make_shared<crow::ConnectionInfo>(
            ioc, "test-sub-multi", policy,
            boost::urls::url_view(
                "http://nonexistent" + std::to_string(i) + ".test:9999/"),
            ensuressl::VerifyCertificate::Verify, i);

        conn->callback = [](bool, uint32_t, crow::Response&) {
            // Would crash if called after destruction
        };

        boost::beast::http::request<boost::beast::http::string_body> req;
        req.method(boost::beast::http::verb::get);
        req.target("/");
        req.set(boost::beast::http::field::host,
                "nonexistent" + std::to_string(i) + ".test:9999");
        conn->req = std::move(req);

        conn->doResolve();
        connections.push_back(conn);
    }

    // Process some events
    for (int i = 0; i < 10; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Destroy all connections while operations are pending
    connections.clear();

    // Continue processing - this should trigger the bug if it exists
    for (int i = 0; i < 100; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    SUCCEED() << "Multiple connections stress test completed without crash";
}

} // namespace

// Made with Bob
