// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

#include "app.hpp"
#include "async_resp.hpp"
#include "http_request.hpp"
#include "redfish_oem_routing.hpp"
#include "sub_request.hpp"
#include "verb.hpp"

#include <memory>

namespace redfish
{
/*
 * @brief Top level class installing and providing Redfish services
 */
class RedfishService
{
  public:
    /*
     * @brief Redfish service constructor
     *
     * Loads Redfish configuration and installs schema resources
     *
     * @param[in] app   Crow app on which Redfish will initialize
     */
    explicit RedfishService(App& app);

    // Temporary change to make redfish instance available in other places
    // like query delegation.
    static RedfishService& getInstance(App& app)
    {
        static RedfishService redfish(app);
        return redfish;
    }

    void validate()
    {
        oemRouter.validate();
    }

    template <StringLiteral Rule>
    auto& newRoute(HttpVerb method)
    {
        constexpr std::string_view rule = Rule;
        BMCWEB_LOG_ERROR("TEST:TEST:TEST subRequest.newRoute. Rule={}", rule);
        return oemRouter.newRule<Rule>(method);
    }

    void handleSubRoute(
        const crow::Request& req,
        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) const
    {
        BMCWEB_LOG_ERROR("TEST:TEST:TEST:  handleSubRoute req,url={}",
                         req.url());

        auto subReq = std::make_shared<SubRequest>(req);
        if (!subReq->needHandling())
        {
            return;
        }

        BMCWEB_LOG_ERROR(
            "TEST:TEST: CALL handleSubRoute, req.url={}, subreq.url={}",
            req.url(), subReq->url());
        oemRouter.handle(subReq, asyncResp);
    }

    OemRouter oemRouter;
};

template <StringLiteral Path>
auto& REDFISH_SUB_ROUTE(RedfishService& service, HttpVerb method)
{
    return service.newRoute<Path>(method);
}

} // namespace redfish
