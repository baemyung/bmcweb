// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
//
// HTTP Client Race Condition Test
// This test verifies that the HTTP client handles rapid client destruction
// without crashing due to race conditions in async callback execution.

#include "http/http_client.hpp"
#include "ssl_key_handler.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/url/url_view.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace crow
{

class HttpClientRaceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        clientsCreated = 0;
        clientsDestroyed = 0;
    }

    void TearDown() override {}

    static std::atomic<int> clientsCreated;
    static std::atomic<int> clientsDestroyed;
};

std::atomic<int> HttpClientRaceTest::clientsCreated{0};
std::atomic<int> HttpClientRaceTest::clientsDestroyed{0};

// Test: Rapid client creation and destruction with pending async operations
// This test creates clients, sends requests to unreachable IPs, and immediately
// destroys them to trigger race conditions in callback execution.
TEST_F(HttpClientRaceTest, RapidClientDestructionWithPendingOperations)
{
    boost::asio::io_context ioc;

    // Use work guard to keep io_context alive
    auto work = boost::asio::make_work_guard(ioc);

    // Run io_context in a separate thread
    std::thread ioThread([&ioc]() { ioc.run(); });

    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 1;
    policy->retryIntervalSecs = std::chrono::seconds(1);
    policy->maxConnections = 2;

    // Create and destroy clients rapidly (reduced count for CI)
    const int iterations = 50;
    for (int i = 0; i < iterations; i++)
    {
        // Create client
        auto client = std::make_unique<crow::HttpClient>(ioc, policy);
        clientsCreated++;

        boost::beast::http::fields headers;
        headers.set(boost::beast::http::field::host, "192.0.2.1");

        // Send multiple requests to unreachable IPs
        // This creates multiple ConnectionInfo objects with active async
        // operations
        for (int j = 0; j < 3; j++)
        {
            std::string ip = "192.0.2." + std::to_string((i * 3 + j) % 254 + 1);
            try
            {
                client->sendData(
                    "test data",
                    boost::urls::url_view("http://" + ip + ":8080/events"),
                    ensuressl::VerifyCertificate::NoVerify, headers,
                    boost::beast::http::verb::post);
            }
            catch (const std::exception& /*e*/)
            {
                // Ignore exceptions from sendData
            }
        }

        // Destroy immediately - this is the critical race condition trigger
        // The async operations (resolve, connect, timers) are still in flight
        client.reset();
        clientsDestroyed++;
    }

    // Allow some time for pending operations to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop io_context
    work.reset();
    ioc.stop();

    // Wait for io thread with timeout
    if (ioThread.joinable())
    {
        ioThread.join();
    }

    // Verify all clients were created and destroyed
    EXPECT_EQ(clientsCreated.load(), iterations);
    EXPECT_EQ(clientsDestroyed.load(), iterations);

    // If we reach here without crashing, the test passes
    // The vulnerability would cause SIGABRT or SIGSEGV before this point
}

// Test: Multi-threaded client creation and destruction
// This test creates clients from multiple threads simultaneously to increase
// the likelihood of triggering race conditions.
TEST_F(HttpClientRaceTest, MultiThreadedClientDestruction)
{
    boost::asio::io_context ioc;
    auto work = boost::asio::make_work_guard(ioc);

    // Run io_context in a separate thread
    std::thread ioThread([&ioc]() { ioc.run(); });

    const int numThreads = 2;
    const int iterationsPerThread = 25;
    std::vector<std::thread> threads;

    for (int t = 0; t < numThreads; t++)
    {
        threads.emplace_back([&ioc, iterationsPerThread, t]() {
            auto policy = std::make_shared<crow::ConnectionPolicy>();
            policy->maxRetryAttempts = 1;
            policy->retryIntervalSecs = std::chrono::seconds(1);
            policy->maxConnections = 2;

            for (int i = 0; i < iterationsPerThread; i++)
            {
                auto client = std::make_unique<crow::HttpClient>(ioc, policy);
                clientsCreated++;

                boost::beast::http::fields headers;
                headers.set(boost::beast::http::field::host, "192.0.2.1");

                // Send to multiple IPs
                for (int j = 0; j < 2; j++)
                {
                    std::string ip =
                        "192.0.2." +
                        std::to_string((t * 100 + i * 2 + j) % 254 + 1);
                    try
                    {
                        client->sendData(
                            "test",
                            boost::urls::url_view(
                                "http://" + ip + ":8080/events"),
                            ensuressl::VerifyCertificate::NoVerify, headers,
                            boost::beast::http::verb::post);
                    }
                    catch (...)
                    {
                        // Ignore exceptions
                    }
                }

                // Immediate destruction
                client.reset();
                clientsDestroyed++;
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads)
    {
        thread.join();
    }

    // Allow some time for pending operations
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop io_context
    work.reset();
    ioc.stop();

    if (ioThread.joinable())
    {
        ioThread.join();
    }

    // Verify counts
    EXPECT_EQ(clientsCreated.load(), numThreads * iterationsPerThread);
    EXPECT_EQ(clientsDestroyed.load(), numThreads * iterationsPerThread);
}

} // namespace crow

// Made with Bob
