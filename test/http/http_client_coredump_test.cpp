// Unit test to reproduce ConnectionInfo coredump caused by use-after-free
// This test exploits the race condition where ConnectionInfo is destroyed
// while async operations (resolve, connect, read) are still pending.

#include "http_client.hpp"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

// Test fixture for HTTP client coredump tests
class HttpClientCoredumpTest : public ::testing::Test
{
  protected:
    boost::asio::io_context ioc;
};

// Test 1: Destroy ConnectionPool immediately after starting async operations
// This is the most likely scenario to trigger the coredump
TEST_F(HttpClientCoredumpTest, DestroyPoolDuringAsyncResolve)
{
    std::shared_ptr<crow::ConnectionPool> pool;
    bool callbackInvoked = false;

    // Create ConnectionPool with a policy
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0; // No retries to make test faster

    // Create connection pool to a non-existent host
    pool = std::make_shared<crow::ConnectionPool>(
        ioc, "test-sub-1", policy,
        boost::urls::url_view("http://nonexistent.invalid.test:9999/"),
        crow::ensuressl::VerifyCertificate::Verify);

    // Send a request that will trigger async operations
    pool->sendData(
        "", boost::urls::url_view("http://nonexistent.invalid.test:9999/"),
        boost::beast::http::fields(), boost::beast::http::verb::get,
        [&callbackInvoked](crow::Response& /*res*/) {
            callbackInvoked = true;
            // If we reach here with a destroyed ConnectionInfo, we'll crash
        });

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

    // CRITICAL: Destroy ConnectionPool while async operation is pending
    // This should trigger use-after-free if the bug exists
    pool.reset();

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

// Test 2: Destroy ConnectionPool during connection attempt
TEST_F(HttpClientCoredumpTest, DestroyPoolDuringConnect)
{
    std::shared_ptr<crow::ConnectionPool> pool;

    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    // Use localhost with a port that's likely closed
    pool = std::make_shared<crow::ConnectionPool>(
        ioc, "test-sub-2", policy,
        boost::urls::url_view("http://127.0.0.1:19999/"),
        crow::ensuressl::VerifyCertificate::Verify);

    pool->sendData("", boost::urls::url_view("http://127.0.0.1:19999/"),
                   boost::beast::http::fields(), boost::beast::http::verb::get,
                   [](crow::Response& /*res*/) {
                       // Callback that would crash if ConnectionInfo is
                       // destroyed
                   });

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
    pool.reset();

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

    // Create and destroy multiple connection pools rapidly
    for (int iteration = 0; iteration < 10; ++iteration)
    {
        auto pool = std::make_shared<crow::ConnectionPool>(
            ioc, "test-sub-rapid", policy,
            boost::urls::url_view("http://nonexistent.test:9999/"),
            crow::ensuressl::VerifyCertificate::Verify);

        pool->sendData(
            "", boost::urls::url_view("http://nonexistent.test:9999/"),
            boost::beast::http::fields(), boost::beast::http::verb::get,
            [](crow::Response& /*res*/) {
                // Would crash if called after destruction
            });

        // Minimal processing before destruction
        ioc.poll();

        // Immediate destruction
        pool.reset();

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

// Test 4: Destroy ConnectionPool with callback that accesses response
TEST_F(HttpClientCoredumpTest, DestroyWithResponseAccess)
{
    std::shared_ptr<crow::ConnectionPool> pool;
    bool crashDetected = false;

    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 1;

    pool = std::make_shared<crow::ConnectionPool>(
        ioc, "test-sub-3", policy,
        boost::urls::url_view("http://invalid.test:9999/"),
        crow::ensuressl::VerifyCertificate::Verify);

    // Callback that accesses response object
    pool->sendData("", boost::urls::url_view("http://invalid.test:9999/"),
                   boost::beast::http::fields(), boost::beast::http::verb::get,
                   [&crashDetected](crow::Response& res) {
                       try
                       {
                           // Simulate accessing response
                           res.result(boost::beast::http::status::ok);
                       }
                       catch (...)
                       {
                           crashDetected = true;
                       }
                   });

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
    pool.reset();

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

// Test 5: Stress test with multiple concurrent connection pools
TEST_F(HttpClientCoredumpTest, MultiplePoolsStressTest)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    std::vector<std::shared_ptr<crow::ConnectionPool>> pools;

    // Create multiple connection pools
    for (int i = 0; i < 5; ++i)
    {
        auto pool = std::make_shared<crow::ConnectionPool>(
            ioc, "test-sub-multi", policy,
            boost::urls::url_view(
                "http://nonexistent" + std::to_string(i) + ".test:9999/"),
            crow::ensuressl::VerifyCertificate::Verify);

        pool->sendData(
            "",
            boost::urls::url_view(
                "http://nonexistent" + std::to_string(i) + ".test:9999/"),
            boost::beast::http::fields(), boost::beast::http::verb::get,
            [](crow::Response& /*res*/) {
                // Would crash if called after destruction
            });

        pools.push_back(pool);
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

    // Destroy all pools while operations are pending
    pools.clear();

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

    SUCCEED() << "Multiple pools stress test completed without crash";
}

} // namespace

// Made with Bob
