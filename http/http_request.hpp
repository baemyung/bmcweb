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

    boost::beast::http::request<bmcweb::HttpBody> req;
    std::shared_ptr<boost::beast::http::request<bmcweb::HttpBody>> reqPtr;


  private:
    boost::urls::url urlBase;

  public:
    bool isSecure{false};

    boost::asio::io_context* ioService = nullptr;
    boost::asio::ip::address ipAddress;

    std::shared_ptr<persistent_data::UserSession> session;

    std::string userRole;
    Request(http_request_body reqIn, std::error_code& ec) :
   //      reqPtr(std::make_shared<http_request_body>(std::move(reqIn))), req(*reqPtr)
       req( std::move(reqIn)) , reqPtr(std::make_shared<http_request_body>( req ))
    {
//reqPtr = std::make_shared<http_request_body>(std::move(reqIn));

        if (!setUrlInfo())
        {
            ec = std::make_error_code(std::errc::invalid_argument);
        }
    }

#if 1
#if 1
    Request(std::string_view bodyIn, std::error_code& /*ec*/) :
   //reqPtr(std::make_shared<http_request_body>( http_request_body({}, bodyIn) ), req(*reqPtr)
   req({}, bodyIn) , reqPtr(std::make_shared<http_request_body>( req ))
    {
       //req.body( bodyIn );
      // boost::beast::http::request<bmcweb::HttpBody>  reqBody{{}, bodyIn);
      // reqPtr = std::make_shared<http_request_body>( reqBody );
    }
#else
    Request(std::string_view bodyIn, std::error_code& /*ec*/) :  
    reqPtr(std::make_shared<http_request_body>(bmcweb::HttpBody::value_type(bodyIn)), req(*reqPtr)
    {}
#endif
#endif


#if 0
    Request() :
   // reqPtr(std::make_shared<http_request_body>()), req(*reqPtr)
   req({}) , reqPtr(std::make_shared<http_request_body>( req ))
    {}
#else
    Request() = default;
#endif


    Request(const Request& other) = default;
    Request(Request&& other) = default;

    Request& operator=(const Request&) = default;

#if 0
    Request& operator=(Request&& other)
    {
       reqPtr = std::move(other.reqPtr);
       req = *reqPtr;
       return *this;
    }
#else
   Request& operator=(Request&&) = default;
#endif

#if 0
    Request& emplace( std::string_view bodyIn, std::error_code& /*ec*/) 
    {
       reqPtr = std::make_shared<http_request_body>(bodyIn);
       req = *reqPtr;
       return *this;
    }
#endif

    ~Request() = default;

    void addHeader(std::string_view key, std::string_view value)
    {
        req.insert(key, value);
    }

    void addHeader(boost::beast::http::field key, std::string_view value)
    {
        req.insert(key, value);
    }

    void clear()
    {
        req.clear();
        urlBase.clear();
        isSecure = false;
        ioService = nullptr;
        ipAddress = boost::asio::ip::address();
        session = nullptr;
        userRole = "";
    }

    boost::beast::http::verb method() const
    {
        return req.method();
    }

    std::string_view getHeaderValue(std::string_view key) const
    {
        return req[key];
    }

    std::string_view getHeaderValue(boost::beast::http::field key) const
    {
        return req[key];
    }

    std::string_view methodString() const
    {
        return req.method_string();
    }

    std::string_view target() const
    {
        return req.target();
    }

    boost::urls::url_view url() const
    {
        return {urlBase};
    }

    const boost::beast::http::fields& fields() const
    {
        return req.base();
    }

    const std::string& body() const
    {
        return req.body().str();
    }

    bool target(std::string_view target)
    {
        req.target(target);
        return setUrlInfo();
    }

    unsigned version() const
    {
        return req.version();
    }

    bool isUpgrade() const
    {
        return boost::beast::websocket::is_upgrade(req);
    }

    bool keepAlive() const
    {
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
