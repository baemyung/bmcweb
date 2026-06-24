// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#include "http_request.hpp"
#include "http_response.hpp"
#include "utils/json_utils.hpp"

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace redfish::json_util
{
namespace
{

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;

TEST(ReadJson, ValidElementsReturnsTrueResponseOkValuesUnpackedCorrectly)
{
    crow::Response res;
    nlohmann::json jsonRequest = {{"integer", 1},
                                  {"string", "hello"},
                                  {"vector", std::vector<uint64_t>{1, 2, 3}}};

    int64_t integer = 0;
    std::string str;
    std::vector<uint64_t> vec;
    ASSERT_TRUE(readJson(jsonRequest, res, "integer", integer, "string", str,
                         "vector", vec));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());

    EXPECT_EQ(integer, 1);
    EXPECT_EQ(str, "hello");
    EXPECT_THAT(vec, ElementsAre(1, 2, 3));
}

TEST(ReadJson, ValidObjectElementsReturnsTrueResponseOkValuesUnpackedCorrectly)
{
    crow::Response res;
    nlohmann::json::object_t jsonRequest;
    jsonRequest["integer"] = 1;
    jsonRequest["string"] = "hello";
    jsonRequest["vector"] = std::vector<uint64_t>{1, 2, 3};

    int64_t integer = 0;
    std::string str;
    std::vector<uint64_t> vec;
    ASSERT_TRUE(readJsonObject(jsonRequest, res, "integer", integer, "string",
                               str, "vector", vec));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());

    EXPECT_EQ(integer, 1);
    EXPECT_EQ(str, "hello");
    EXPECT_THAT(vec, ElementsAre(1, 2, 3));
}

TEST(ReadJson, VariantValueUnpackedNull)
{
    crow::Response res;
    nlohmann::json jsonRequest = {{"nullval", nullptr}};

    std::variant<std::string, std::nullptr_t> str;

    ASSERT_TRUE(readJson(jsonRequest, res, "nullval", str));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);

    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(str));
}

TEST(ReadJson, VariantValueUnpackedValue)
{
    crow::Response res;
    nlohmann::json jsonRequest = {{"stringval", "mystring"}};

    std::variant<std::string, std::nullptr_t> str;

    ASSERT_TRUE(readJson(jsonRequest, res, "stringval", str));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);

    ASSERT_TRUE(std::holds_alternative<std::string>(str));
    EXPECT_EQ(std::get<std::string>(str), "mystring");
}

TEST(readJson, ExtraElementsReturnsFalseReponseIsBadRequest)
{
    crow::Response res;
    nlohmann::json jsonRequest = {{"integer", 1}, {"string", "hello"}};

    std::optional<int> integer;
    std::optional<std::string> str;

    ASSERT_FALSE(readJson(jsonRequest, res, "integer", integer));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));
    EXPECT_EQ(integer, 1);

    ASSERT_FALSE(readJson(jsonRequest, res, "string", str));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));
    EXPECT_EQ(str, "hello");
}

TEST(ReadJson, WrongElementTypeReturnsFalseReponseIsBadRequest)
{
    crow::Response res;
    nlohmann::json jsonRequest = {{"integer", 1}, {"string0", "hello"}};

    int64_t integer = 0;
    std::string str0;
    ASSERT_FALSE(readJson(jsonRequest, res, "integer", str0));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));

    ASSERT_FALSE(readJson(jsonRequest, res, "string0", integer));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));

    ASSERT_FALSE(
        readJson(jsonRequest, res, "integer", str0, "string0", integer));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));
}

TEST(ReadJson, MissingElementReturnsFalseReponseIsBadRequest)
{
    crow::Response res;
    nlohmann::json jsonRequest = {{"integer", 1}, {"string0", "hello"}};

    int64_t integer = 0;
    std::string str0;
    std::string str1;
    std::vector<uint8_t> vec;
    ASSERT_FALSE(readJson(jsonRequest, res, "integer", integer, "string0", str0,
                          "vector", vec));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));

    ASSERT_FALSE(readJson(jsonRequest, res, "integer", integer, "string0", str0,
                          "string1", str1));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));
}

