// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#include "http/http_client.hpp"
#include "http_response.hpp"
#include "ssl_key_handler.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/url/url_view.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace
{

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

    void acceptAndRespondOnce()
    {
        acceptAndRespondNTimes(1);
    }

    void acceptAndRespondNTimes(std::size_t remainingResponses)
    {
        acceptor.async_accept(
            socket, [this, remainingResponses](
                        const boost::system::error_code& ec) mutable {
                if (ec)
                {
                    return;
                }

                static constexpr std::string_view response =
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n"
                    "\r\n";

                boost::asio::async_write(
                    socket, boost::asio::buffer(response),
                    [this, remainingResponses](const boost::system::error_code&,
                                               std::size_t) mutable {
                        boost::system::error_code ignoredEc;
                        socket.shutdown(
                            boost::asio::ip::tcp::socket::shutdown_both,
                            ignoredEc);
                        socket.close(ignoredEc);

                        if (remainingResponses > 1)
                        {
                            acceptAndRespondNTimes(remainingResponses - 1);
                        }
                    });
            });
    }

    uint16_t getPort() const
    {
        return port;
    }

  private:
    boost::asio::ip::tcp::acceptor acceptor;
    boost::asio::ip::tcp::socket socket;
    uint16_t port{};
};

class HttpClientCoredumpTest : public ::testing::Test
{
  protected:
    boost::asio::io_context ioc;
};

TEST_F(HttpClientCoredumpTest, EmptyInvalidRespThrowsBadFunctionCallAfterRead)
{
    SimpleHttpServer server(ioc);
    server.acceptAndRespondOnce();

    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;
    policy->invalidResp = {};

    auto client = std::make_unique<crow::HttpClient>(ioc, policy);

    boost::beast::http::fields headers;
    headers.set(boost::beast::http::field::host, "127.0.0.1");

    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.getPort()) + "/";

    client->sendDataWithCallback(
        "", boost::urls::url_view(url), ensuressl::VerifyCertificate::NoVerify,
        headers, boost::beast::http::verb::get, [](crow::Response&) {});

    EXPECT_THROW(
        {
            try
            {
                ioc.run();
            }
            catch (const std::bad_function_call&)
            {
                throw std::runtime_error("empty invalidResp throws");
            }
        },
        std::runtime_error);
}

TEST_F(HttpClientCoredumpTest, DefaultInvalidRespDoesNotThrow)
{
    SimpleHttpServer server(ioc);
    server.acceptAndRespondOnce();

    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    auto client = std::make_unique<crow::HttpClient>(ioc, policy);

    boost::beast::http::fields headers;
    headers.set(boost::beast::http::field::host, "127.0.0.1");

    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.getPort()) + "/";

    bool callbackInvoked = false;
    client->sendDataWithCallback(
        "", boost::urls::url_view(url), ensuressl::VerifyCertificate::NoVerify,
        headers, boost::beast::http::verb::get,
        [&callbackInvoked](crow::Response&) { callbackInvoked = true; });

    EXPECT_NO_THROW(ioc.run());
    EXPECT_TRUE(callbackInvoked);
}

TEST_F(HttpClientCoredumpTest, QueuedHandlerCanObserveMovedFromCallbackState)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;
    policy->maxConnections = 1;

    auto client = std::make_unique<crow::HttpClient>(ioc, policy);

    boost::beast::http::fields headers;
    headers.set(boost::beast::http::field::host, "192.0.2.1");

    std::atomic<bool> firstCallbackInvoked{false};

    std::function<void(crow::Response&)> sharedHandler =
        [&firstCallbackInvoked](crow::Response&) {
            firstCallbackInvoked.store(true);
        };

    std::function<void(crow::Response&)> movedHandler = sharedHandler;

    EXPECT_TRUE(static_cast<bool>(sharedHandler));
    EXPECT_TRUE(static_cast<bool>(movedHandler));

    client->sendDataWithCallback(
        "", boost::urls::url_view("http://192.0.2.1:81/"),
        ensuressl::VerifyCertificate::NoVerify, headers,
        boost::beast::http::verb::get, movedHandler);

    client->sendDataWithCallback(
        "", boost::urls::url_view("http://192.0.2.1:81/"),
        ensuressl::VerifyCertificate::NoVerify, headers,
        boost::beast::http::verb::get, movedHandler);

    for (int i = 0; i < 200; ++i)
    {
        ioc.poll();
        if (ioc.stopped())
        {
            ioc.restart();
        }
    }

    EXPECT_FALSE(firstCallbackInvoked.load());
}

TEST_F(HttpClientCoredumpTest,
       QueuedRequestHitsEmptyInvalidRespAfterFirstCallbackMutatesState)
{
    SimpleHttpServer server(ioc);
    server.acceptAndRespondNTimes(2);

    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;
    policy->maxConnections = 1;

    auto client = std::make_unique<crow::HttpClient>(ioc, policy);

    boost::beast::http::fields headers;
    headers.set(boost::beast::http::field::host, "127.0.0.1");

    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.getPort()) + "/";

    std::unique_ptr<crow::HttpClient>* clientPtr = &client;
    std::atomic<bool> firstCallbackInvoked{false};

    client->sendDataWithCallback(
        "", boost::urls::url_view(url), ensuressl::VerifyCertificate::NoVerify,
        headers, boost::beast::http::verb::get,
        [clientPtr, policy, &firstCallbackInvoked](crow::Response&) {
            firstCallbackInvoked.store(true);
            policy->invalidResp = {};
            clientPtr->reset();
        });

    client->sendDataWithCallback(
        "", boost::urls::url_view(url), ensuressl::VerifyCertificate::NoVerify,
        headers, boost::beast::http::verb::get, [](crow::Response&) {});

    EXPECT_THROW(
        {
            try
            {
                ioc.run();
            }
            catch (const std::bad_function_call&)
            {
                throw std::runtime_error(
                    "queued request hit empty invalidResp");
            }
        },
        std::runtime_error);
    EXPECT_TRUE(firstCallbackInvoked.load());
    EXPECT_EQ(client, nullptr);
}

TEST_F(HttpClientCoredumpTest, PostedHandlerRunsAfterStateMutation)
{
    auto policy = std::make_shared<crow::ConnectionPolicy>();
    policy->maxRetryAttempts = 0;

    std::atomic<bool> handlerInvoked{false};

    boost::asio::post(ioc, [&policy, &handlerInvoked]() {
        handlerInvoked.store(true);
        EXPECT_THROW(
            {
                if (policy->invalidResp(500))
                {}
            },
            std::bad_function_call);
    });

    policy->invalidResp = {};

    EXPECT_NO_THROW(ioc.run());
    EXPECT_TRUE(handlerInvoked.load());
}

} // namespace
