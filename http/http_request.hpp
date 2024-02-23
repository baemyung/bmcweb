#pragma once

#include "common.hpp"
#include "sessions.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/url/url.hpp>
#include <http_file_body.hpp>

#include <string>
#include <string_view>
#include <system_error>

namespace crow
{

struct Request
{
    boost::beast::http::request<bmcweb::FileBody> req;

  private:
    boost::urls::url urlBase{};

  public:
    bool isSecure{false};

    boost::asio::io_context* ioService{};
    boost::asio::ip::address ipAddress{};

    std::shared_ptr<persistent_data::UserSession> session;

    std::string userRole{};
    Request(boost::beast::http::request<bmcweb::FileBody> reqIn,
            std::error_code& ec) :
        req(std::move(reqIn))
    {
        if (!setUrlInfo())
        {
            ec = std::make_error_code(std::errc::invalid_argument);
        }


        std::string ifmatchEtag;
        BMCWEB_LOG_ERROR("TEST:A: Request Going thru fields BEGIN");
        for (const auto& field : req.base())
        {
            std::string header;
            header.reserve(field.name_string().size() + 2 +
                           field.value().size());
            header += field.name_string();
            header += ": ";
            header += field.value();

            if(field.name_string() == "if-match") {
                ifmatchEtag = field.value();
            }

            BMCWEB_LOG_ERROR("    TEST:B: Request  header=({})", header);
        }
        BMCWEB_LOG_ERROR("TEST:A: Request Going thru fields END");

        //Change "if-match" to a single element
#if 0
        if(!ifmatchEtag.empty())
        {
            std::string ifmatchEtagOne = ifmatchEtag.substr(0, ifmatchEtag.find(","));
            BMCWEB_LOG_ERROR(" TEST: REQUEST Change ifmatch({} to a single token={}", ifmatchEtag, ifmatchEtagOne);
            req.base().erase("if-match");
            req.base().insert("if-match", ifmatchEtagOne);
             BMCWEB_LOG_ERROR(" TEST: REQUEST Verify if-match{}", req.base()["if-match"]);
        }
#endif
        
    }

    Request(std::string_view bodyIn, std::error_code& /*ec*/) : req({}, bodyIn)
    {
     BMCWEB_LOG_ERROR("TEST:A: Request Going thru fields BEGIN");
        for (const auto& field : req.base())
        {
            std::string header;
            header.reserve(field.name_string().size() + 2 +
                           field.value().size());
            header += field.name_string();
            header += ": ";
            header += field.value();

            BMCWEB_LOG_ERROR("    TEST:B: Request  header=({})", header);
        }
     BMCWEB_LOG_ERROR("TEST:A: Request Going thru fields END");
    }

    Request() = default;

    Request(const Request& other) = default;
    Request(Request&& other) = default;

    Request& operator=(const Request&) = default;
    Request& operator=(Request&&) = default;
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
        BMCWEB_LOG_ERROR("TEST:0:X:X:X: http_request CLEAR BEGIN");
        req.clear();
        urlBase.clear();
        isSecure = false;
        ioService = nullptr;
        ipAddress = boost::asio::ip::address();
        session = nullptr;
        userRole = "";
        BMCWEB_LOG_ERROR("TEST:0: http_request CLEAR END");
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
