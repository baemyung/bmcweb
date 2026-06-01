// Unit test to reproduce ConnectionInfo coredump caused by use-after-free
// This test exploits the race condition where ConnectionInfo is destroyed
// while async operations (resolve, connect, read) are still pending.

#include "http_client.hpp"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <memory>
#include <string>
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
// This test verifies that destroying the client doesn't cause crashes
TEST_F(HttpClientCoredumpTest, DestroyClientDuringAsyncResolve)
{
    bool callbackInvoked = false;

    {
        // Create HttpClient with a policy
        auto policy = std::make_shared<crow::ConnectionPolicy>();
        policy->maxRetryAttempts = 0; // No retries to make test faster

        auto client = std::make_shared<crow::HttpClient>(ioc, policy);

        // Send a request that will trigger async operations to non-existent
        // host
        client->sendDataWithCallback(
            "", boost::urls::url_view("http://nonexistent.invalid.test:9999/"),
            ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
            boost::beast::http::verb::get,
            [&callbackInvoked](crow::Response& /*res*/) {
                callbackInvoked = true;
            });

        // Process a few events to let async_resolve start
        ioc.poll();
        ioc.poll();

        // Client goes out of scope here and is destroyed
    }

    // Process any remaining events after client destruction
    // If the bug exists, this may cause a crash
    ioc.poll();
    ioc.restart();
    ioc.poll();

    // If we reach here without crashing, the test passes
    EXPECT_FALSE(callbackInvoked)
        << "Callback should not be invoked after client destruction";
}

// Test 2: Destroy HttpClient during connection attempt
TEST_F(HttpClientCoredumpTest, DestroyClientDuringConnect)
{
    {
        auto policy = std::make_shared<crow::ConnectionPolicy>();
        policy->maxRetryAttempts = 0;

        auto client = std::make_shared<crow::HttpClient>(ioc, policy);

        // Use localhost with a port that's likely closed
        client->sendDataWithCallback(
            "", boost::urls::url_view("http://127.0.0.1:19999/"),
            ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
            boost::beast::http::verb::get, [](crow::Response& /*res*/) {});

        // Let some async operations start
        ioc.poll();
        ioc.poll();

        // Client destroyed here
    }

    // Process remaining events
    ioc.poll();
    ioc.restart();
    ioc.poll();

    SUCCEED() << "No crash occurred during connection destruction";
}

// Test 3: Rapid creation and destruction
TEST_F(HttpClientCoredumpTest, RapidCreateDestroy)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    // Create and destroy multiple clients rapidly
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        {
            auto client = std::make_shared<crow::HttpClient>(ioc, policy);

            client->sendDataWithCallback(
                "", boost::urls::url_view("http://nonexistent.test:9999/"),
                ensuressl::VerifyCertificate::Verify,
                boost::beast::http::fields(), boost::beast::http::verb::get,
                [](crow::Response& /*res*/) {});

            ioc.poll();
            // Client destroyed here
        }

        ioc.poll();
        ioc.restart();
    }

    SUCCEED() << "Rapid create/destroy completed without crash";
}

// Test 4: Multiple concurrent clients
TEST_F(HttpClientCoredumpTest, MultipleClientsStressTest)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    {
        std::vector<std::shared_ptr<crow::HttpClient>> clients;

        // Create multiple clients
        for (int i = 0; i < 3; ++i)
        {
            auto client = std::make_shared<crow::HttpClient>(ioc, policy);

            client->sendDataWithCallback(
                "",
                boost::urls::url_view(
                    "http://test" + std::to_string(i) + ".invalid:9999/"),
                ensuressl::VerifyCertificate::Verify,
                boost::beast::http::fields(), boost::beast::http::verb::get,
                [](crow::Response& /*res*/) {});

            clients.push_back(client);
        }

        // Process some events
        ioc.poll();
        ioc.poll();

        // All clients destroyed here
    }

    // Process remaining events
    ioc.poll();
    ioc.restart();
    ioc.poll();

    SUCCEED() << "Multiple clients test completed without crash";
}

} // namespace

// Made with Bob