TEST(ReadJson, JsonArrayAreUnpackedCorrectly)
{
    crow::Response res;
    nlohmann::json jsonRequest = R"(
        {
            "TestJson": [{"hello": "yes"}, {"there": "no"}]
        }
    )"_json;

    std::vector<nlohmann::json::object_t> jsonVec;
    ASSERT_TRUE(readJson(jsonRequest, res, "TestJson", jsonVec));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());
    nlohmann::json::object_t hello;
    hello["hello"] = "yes";
    nlohmann::json::object_t there;
    there["there"] = "no";
    EXPECT_THAT(jsonVec, ElementsAre(hello, there));
}

TEST(ReadJson, JsonSubElementValueAreUnpackedCorrectly)
{
    crow::Response res;
    nlohmann::json jsonRequest = R"(
        {
            "json": {"integer": 42}
        }
    )"_json;

    int integer = 0;
    ASSERT_TRUE(readJson(jsonRequest, res, "json/integer", integer));
    EXPECT_EQ(integer, 42);
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());
}

TEST(ReadJson, JsonDeeperSubElementValueAreUnpackedCorrectly)
{
    crow::Response res;
    nlohmann::json jsonRequest = R"(
        {
            "json": {
                "json2": {"string": "foobar"}
            }
        }
    )"_json;

    std::string foobar;
    ASSERT_TRUE(readJson(jsonRequest, res, "json/json2/string", foobar));
    EXPECT_EQ(foobar, "foobar");
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());
}

TEST(ReadJson, MultipleJsonSubElementValueAreUnpackedCorrectly)
{
    crow::Response res;
    nlohmann::json jsonRequest = R"(
        {
            "json": {
                "integer": 42,
                "string": "foobar"
            },
            "string": "bazbar"
        }
    )"_json;

    int integer = 0;
    std::string foobar;
    std::string bazbar;
    ASSERT_TRUE(readJson(jsonRequest, res, "json/integer", integer,
                         "json/string", foobar, "string", bazbar));
    EXPECT_EQ(integer, 42);
    EXPECT_EQ(foobar, "foobar");
    EXPECT_EQ(bazbar, "bazbar");
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());
}

TEST(ReadJson, ExtraElement)
{
    crow::Response res;
    nlohmann::json jsonRequest = {{"integer", 1}, {"string", "hello"}};

    std::optional<int> integer;
    std::optional<std::string> str;

    EXPECT_FALSE(readJson(jsonRequest, res, "integer", integer));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_FALSE(res.jsonValue.empty());
    EXPECT_EQ(integer, 1);

    EXPECT_FALSE(readJson(jsonRequest, res, "string", str));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_FALSE(res.jsonValue.empty());
    EXPECT_EQ(str, "hello");
}

TEST(ReadJson, ValidMissingElementReturnsTrue)
{
    crow::Response res;
    nlohmann::json jsonRequest = {{"integer", 1}};

    std::optional<int> integer;
    int requiredInteger = 0;
    std::optional<std::string> str0;
    std::optional<std::string> str1;
    std::optional<std::vector<uint8_t>> vec;
    ASSERT_TRUE(readJson(jsonRequest, res, "missing_integer", integer,
                         "integer", requiredInteger));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_TRUE(res.jsonValue.empty());
    EXPECT_EQ(integer, std::nullopt);

    ASSERT_TRUE(readJson(jsonRequest, res, "missing_string", str0, "integer",
                         requiredInteger));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());
    EXPECT_EQ(str0, std::nullopt);

    ASSERT_TRUE(readJson(jsonRequest, res, "integer", integer, "string", str0,
                         "vector", vec));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());
    EXPECT_EQ(integer, 1);
    EXPECT_EQ(str0, std::nullopt);
    EXPECT_EQ(vec, std::nullopt);

    ASSERT_TRUE(readJson(jsonRequest, res, "integer", integer, "string0", str0,
                         "missing_string", str1));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());
    EXPECT_EQ(str1, std::nullopt);
}

TEST(ReadJson, InvalidMissingElementReturnsFalse)
{
    crow::Response res;
    nlohmann::json jsonRequest = {{"integer", 1}, {"string", "hello"}};

    int integer = 0;
    std::string str0;
    std::string str1;
    std::vector<uint8_t> vec;
    ASSERT_FALSE(readJson(jsonRequest, res, "missing_integer", integer));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));

    ASSERT_FALSE(readJson(jsonRequest, res, "missing_string", str0));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));

    ASSERT_FALSE(readJson(jsonRequest, res, "integer", integer, "string", str0,
                          "vector", vec));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));

    ASSERT_FALSE(readJson(jsonRequest, res, "integer", integer, "string0", str0,
                          "missing_string", str1));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));
}

