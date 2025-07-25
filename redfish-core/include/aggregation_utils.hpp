// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

/****************************************************************
 *                 READ THIS WARNING FIRST
 * This is an auto-generated header which contains definitions
 * for Redfish DMTF defined schemas.
 * DO NOT modify this registry outside of running the
 * update_schemas.py script.  The definitions contained within
 * this file are owned by DMTF.  Any modifications to these files
 * should be first pushed to the relevant registry in the DMTF
 * github organization.
 ***************************************************************/
// clang-format off
#include <array>
#include <string_view>

namespace redfish
{
// Note that each URI actually begins with "/redfish/v1"
// They've been omitted to save space and reduce search time
constexpr std::array<std::string_view, 55> topCollections{
    "/AggregationService/Aggregates",
    "/AggregationService/AggregationSources",
    "/AggregationService/ConnectionMethods",
    "/AutomationNodes",
    "/Cables",
    "/Chassis",
    "/ComponentIntegrity",
    "/CompositionService/ActivePool",
    "/CompositionService/CompositionReservations",
    "/CompositionService/FreePool",
    "/CompositionService/ResourceBlocks",
    "/CompositionService/ResourceZones",
    "/EventService/Subscriptions",
    "/Fabrics",
    "/Facilities",
    "/JobService/JobDocuments",
    "/JobService/JobExecutors",
    "/JobService/Jobs",
    "/JobService/Log/Entries",
    "/KeyService/NVMeoFKeyPolicies",
    "/KeyService/NVMeoFSecrets",
    "/LicenseService/Licenses",
    "/Managers",
    "/NVMeDomains",
    "/PowerEquipment/ElectricalBuses",
    "/PowerEquipment/FloorPDUs",
    "/PowerEquipment/PowerShelves",
    "/PowerEquipment/RackPDUs",
    "/PowerEquipment/Switchgear",
    "/PowerEquipment/TransferSwitches",
    "/RegisteredClients",
    "/Registries",
    "/ResourceBlocks",
    "/Storage",
    "/StorageServices",
    "/StorageSystems",
    "/Systems",
    "/TaskService/TaskMonitors",
    "/TaskService/Tasks",
    "/TelemetryService/LogService/Entries",
    "/TelemetryService/MetricDefinitions",
    "/TelemetryService/MetricReportDefinitions",
    "/TelemetryService/MetricReports",
    "/TelemetryService/TelemetryData",
    "/TelemetryService/Triggers",
    "/ThermalEquipment/CDUs",
    "/ThermalEquipment/CoolingLoops",
    "/ThermalEquipment/HeatExchangers",
    "/ThermalEquipment/ImmersionUnits",
    "/ThermalEquipment/RPUs",
    "/UpdateService/ClientCertificates",
    "/UpdateService/FirmwareInventory",
    "/UpdateService/RemoteServerCertificates",
    "/UpdateService/RemoteServerSSHKeys",
    "/UpdateService/SoftwareInventory",
};
} // namespace redfish
