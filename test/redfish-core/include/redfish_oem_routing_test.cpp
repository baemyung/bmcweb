#include "app.hpp"
#include "async_resp.hpp"
#include "http_request.hpp"
#include "redfish.hpp"
#include "registries/privilege_registry.hpp"
#include "sub_request.hpp"
#include "verb.hpp"

#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

namespace redfish
{
namespace
{

TEST(OemRouter, FragmentRoutesWithAssembly)
{
    std::error_code ec;
    App app;
    RedfishService service(app);

    bool assemblyHeadCalled = false;
    auto assemblyHeadCallback =
        [&assemblyHeadCalled](const crow::Request&,
                              const std::shared_ptr<bmcweb::AsyncResp>&,
                              const std::string& chassisId) {
            assemblyHeadCalled = true;
            EXPECT_EQ(chassisId, "chassis");
        };

    bool assemblyGetCalled = false;
    auto assemblyGetCallback =
        [&assemblyGetCalled](const crow::Request&,
                             const std::shared_ptr<bmcweb::AsyncResp>&,
                             const std::string& chassisId) {
            assemblyGetCalled = true;
            EXPECT_EQ(chassisId, "chassis");
        };

    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/Assembly/")
        .privileges(redfish::privileges::headAssembly)
        .methods(boost::beast::http::verb::head)(assemblyHeadCallback);

    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/Assembly/")
        .privileges(redfish::privileges::getAssembly)
        .methods(boost::beast::http::verb::get)(assemblyGetCallback);

    // Need the normal route registered for OEM to work

    // Callback handler that does nothing
    bool oemCalled = false;
    auto oemCallback = [&oemCalled](const SubRequest&,
                                    const std::shared_ptr<bmcweb::AsyncResp>&,
                                    const std::string& bar) {
        oemCalled = true;
        EXPECT_EQ(bar, "bar");
    };

    bool standardCalled = false;
    auto standardCallback =
        [&standardCalled,
         &service](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& bar) {
            service.handleSubRoute(req, asyncResp);
            standardCalled = true;
            EXPECT_EQ(bar, "bar");
        };

    BMCWEB_ROUTE(app, "/foo/<str>/")
        .privileges(redfish::privileges::headAssembly)
        .methods(boost::beast::http::verb::head)(standardCallback);

    BMCWEB_ROUTE(app, "/foo/<str>/")
        .privileges(redfish::privileges::getAssembly)
        .methods(boost::beast::http::verb::get)(standardCallback);

    REDFISH_SUB_ROUTE<"/foo/<str>/#/Oem">(service, HttpVerb::Get)(oemCallback);

    app.validate();
    service.validate();

    {
        //
        std::shared_ptr<bmcweb::AsyncResp> asyncResp =
            std::make_shared<bmcweb::AsyncResp>();

        // foo bar
        constexpr std::string_view reqUrl = "/foo/bar";
        std::shared_ptr<crow::Request> req = std::make_shared<crow::Request>(
            crow::Request::Body{boost::beast::http::verb::get, reqUrl, 11}, ec);

        // assembly
        constexpr std::string_view reqAssemblyUrl =
            "/redfish/v1/Chassis/chassis/Assembly";
        std::shared_ptr<crow::Request> reqHeadAssembly =
            std::make_shared<crow::Request>(
                crow::Request::Body{boost::beast::http::verb::head,
                                    reqAssemblyUrl, 11},
                ec);
        std::shared_ptr<crow::Request> reqGetAssembly =
            std::make_shared<crow::Request>(
                crow::Request::Body{boost::beast::http::verb::get,
                                    reqAssemblyUrl, 11},
                ec);

        // perform handle
        app.handle(req, asyncResp);
        app.handle(reqHeadAssembly, asyncResp);
        app.handle(reqGetAssembly, asyncResp);
    }
    EXPECT_TRUE(oemCalled);
    EXPECT_TRUE(standardCalled);
    EXPECT_TRUE(assemblyHeadCalled);
    EXPECT_TRUE(assemblyGetCalled);
}

TEST(OemRouter, FragmentRoutes)
{
    std::error_code ec;
    App app;
    RedfishService service(app);

    // Callback handler that does nothing
    bool oemCalled = false;
    auto oemCallback = [&oemCalled](const SubRequest&,
                                    const std::shared_ptr<bmcweb::AsyncResp>&,
                                    const std::string& bar) {
        oemCalled = true;
        EXPECT_EQ(bar, "bar");
    };
    bool standardCalled = false;
    auto standardCallback =
        [&standardCalled,
         &service](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& bar) {
            service.handleSubRoute(req, asyncResp);
            standardCalled = true;
            EXPECT_EQ(bar, "bar");
        };
    // Need the normal route registered for OEM to work
    BMCWEB_ROUTE(app, "/foo/<str>/")
        .methods(boost::beast::http::verb::get)(standardCallback);
    REDFISH_SUB_ROUTE<"/foo/<str>/#/Oem">(service, HttpVerb::Get)(oemCallback);

    app.validate();
    service.validate();

    {
        constexpr std::string_view reqUrl = "/foo/bar";

        std::shared_ptr<crow::Request> req = std::make_shared<crow::Request>(
            crow::Request::Body{boost::beast::http::verb::get, reqUrl, 11}, ec);

        std::shared_ptr<bmcweb::AsyncResp> asyncResp =
            std::make_shared<bmcweb::AsyncResp>();

        app.handle(req, asyncResp);
    }
    EXPECT_TRUE(oemCalled);
    EXPECT_TRUE(standardCalled);
}

TEST(OemRouter, PatchHandlerWithJsonObject)
{
    std::error_code ec;
    App app;
    RedfishService service(app);

    // Callback handlers
    bool callback1Called = false;
    auto patchCallback1 =
        [&callback1Called](
            const SubRequest& req,
            [[maybe_unused]] const std::shared_ptr<bmcweb::AsyncResp>&
                asyncResp,
            [[maybe_unused]] const std::string& param) {
            callback1Called = true;

            const nlohmann::json::object_t& payload = req.payload();
            auto oemFooIt = payload.find("OemFoo");
            ASSERT_NE(oemFooIt, payload.end());
            ASSERT_TRUE(oemFooIt->second.is_object());

            auto keyIt = oemFooIt->second.find("OemFooKey");
            ASSERT_NE(keyIt, oemFooIt->second.end());
            EXPECT_EQ(keyIt.value(), "fooValue");
        };

    bool callback2Called = false;
    auto patchCallback2 =
        [&callback2Called](
            const SubRequest& req,
            [[maybe_unused]] const std::shared_ptr<bmcweb::AsyncResp>&
                asyncResp,
            [[maybe_unused]] const std::string& param) {
            callback2Called = true;

            const nlohmann::json::object_t& payload = req.payload();
            auto oemBarIt = payload.find("OemBar");
            ASSERT_NE(oemBarIt, payload.end());
            ASSERT_TRUE(oemBarIt->second.is_object());

            auto keyIt = oemBarIt->second.find("OemBarKey");
            ASSERT_NE(keyIt, oemBarIt->second.end());
            EXPECT_EQ(keyIt.value(), "barValue");
        };

    bool standardCalled = false;
    auto standardCallback =
        [&standardCalled,
         &service](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& bar) {
            service.handleSubRoute(req, asyncResp);
            standardCalled = true;
            EXPECT_EQ(bar, "bar");
        };

    // Need the normal route registered for OEM to work
    BMCWEB_ROUTE(app, "/foo/<str>/")
        .methods(boost::beast::http::verb::patch)(standardCallback);

    REDFISH_SUB_ROUTE<"/foo/<str>/#/Oem/OemFoo">(service, HttpVerb::Patch)(
        patchCallback1);
    REDFISH_SUB_ROUTE<"/foo/<str>/#/Oem/OemBar">(service, HttpVerb::Patch)(
        patchCallback2);

    app.validate();
    service.validate();

    {
        constexpr std::string_view reqUrl = "/foo/bar";
        // OEM payload
        nlohmann::json reqBody = {{"Oem",
                                   {{"OemFoo", {{"OemFooKey", "fooValue"}}},
                                    {"OemBar", {{"OemBarKey", "barValue"}}}}}};

        // Create request with the body string
        std::shared_ptr<crow::Request> req = std::make_shared<crow::Request>(
            crow::Request::Body{
                boost::beast::http::verb::patch, reqUrl, 11,
                reqBody.dump() // Pass body string directly in the constructor
            },
            ec);

        req->addHeader("Content-Type", "application/json");

        std::shared_ptr<bmcweb::AsyncResp> asyncResp =
            std::make_shared<bmcweb::AsyncResp>();

        app.handle(req, asyncResp);
    }

    EXPECT_TRUE(standardCalled);
    EXPECT_TRUE(callback1Called);
    EXPECT_TRUE(callback2Called);
}

} // namespace
} // namespace redfish
