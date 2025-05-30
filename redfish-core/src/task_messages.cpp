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
#include "task_messages.hpp"

#include "registries.hpp"
#include "registries/task_event_message_registry.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Clang can't seem to decide whether this header needs to be included or not,
// and is inconsistent.  Include it for now
// NOLINTNEXTLINE(misc-include-cleaner)
#include <utility>

namespace redfish
{

namespace messages
{

static nlohmann::json::object_t getLog(
    redfish::registries::TaskEvent::Index name,
    std::span<const std::string_view> args)
{
    size_t index = static_cast<size_t>(name);
    if (index >= redfish::registries::TaskEvent::registry.size())
    {
        return {};
    }
    return getLogFromRegistry(redfish::registries::TaskEvent::header,
                              redfish::registries::TaskEvent::registry, index,
                              args);
}

/**
 * @internal
 * @brief Formats TaskStarted message into JSON
 *
 * See header file for more information
 * @endinternal
 */
nlohmann::json::object_t taskStarted(std::string_view arg1)
{
    return getLog(redfish::registries::TaskEvent::Index::taskStarted,
                  std::to_array({arg1}));
}

/**
 * @internal
 * @brief Formats TaskCompletedOK message into JSON
 *
 * See header file for more information
 * @endinternal
 */
nlohmann::json::object_t taskCompletedOK(std::string_view arg1)
{
    return getLog(redfish::registries::TaskEvent::Index::taskCompletedOK,
                  std::to_array({arg1}));
}

/**
 * @internal
 * @brief Formats TaskCompletedWarning message into JSON
 *
 * See header file for more information
 * @endinternal
 */
nlohmann::json::object_t taskCompletedWarning(std::string_view arg1)
{
    return getLog(redfish::registries::TaskEvent::Index::taskCompletedWarning,
                  std::to_array({arg1}));
}

/**
 * @internal
 * @brief Formats TaskAborted message into JSON
 *
 * See header file for more information
 * @endinternal
 */
nlohmann::json::object_t taskAborted(std::string_view arg1)
{
    return getLog(redfish::registries::TaskEvent::Index::taskAborted,
                  std::to_array({arg1}));
}

/**
 * @internal
 * @brief Formats TaskCancelled message into JSON
 *
 * See header file for more information
 * @endinternal
 */
nlohmann::json::object_t taskCancelled(std::string_view arg1)
{
    return getLog(redfish::registries::TaskEvent::Index::taskCancelled,
                  std::to_array({arg1}));
}

/**
 * @internal
 * @brief Formats TaskRemoved message into JSON
 *
 * See header file for more information
 * @endinternal
 */
nlohmann::json::object_t taskRemoved(std::string_view arg1)
{
    return getLog(redfish::registries::TaskEvent::Index::taskRemoved,
                  std::to_array({arg1}));
}

/**
 * @internal
 * @brief Formats TaskPaused message into JSON
 *
 * See header file for more information
 * @endinternal
 */
nlohmann::json::object_t taskPaused(std::string_view arg1)
{
    return getLog(redfish::registries::TaskEvent::Index::taskPaused,
                  std::to_array({arg1}));
}

/**
 * @internal
 * @brief Formats TaskResumed message into JSON
 *
 * See header file for more information
 * @endinternal
 */
nlohmann::json::object_t taskResumed(std::string_view arg1)
{
    return getLog(redfish::registries::TaskEvent::Index::taskResumed,
                  std::to_array({arg1}));
}

/**
 * @internal
 * @brief Formats TaskProgressChanged message into JSON
 *
 * See header file for more information
 * @endinternal
 */
nlohmann::json::object_t taskProgressChanged(std::string_view arg1,
                                             uint64_t arg2)
{
    std::string arg2Str = std::to_string(arg2);
    return getLog(redfish::registries::TaskEvent::Index::taskProgressChanged,
                  std::to_array<std::string_view>({arg1, arg2Str}));
}

} // namespace messages
} // namespace redfish
