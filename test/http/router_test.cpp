#include "async_resp.hpp" // IWYU pragma: keep
#include "http_request.hpp"
#include "routing.hpp"
#include "utility.hpp"

#include <boost/beast/http/message.hpp> // IWYU pragma: keep
#include <boost/beast/http/verb.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h> // IWYU pragma: keep

// IWYU pragma: no_include <boost/beast/http/impl/message.hpp>
// IWYU pragma: no_include "gtest/gtest_pred_impl.h"
// IWYU pragma: no_include <boost/intrusive/detail/list_iterator.hpp>
// IWYU pragma: no_include <gtest/gtest-message.h>
// IWYU pragma: no_include <gtest/gtest-test-part.h>
// IWYU pragma: no_forward_declare bmcweb::AsyncResp

namespace crow
{
namespace
{

using ::crow::utility::getParameterTag;

TEST(Router, OverlapingRoutes2)
{
    // Callback handler that does nothing
    bool foofooCalled = false;
    auto foofooCallback = [](const Request&,
                             const std::shared_ptr<bmcweb::AsyncResp>&) {
        foofooCalled = true;
        EXPECT_TRUE(true);
    };
    bool barCalled = false;
    auto foobarCallback =
        [&barCalled](const Request&, const std::shared_ptr<bmcweb::AsyncResp>&,
                     const std::string& bar) {
        barCalled = true;
        EXPECT_EQ(bar, "bar");
    };

    Router router;
    std::error_code ec;

    router.newRuleTagged<getParameterTag("/foo/<str>")>("/foo/<str>")(
        foobarCallback);

    router.newRuleTagged<getParameterTag("/foo/foo")>("/foo/foo")(
        foofooCallback);

    router.validate();
    {
        constexpr std::string_view url = "/foo/foo";

        auto req = std::make_shared<Request>(
            Request::Body{boost::beast::http::verb::get, url, 11}, ec);

        std::shared_ptr<bmcweb::AsyncResp> asyncResp =
            std::make_shared<bmcweb::AsyncResp>();

        router.handle(req, asyncResp);
    }
    EXPECT_TRUE(foofooCalled);
}

} // namespace
} // namespace crow
