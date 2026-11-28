#include "bar_settings_rules.h"
#include "dock/dock_settings_types.h"
#include "system/status_bar/status_bar_settings_types.h"

#include <algorithm>

namespace snowdesktop
{

BarSettingsAvailability ResolveBarSettingsAvailability(bool dockEnabled,
    const DockSettings& dock, const StatusBarSettings& bar, int monitorCount)
{
    BarSettingsAvailability result;
    monitorCount = std::max(1, monitorCount);
    int docks = 0, bars = 0, merged = 0;
    for (int index = 0; index < monitorCount; ++index)
    {
        const auto role = GetBarMonitorRole(static_cast<size_t>(index), static_cast<size_t>(monitorCount));
        const bool hasDock = dockEnabled && BarMonitorVisible(dock.monitorScope, dock.monitorVisibility, role, monitorCount);
        const bool hasBar = bar.enabled && BarMonitorVisible(bar.monitorScope, bar.monitorVisibility, role, monitorCount);
        docks += hasDock; bars += hasBar;
        const auto dockLayout = dock.MonitorLayout(role);
        const auto barLayout = bar.MonitorLayout(role);
        merged += hasDock && hasBar && dockLayout.edgeAttached && dockLayout.position == barLayout.position;
        if (index == monitorCount - 1) result.dockOnLastMonitor = hasDock;
    }
    result.anyMerged = merged > 0;
    result.allDockMerged = docks > 0 && merged == docks;
    result.allStatusMerged = bars > 0 && merged == bars;
    return result;
}

int TaskbarDisplayMode(const DockSettings& dock)
{
    return dock.suppressSystemTaskbar ? (dock.suppressSystemTaskbarAllMonitors ? 3 : 2) :
        dock.systemTaskbarAutoHide ? 1 : 0;
}

bool CanSelectTaskbarDisplayMode(int mode, bool dockEnabled, bool taskbarAllowed) noexcept
{
    return mode >= 0 && mode <= 3 && (mode < 2 || taskbarAllowed) &&
        (mode != 2 || dockEnabled);
}

void SetTaskbarDisplayMode(DockSettings& dock, int mode)
{
    mode = std::clamp(mode, 0, 3);
    dock.suppressSystemTaskbar = mode >= 2;
    dock.suppressSystemTaskbarAllMonitors = mode == 3;
    dock.systemTaskbarAutoHide = mode != 0;
}

} // namespace snowdesktop
