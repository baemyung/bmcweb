#pragma once

#include "http_request.hpp"

#include <functional>

namespace bmcweb
{

/**
 * AsyncReq
 */

class AsyncReq
{
  public:
    AsyncReq() = delete;
    explicit AsyncReq(crow::Request&& reqIn) : req(std::move(reqIn)) {}

    AsyncReq(const AsyncReq&) = delete;
    AsyncReq(AsyncReq&&) = delete;
    AsyncReq& operator=(const AsyncReq&) = delete;
    AsyncReq& operator=(AsyncReq&&) = delete;

    ~AsyncReq()
    {
        req.end();
    }

    crow::Request req;
};

} // namespace bmcweb
