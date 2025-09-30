// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#include "redfish.hpp"

#include "bmcweb_config.h"

#include "account_service.hpp"
#include "aggregation_service.hpp"
#include "app.hpp"
#include "assembly.hpp"
#include "bios.hpp"
#include "cable.hpp"
#include "certificate_service.hpp"
#include "chassis.hpp"
#include "environment_metrics.hpp"
#include "ethernet.hpp"
#include "event_service.hpp"
#include "eventservice_sse.hpp"
#include "fabric_adapters.hpp"
#include "fabric_ports.hpp"
#include "fan.hpp"
#include "hypervisor_system.hpp"
#include "log_services.hpp"
#include "manager_diagnostic_data.hpp"
#include "manager_logservices_journal.hpp"
#include "managers.hpp"
#include "memory.hpp"
#include "message_registries.hpp"
#include "metadata.hpp"
#include "metric_report.hpp"
#include "metric_report_definition.hpp"
#include "network_protocol.hpp"
#include "odata.hpp"
#include "openbmc/openbmc_managers.hpp"
#include "pcie.hpp"
#include "power.hpp"
#include "power_subsystem.hpp"
#include "power_supply.hpp"
#include "processor.hpp"
#include "redfish_sessions.hpp"
#include "redfish_v1.hpp"
#include "roles.hpp"
#include "sensors.hpp"
#include "service_root.hpp"
#include "storage.hpp"
#include "systems.hpp"
#include "systems_logservices_hostlogger.hpp"
#include "systems_logservices_journal_eventlog.hpp"
#include "systems_logservices_postcodes.hpp"
#include "task.hpp"
#include "telemetry_service.hpp"
#include "thermal.hpp"
#include "thermal_metrics.hpp"
#include "thermal_subsystem.hpp"
#include "trigger.hpp"
#include "update_service.hpp"
#include "virtual_media.hpp"

namespace redfish
{

RedfishService::RedfishService(App& app)
{
    requestRoutesMetadata(app);
    requestRoutesOdata(app);

    requestAccountServiceRoutes(app);

    requestRoutesProcessorCollection(app);

    requestRoutesAssembly(app);

    requestRoutesProcessor(app);
    requestRoutesOperatingConfigCollection(app);
    requestRoutesOperatingConfig(app);
    requestRoutesMemoryCollection(app);
    requestRoutesMemory(app);

    requestRoutesSystems(app);

    requestRoutesBiosService(app);
    requestRoutesBiosReset(app);

    // Note, this must be the last route registered
    requestRoutesRedfish(app);

    requestRoutesOpenBmcManager(*this);

    validate();
}

} // namespace redfish
