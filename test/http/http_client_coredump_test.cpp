// Unit test to reproduce ConnectionInfo coredump caused by use-after-free
// This test demonstrates the bug where ConnectionInfo is destroyed while
// async operations are pending, causing callbacks to fire with dangling
// pointers.

#include "http_client.hpp"
#include "http_response.hpp"
#include "ssl_key_handler.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/url/url_view.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace
{

// Test fixture for HTTP client coredump tests
class HttpClientCoredumpTest : public ::testing::Test
{
  protected:
    boost::asio::io_context ioc;
};

// Helper to run a simple HTTP server that accepts connections
class SimpleHttpServer
{
  public:
    explicit SimpleHttpServer(boost::asio::io_context& ioc) :
        acceptor(ioc,
                 boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0)),
        socket(ioc)
    {
        port = acceptor.local_endpoint().port();
    }

    void startAccept()
    {
        acceptor.async_accept(socket, [this](boost::system::error_code ec) {
            if (!ec)
            {
                // Connection accepted, close it immediately
                socket.close();
            }
        });
    }

    uint16_t getPort() const
    {
        return port;
    }

  private:
    boost::asio::ip::tcp::acceptor acceptor;
    boost::asio::ip::tcp::socket socket;
    uint16_t port;
};

// Test 1: Destroy client while DNS resolution completes
// This test uses a real local server to ensure async operations complete
TEST_F(HttpClientCoredumpTest, DestroyDuringAsyncOperation)
{
    // Start a local server
    SimpleHttpServer server(ioc);
    server.startAccept();

    std::atomic<bool> callbackInvoked{false};
    std::atomic<bool> clientDestroyed{false};

    {
        auto policy = std::make_shared<crow::ConnectionPolicy>();
        policy->maxRetryAttempts = 0;

        auto client = std::make_shared<crow::HttpClient>(ioc, policy);

        // Send request to local server
        std::string url =
            "http://127.0.0.1:" + std::to_string(server.getPort()) + "/";
        client->sendDataWithCallback(
            "", boost::urls::url_view(url),
            ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
            boost::beast::http::verb::get,
            [&callbackInvoked, &clientDestroyed](crow::Response& /*res*/) {
                callbackInvoked = true;
                // If client was destroyed and bug exists, this will crash
                if (clientDestroyed)
                {
                    // This is the dangerous scenario - callback after
                    // destruction
                    GTEST_FAIL()
                        << "Callback invoked after client destruction!";
                }
            });

        // Run one async operation to start the request
        ioc.run_one();

        // Mark that we're about to destroy the client
        clientDestroyed = true;

        // Client destroyed here - if bug exists, pending callbacks may crash
    }

    // Process any remaining events
    // With the bug, this may cause a crash
    // With the fix, callbacks should be safely cancelled
    while (ioc.poll_one() > 0)
    {
        // Keep processing
    }

    // If callback was invoked after destruction, test should have failed above
    SUCCEED() << "Test completed without crash";
}

// Test 2: Multiple rapid create/destroy cycles
TEST_F(HttpClientCoredumpTest, RapidCreateDestroyCycles)
{
    SimpleHttpServer server(ioc);
    server.startAccept();

    for (int i = 0; i < 5; ++i)
    {
        auto policy = std::make_shared<crow::ConnectionPolicy>();
        policy->maxRetryAttempts = 0;

        {
            auto client = std::make_shared<crow::HttpClient>(ioc, policy);

            std::string url =
                "http://127.0.0.1:" + std::to_string(server.getPort()) + "/";
            client->sendDataWithCallback(
                "", boost::urls::url_view(url),
                ensuressl::VerifyCertificate::Verify,
                boost::beast::http::fields(), boost::beast::http::verb::get,
                [](crow::Response& /*res*/) {});

            // Start the operation
            ioc.run_one();

            // Client destroyed immediately
        }

        // Process remaining events
        ioc.poll();
    }

    SUCCEED() << "Rapid cycles completed without crash";
}

// Test 3: Concurrent clients with destruction
TEST_F(HttpClientCoredumpTest, ConcurrentClientsDestruction)
{
    SimpleHttpServer server(ioc);
    server.startAccept();

    std::vector<std::shared_ptr<crow::HttpClient>> clients;
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    // Create multiple clients
    for (int i = 0; i < 3; ++i)
    {
        auto client = std::make_shared<crow::HttpClient>(ioc, policy);

        std::string url =
            "http://127.0.0.1:" + std::to_string(server.getPort()) + "/";
        client->sendDataWithCallback(
            "", boost::urls::url_view(url),
            ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
            boost::beast::http::verb::get, [](crow::Response& /*res*/) {});

        clients.push_back(client);
    }

    // Start operations
    ioc.run_one();

    // Destroy all clients
    clients.clear();

    // Process remaining events
    while (ioc.poll_one() > 0)
    {
        // Keep processing
    }

    SUCCEED() << "Concurrent clients test completed without crash";
}

// Test 4: Basic lifecycle test (should always pass)
TEST_F(HttpClientCoredumpTest, BasicLifecycleTest)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    auto client = std::make_shared<crow::HttpClient>(ioc, policy);

    // Just create and destroy
    client.reset();

    SUCCEED() << "Basic lifecycle test passed";
}

} // namespace

// Made with Bob