TEST(ReadJsonPatch, ValidElementsReturnsTrueResponseOkValuesUnpackedCorrectly)
{
    crow::Response res;
    std::error_code ec;
    crow::Request req("{\"integer\": 1}", ec);

    // Ignore errors intentionally
    req.addHeader(boost::beast::http::field::content_type, "application/json");

    int64_t integer = 0;
    ASSERT_TRUE(readJsonPatch(req, res, "integer", integer));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());
    EXPECT_EQ(integer, 1);
}

TEST(ReadJsonPatch, EmptyObjectReturnsFalseResponseBadRequest)
{
    crow::Response res;
    std::error_code ec;
    crow::Request req("{}", ec);
    // Ignore errors intentionally

    std::optional<int64_t> integer = 0;
    ASSERT_FALSE(readJsonPatch(req, res, "integer", integer));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));
}

TEST(ReadJsonPatch, OdataIgnored)
{
    crow::Response res;
    std::error_code ec;
    crow::Request req(R"({"@odata.etag": "etag", "integer": 1})", ec);
    req.addHeader(boost::beast::http::field::content_type, "application/json");
    // Ignore errors intentionally

    std::optional<int64_t> integer = 0;
    ASSERT_TRUE(readJsonPatch(req, res, "integer", integer));
    EXPECT_EQ(res.result(), boost::beast::http::status::ok);
    EXPECT_THAT(res.jsonValue, IsEmpty());
    EXPECT_EQ(integer, 1);
}

TEST(ReadJsonPatch, OnlyOdataGivesNoOperation)
{
    crow::Response res;
    std::error_code ec;
    crow::Request req(R"({"@odata.etag": "etag"})", ec);
    // Ignore errors intentionally

    std::optional<int64_t> integer = 0;
    ASSERT_FALSE(readJsonPatch(req, res, "integer", integer));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);
    EXPECT_THAT(res.jsonValue, Not(IsEmpty()));
}

TEST(ReadJsonPatch, VerifyReadJsonPatchIntegerReturnsOutOfRange)
{
    crow::Response res;
    std::error_code ec;

    // 4294967296 is an out-of-range value for uint32_t
    crow::Request req(R"({"@odata.etag": "etag", "integer": 4294967296})", ec);
    req.addHeader(boost::beast::http::field::content_type, "application/json");

    uint32_t integer = 0;
    ASSERT_FALSE(readJsonPatch(req, res, "integer", integer));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);

    const nlohmann::json& resExtInfo =
        res.jsonValue["error"]["@Message.ExtendedInfo"];
    EXPECT_THAT(resExtInfo[0]["@odata.type"], "#Message.v1_1_1.Message");
    EXPECT_THAT(resExtInfo[0]["MessageId"],
                "Base.1.19.PropertyValueOutOfRange");
    EXPECT_THAT(resExtInfo[0]["MessageSeverity"], "Warning");
}

TEST(ReadJsonPatch, VerifyReadJsonPatchBadVectorObject)
{
    crow::Response res;
    std::error_code ec;
    nlohmann::json jsonRequest = {{"NotVector", 1}};

    std::vector<int> indices;
    ASSERT_FALSE(readJson(jsonRequest, res, "NotVector", indices));
    EXPECT_EQ(res.result(), boost::beast::http::status::bad_request);

    std::cout << " XXX.VerifyReadJsonPatchBadVectorObject111="
              << res.jsonValue.dump(-1, ' ', true,
                                    nlohmann::json::error_handler_t::replace)
              << "\n";
    std::cerr << " XXX.VerifyReadJsonPatchBadVectorObject222="
              << res.jsonValue.dump(-1, ' ', true,
                                    nlohmann::json::error_handler_t::replace)
              << "\n";
    const nlohmann::json& argsExtInfo =
        res.jsonValue["NotVector@Message.ExtendedInfo"][0];

    // EXPECT_THAT(argsExtInfo, "XXXX");

    std::cout << " XXX.argsExtInfo4444="
              << argsExtInfo.dump(-1, ' ', true,
                                  nlohmann::json::error_handler_t::replace)
              << "\n";

    EXPECT_THAT(argsExtInfo["MessageArgs"][0], "1");
    EXPECT_THAT(argsExtInfo["MessageArgs"][1], "NotVector");
}

} // namespace
} // namespace redfish::json_util
