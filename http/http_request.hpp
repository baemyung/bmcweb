#pragma once

#include "common.hpp"
#include "http_body.hpp"
#include "sessions.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/url/url.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace crow
{

struct Request
{
    using http_request_body = boost::beast::http::request<bmcweb::HttpBody>;

    std::shared_ptr<http_request_body> reqPtr;

  private:
    boost::urls::url urlBase;

  public:
    bool isSecure{false};

    boost::asio::io_context* ioService = nullptr;
    boost::asio::ip::address ipAddress;

    std::shared_ptr<persistent_data::UserSession> session;

    std::string userRole;
    Request(http_request_body reqIn, std::error_code& ec) :
        reqPtr(std::make_shared<http_request_body>(std::move(reqIn)))
    {
        if (!setUrlInfo())
        {
            ec = std::make_error_code(std::errc::invalid_argument);
        }
    }

    Request() = default;

    Request(const Request& other) = default;
    Request(Request&& other) = default;

    Request& operator=(const Request&) = default;
    Request& operator=(Request&&) = default;
    ~Request() = default;

    void addHeader(std::string_view key, std::string_view value)
    {
        reqPtr->insert(key, value);
    }

    void addHeader(boost::beast::http::field key, std::string_view value)
    {
        reqPtr->insert(key, value);
    }

    void clear()
    {
        reqPtr = std::make_shared<http_request_body>();
        urlBase.clear();
        isSecure = false;
        ioService = nullptr;
        ipAddress = boost::asio::ip::address();
        session = nullptr;
        userRole = "";
    }

    boost::beast::http::verb method() const
    {
        return reqPtr->method();
    }

    std::string_view getHeaderValue(std::string_view key) const
    {
        return (*reqPtr)[key];
    }

    std::string_view getHeaderValue(boost::beast::http::field key) const
    {
        return (*reqPtr)[key];
    }

    std::string_view methodString() const
    {
        return reqPtr->method_string();
    }

    std::string_view target() const
    {
        return reqPtr->target();
    }

    boost::urls::url_view url() const
    {
        return {urlBase};
    }

    const boost::beast::http::fields& fields() const
    {
        return reqPtr->base();
    }

    const std::string& body() const
    {
        return reqPtr->body().str();
    }

    bool target(std::string_view target)
    {
        reqPtr->target(target);
        return setUrlInfo();
    }

    unsigned version() const
    {
        return reqPtr->version();
    }

    bool isUpgrade() const
    {
        return boost::beast::websocket::is_upgrade(*reqPtr);
    }

    bool keepAlive() const
    {
        return reqPtr->keep_alive();
    }

  private:
    bool setUrlInfo()
    {
        auto result = boost::urls::parse_relative_ref(target());

        if (!result)
        {
            return false;
        }
        urlBase = *result;
        return true;
    }
};

} // namespace crow
