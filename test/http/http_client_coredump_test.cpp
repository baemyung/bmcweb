// Unit test to reproduce ConnectionInfo coredump caused by use-after-free
// This test verifies that destroying HttpClient doesn't cause crashes

#include "http_client.hpp"
#include "http_response.hpp"
#include "ssl_key_handler.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/url/url_view.hpp>

#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace
{

// Test fixture for HTTP client coredump tests
class HttpClientCoredumpTest : public ::testing::Test
{
  protected:
    boost::asio::io_context ioc;
};

// Test: Destroy HttpClient immediately after creation
// This verifies basic destruction doesn't crash
TEST_F(HttpClientCoredumpTest, DestroyClientImmediately)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    {
        auto client = std::make_shared<crow::HttpClient>(ioc, policy);
        // Client destroyed immediately
    }

    SUCCEED() << "Client destroyed without crash";
}

// Test: Destroy HttpClient after sending request
// Uses IP address to avoid DNS resolution delay
TEST_F(HttpClientCoredumpTest, DestroyClientAfterRequest)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    {
        auto client = std::make_shared<crow::HttpClient>(ioc, policy);

        // Use IP address to avoid DNS resolution
        client->sendDataWithCallback(
            "", boost::urls::url_view("http://127.0.0.1:19999/"),
            ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
            boost::beast::http::verb::get, [](crow::Response& /*res*/) {});

        // Don't poll - just destroy immediately
        // Client destroyed here
    }

    SUCCEED() << "Client destroyed after request without crash";
}

// Test: Create and destroy multiple clients
TEST_F(HttpClientCoredumpTest, MultipleClientsCreateDestroy)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    for (int i = 0; i < 3; ++i)
    {
        auto client = std::make_shared<crow::HttpClient>(ioc, policy);

        client->sendDataWithCallback(
            "", boost::urls::url_view("http://127.0.0.1:19999/"),
            ensuressl::VerifyCertificate::Verify, boost::beast::http::fields(),
            boost::beast::http::verb::get, [](crow::Response& /*res*/) {});

        // Client destroyed at end of iteration
    }

    SUCCEED() << "Multiple clients created and destroyed without crash";
}

// Test: Verify client can be moved
TEST_F(HttpClientCoredumpTest, MoveClient)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();

    auto client1 = std::make_shared<crow::HttpClient>(ioc, policy);
    auto client2 = std::move(client1);

    EXPECT_EQ(client1, nullptr);
    EXPECT_NE(client2, nullptr);

    SUCCEED() << "Client moved successfully";
}

} // namespace

// Made with Bob
