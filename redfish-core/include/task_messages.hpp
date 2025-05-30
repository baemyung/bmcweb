#pragma once
/****************************************************************
 *                 READ THIS WARNING FIRST
 * This is an auto-generated header which contains definitions
 * for Redfish DMTF defined messages.
 * DO NOT modify this registry outside of running the
 * parse_registries.py script.  The definitions contained within
 * this file are owned by DMTF.  Any modifications to these files
 * should be first pushed to the relevant registry in the DMTF
 * github organization.
 ***************************************************************/
// These generated headers are a superset of what is needed.
// clang sees them as an error, so ignore
// NOLINTBEGIN(misc-include-cleaner)
#include "http_response.hpp"

#include <boost/url/url_view_base.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <source_location>
#include <string_view>
// NOLINTEND(misc-include-cleaner)

namespace redfish
{

namespace messages
{
nlohmann::json::object_t taskStarted(std::string_view arg1);

nlohmann::json::object_t taskCompletedOK(std::string_view arg1);

nlohmann::json::object_t taskCompletedWarning(std::string_view arg1);

nlohmann::json::object_t taskAborted(std::string_view arg1);

nlohmann::json::object_t taskCancelled(std::string_view arg1);

nlohmann::json::object_t taskRemoved(std::string_view arg1);

nlohmann::json::object_t taskPaused(std::string_view arg1);

nlohmann::json::object_t taskResumed(std::string_view arg1);

nlohmann::json::object_t taskProgressChanged(std::string_view arg1,
                                             uint64_t arg2);

} // namespace messages
} // namespace redfish
