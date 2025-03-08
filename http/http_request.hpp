// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

#include "http_body.hpp"
#include "sessions.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/url.hpp>
#include <boost/url/url_view.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace crow
{

struct Request
{
    using Body = boost::beast::http::request<bmcweb::HttpBody>;
    std::shared_ptr<Body> reqPtr = std::make_shared<Body>();

  private:
    boost::urls::url urlBase;

  public:
    boost::asio::ip::address ipAddress;

    std::shared_ptr<persistent_data::UserSession> session;

    std::string userRole;
    Request(Body reqIn, std::error_code& ec) :
        reqPtr(std::make_shared<Body>(std::move(reqIn)))
    {
        if (!setUrlInfo())
        {
            ec = std::make_error_code(std::errc::invalid_argument);
        }
    }

    Request(std::string_view bodyIn, std::error_code& /*ec*/) :
        reqPtr(std::make_shared<Body>(Body({}, bodyIn)))
    {}

    Request() = default;
    Request(const Request& other) = default;
    Request(Request&& other) = default;

    Request& operator=(const Request&) = delete;
    Request& operator=(const Request&&) = delete;
    ~Request() = default;

    Body& req()
    {
        return *reqPtr;
    }
    const Body& req() const
    {
        return *reqPtr;
    }

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
        reqPtr->clear();
        urlBase.clear();
        ipAddress = boost::asio::ip::address();
        session = nullptr;
        userRole = "";
    }

    boost::beast::http::verb method() const
    {
        return reqPtr->method();
    }

    void method(boost::beast::http::verb verb)
    {
        reqPtr->method(verb);
    }

    std::string_view methodString()
    {
        return reqPtr->method_string();
    }

    std::string_view getHeaderValue(std::string_view key) const
    {
        return (*reqPtr)[key];
    }

    std::string_view getHeaderValue(boost::beast::http::field key) const
    {
        return (*reqPtr)[key];
    }

    void clearHeader(boost::beast::http::field key)
    {
        reqPtr->erase(key);
    }

    std::string_view methodString() const
    {
        return reqPtr->method_string();
    }

    std::string_view target() const
    {
        return reqPtr->target();
    }

    boost::urls::url& url()
    {
        return urlBase;
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
        // NOLINTNEXTLINE(misc-include-cleaner)
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
