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

// Test 1: Destroy HttpClient immediately after starting async operations
// This is the most likely scenario to trigger the coredump
TEST_F(HttpClientCoredumpTest, DestroyClientDuringAsyncResolve)
{
    std::shared_ptr<crow::HttpClient> client;
    bool callbackInvoked = false;

    // Create HttpClient with a policy
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0; // No retries to make test faster

    client = std::make_shared<crow::HttpClient>(ioc, policy);

    // Send a request that will trigger async operations to non-existent host
    client->sendDataWithCallback(
        "", boost::urls::url_view("http://nonexistent.invalid.test:9999/"),
        ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
        boost::beast::http::verb::get,
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
            break;
        }
    }

    // CRITICAL: Destroy HttpClient while async operation is pending
    // This should trigger use-after-free if the bug exists
    client.reset();

    // Continue processing events - the callback may fire with dangling 'this'
    // Limit iterations to prevent test timeout
    for (int i = 0; i < 20; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            break;
        }
    }

    // If we reach here without crashing, the bug is fixed
    EXPECT_FALSE(callbackInvoked)
        << "Callback should not be invoked after destruction";
}

// Test 2: Destroy HttpClient during connection attempt
TEST_F(HttpClientCoredumpTest, DestroyClientDuringConnect)
{
    std::shared_ptr<crow::HttpClient> client;

    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    client = std::make_shared<crow::HttpClient>(ioc, policy);

    // Use localhost with a port that's likely closed
    client->sendDataWithCallback(
        "", boost::urls::url_view("http://127.0.0.1:19999/"),
        ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
        boost::beast::http::verb::get, [](crow::Response& /*res*/) {
            // Callback that would crash if ConnectionInfo is destroyed
        });

    // Let resolve complete and connect start
    for (int i = 0; i < 5; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            break;
        }
    }

    // Destroy during connection attempt
    client.reset();

    // Process remaining events
    for (int i = 0; i < 20; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            break;
        }
    }

    SUCCEED() << "No crash occurred during connection destruction";
}

// Test 3: Rapid creation and destruction to maximize race condition
TEST_F(HttpClientCoredumpTest, RapidCreateDestroy)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    // Create and destroy multiple clients rapidly
    for (int iteration = 0; iteration < 5; ++iteration)
    {
        auto client = std::make_shared<crow::HttpClient>(ioc, policy);

        client->sendDataWithCallback(
            "", boost::urls::url_view("http://nonexistent.test:9999/"),
            ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
            boost::beast::http::verb::get, [](crow::Response& /*res*/) {
                // Would crash if called after destruction
            });

        // Minimal processing before destruction
        ioc.poll();

        // Immediate destruction
        client.reset();

        // Process a few events
        for (int i = 0; i < 3; ++i)
        {
            ioc.poll();
            if (ioc.stopped())
            {
                break;
            }
        }
    }

    SUCCEED() << "Rapid create/destroy completed without crash";
}

// Test 4: Destroy HttpClient with callback that accesses response
TEST_F(HttpClientCoredumpTest, DestroyWithResponseAccess)
{
    std::shared_ptr<crow::HttpClient> client;
    bool crashDetected = false;

    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    client = std::make_shared<crow::HttpClient>(ioc, policy);

    // Callback that accesses response object
    client->sendDataWithCallback(
        "", boost::urls::url_view("http://invalid.test:9999/"),
        ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
        boost::beast::http::verb::get, [&crashDetected](crow::Response& res) {
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
    for (int i = 0; i < 3; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            break;
        }
    }

    // Destroy while operation is pending
    client.reset();

    // Process events that might trigger the callback
    for (int i = 0; i < 20; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            break;
        }
    }

    EXPECT_FALSE(crashDetected) << "Crash or exception detected in callback";
}

// Test 5: Stress test with multiple concurrent clients
TEST_F(HttpClientCoredumpTest, MultipleClientsStressTest)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    std::vector<std::shared_ptr<crow::HttpClient>> clients;

    // Create multiple clients
    for (int i = 0; i < 3; ++i)
    {
        auto client = std::make_shared<crow::HttpClient>(ioc, policy);

        client->sendDataWithCallback(
            "",
            boost::urls::url_view(
                "http://nonexistent" + std::to_string(i) + ".test:9999/"),
            ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
            boost::beast::http::verb::get, [](crow::Response& /*res*/) {
                // Would crash if called after destruction
            });

        clients.push_back(client);
    }

    // Process some events
    for (int i = 0; i < 5; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            break;
        }
    }

    // Destroy all clients while operations are pending
    clients.clear();

    // Continue processing - this should trigger the bug if it exists
    for (int i = 0; i < 20; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            break;
        }
    }

    SUCCEED() << "Multiple clients stress test completed without crash";
}

} // namespace

// Made with Bob
