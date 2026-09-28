#pragma once
#include <algorithm>
#include "dock_settings.h"
#include "status_bar_settings.h"

namespace snowdesktop
{
struct BarSettingsAvailability
{
    bool anyMerged = false;
    bool allDockMerged = false;
    bool allStatusMerged = false;
    bool dockOnLastMonitor = false;
};
inline BarSettingsAvailability ResolveBarSettingsAvailability(bool dockEnabled,
    const DockLayoutSettings& dock, const StatusBarSettings& bar, int monitorCount)
{
    BarSettingsAvailability result;
    monitorCount = std::max(1, monitorCount);
    const auto contains = [monitorCount](DockMonitorScope scope, int index) {
        return scope == DockMonitorScope::All ||
            (scope == DockMonitorScope::First ? index == 0 : index == monitorCount - 1);
    };
    int docks = 0, bars = 0, merged = 0;
    for (int index = 0; index < monitorCount; ++index)
    {
        const bool hasDock = dockEnabled && contains(dock.monitorScope, index);
        const bool hasBar = bar.enabled && contains(bar.monitorScope, index);
        docks += hasDock; bars += hasBar;
        merged += hasDock && hasBar && dock.edgeAttached && dock.position == bar.position;
        if (index == monitorCount - 1) result.dockOnLastMonitor = hasDock;
    }
    result.anyMerged = merged > 0;
    result.allDockMerged = docks > 0 && merged == docks;
    result.allStatusMerged = bars > 0 && merged == bars;
    return result;
}
inline int TaskbarDisplayMode(const DockSettings& dock)
{
    return dock.suppressSystemTaskbar ? 2 : dock.systemTaskbarAutoHide ? 1 : 0;
}
inline void SetTaskbarDisplayMode(DockSettings& dock, int mode)
{
    dock.suppressSystemTaskbar = mode == 2;
    dock.systemTaskbarAutoHide = mode != 0;
}
}
