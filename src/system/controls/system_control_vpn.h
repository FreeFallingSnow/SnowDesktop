#pragma once
#include "system_controls.h"
#include <windows.h>
#include <ras.h>

namespace snowdesktop::system_control::windows
{
// Replaceable RAS boundary. The native component preview never constructs this
// backend; deterministic drivers can supply fakes without a configured VPN.
struct VpnApi
{
    decltype(&RasEnumEntriesW) enumEntries = &RasEnumEntriesW;
    decltype(&RasGetEntryPropertiesW) entryProperties = &RasGetEntryPropertiesW;
    decltype(&RasEnumConnectionsW) enumConnections = &RasEnumConnectionsW;
    decltype(&RasGetConnectStatusW) connectStatus = &RasGetConnectStatusW;
    decltype(&RasHangUpW) hangUp = &RasHangUpW;
    decltype(&RasDialW) dial = &RasDialW;
    decltype(&RasGetEntryDialParamsW) entryDialParams = &RasGetEntryDialParamsW;
};
std::shared_ptr<Backend> CreateVpnBackend(VpnApi api = {});
}
