#pragma once

#include "common.hpp"
#include "http_body.hpp"
#include "sessions.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/url/url.hpp>

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

// Use (void) to silence unused warnings.
#define assertm(exp, msg) assert(((void)msg, exp))

namespace crow
{

struct Request
{
    using http_request_body = boost::beast::http::request<bmcweb::HttpBody>;

    std::shared_ptr<http_request_body> reqPtr;
    http_request_body& req;

  private:
    boost::urls::url urlBase;

  public:
    bool isSecure{false};

    boost::asio::io_context* ioService = nullptr;
    boost::asio::ip::address ipAddress;

    std::shared_ptr<persistent_data::UserSession> session;

    std::string userRole;
    Request(http_request_body&& reqIn, std::error_code& ec) :
        reqPtr(std::make_shared<http_request_body>(std::move(reqIn))),
        req(*reqPtr)
    {
        BMCWEB_LOG_ERROR("TEST: Request CTOR BEGIN");
        if (!setUrlInfo())
        {
            ec = std::make_error_code(std::errc::invalid_argument);
        }
        assertm(reqPtr != nullptr, "ABORT");
        BMCWEB_LOG_ERROR("TEST: Request CTOR END");
    }

#if 1
    Request() :
        reqPtr(std::make_shared<http_request_body>()),
        req(*reqPtr)
    {
        BMCWEB_LOG_ERROR("TEST: Request default CTOR2 begin");
        assertm(reqPtr != nullptr, "ABORT");
        BMCWEB_LOG_ERROR("TEST: Request default CTOR2 BEGIN");
        setUrlInfo();
        BMCWEB_LOG_ERROR("TEST: Request default CTOR2 END");
        assertm(reqPtr != nullptr, "ABORT");
    }
#endif


#if 1
    Request(const Request& other) :
        reqPtr(other.reqPtr), req(*reqPtr), urlBase(other.urlBase),
        isSecure(other.isSecure), ioService(other.ioService),
        ipAddress(other.ipAddress), session(other.session),
        userRole(other.userRole)
    {
            setUrlInfo();
    }
#else
    Request(const Request& other) = delete;
#endif

    Request(Request&& other) :
        reqPtr{std::move(other.reqPtr)}, req(*reqPtr),
        urlBase(std::move(other.urlBase)), isSecure(std::move(other.isSecure)),
        ioService(std::move(other.ioService)),
        ipAddress(std::move(other.ipAddress)),
        session(std::move(other.session)), userRole(std::move(other.userRole))
    {
        setUrlInfo();
    }

#if 0
    Request& operator=(const Request&) = delete;
    Request& operator=(const Request&&) = delete;

#else
    Request& operator=(const Request& other)
    {
        BMCWEB_LOG_ERROR("TEST: Request ASSIGN000 begin");
        reqPtr = other.reqPtr;
        assertm(reqPtr != nullptr, "ASSIGN000");
        req = *reqPtr;
        urlBase = other.urlBase;
        isSecure = other.isSecure;
        ipAddress = other.ipAddress;
        session = other.session;
        userRole = other.userRole;
 setUrlInfo();
        assertm(reqPtr != nullptr, "ASSIGN000");
        return *this;
    }

    Request& operator=(Request&& other)
    {
        BMCWEB_LOG_ERROR("TEST: Request ASSIGN begin");
        //reqPtr = std::move(other.reqPtr);
        reqPtr = other.reqPtr;
        assertm(reqPtr != nullptr, "ABORT");
        req = *reqPtr;
        urlBase = std::move(other.urlBase);
        isSecure = std::move(other.isSecure);
        ioService = std::move(other.ioService);
        ipAddress = std::move(other.ipAddress);
        session = std::move(other.session);
        userRole = std::move(other.userRole);
 setUrlInfo();
        assertm(reqPtr != nullptr, "ABORT");
        return *this;
    }
#endif

    ~Request() = default;

    void addHeader(std::string_view key, std::string_view value)
    {
        BMCWEB_LOG_ERROR("TEST: addHeader key={}", key);
        assertm(reqPtr != nullptr, "ABORT");
        req.insert(key, value);
    }

    void addHeader(boost::beast::http::field key, std::string_view value)
    {
        BMCWEB_LOG_ERROR("TEST: addHeader2 key");
        assertm(reqPtr != nullptr, "ABORT");
        req.insert(key, value);
    }

    void clear()
    {
        BMCWEB_LOG_ERROR("TEST: clear clear clear 11111");

        reqPtr = std::make_shared<http_request_body>();
        req = *reqPtr;

        BMCWEB_LOG_ERROR("TEST: clear clear clear 22222");
        assertm(reqPtr != nullptr, "ABORT");

        urlBase.clear();
        isSecure = false;
        ioService = nullptr;
        ipAddress = boost::asio::ip::address();
        session = nullptr;
        userRole = "";

    }

    boost::beast::http::verb method() const
    {
        assertm(reqPtr != nullptr, "ABORT");
        return req.method();
    }

    std::string_view getHeaderValue(std::string_view key) const
    {
        BMCWEB_LOG_ERROR("TEST: getHeaderValue key={}", key);
        assertm(reqPtr != nullptr, "ABORT");
        BMCWEB_LOG_ERROR("TEST: getHeaderValue key={} ... 1", key);
        // std::string_view val = req[key];
        // BMCWEB_LOG_ERROR("TEST: getHeaderValue key={} ... 2", val);
        return req[key];
    }

    std::string_view getHeaderValue(boost::beast::http::field key) const
    {
        BMCWEB_LOG_ERROR("TEST: getHeaderValue 22");
        assertm(reqPtr != nullptr, "ABORT");
        return req[key];
    }

    std::string_view methodString() const
    {
        assertm(reqPtr != nullptr, "ABORT");
        return req.method_string();
    }

    std::string_view target() const
    {
        assertm(reqPtr != nullptr, "ABORT");
        return req.target();
    }

    boost::urls::url_view url() const
    {
        return {urlBase};
    }

    const boost::beast::http::fields& fields() const
    {
        assertm(reqPtr != nullptr, "ABORT");
        return req.base();
    }

    const std::string& body() const
    {
        assertm(reqPtr != nullptr, "ABORT");
        return req.body().str();
    }

    bool target(std::string_view target)
    {
        assertm(reqPtr != nullptr, "ABORT");
        req.target(target);
        return setUrlInfo();
    }

    unsigned version() const
    {
        assertm(reqPtr != nullptr, "ABORT");
        return req.version();
    }

    bool isUpgrade() const
    {
        assertm(reqPtr != nullptr, "ABORT");
        return boost::beast::websocket::is_upgrade(req);
    }

    bool keepAlive() const
    {
        assertm(reqPtr != nullptr, "ABORT");
        return req.keep_alive();
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
