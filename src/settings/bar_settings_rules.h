#pragma once

struct DockLayoutSettings;
struct DockSettings;

namespace snowdesktop
{

struct StatusBarSettings;

struct BarSettingsAvailability
{
    bool anyMerged = false;
    bool allDockMerged = false;
    bool allStatusMerged = false;
    bool dockOnLastMonitor = false;
};

BarSettingsAvailability ResolveBarSettingsAvailability(bool dockEnabled,
    const DockLayoutSettings& dock, const StatusBarSettings& bar, int monitorCount);
int TaskbarDisplayMode(const DockSettings& dock);
bool CanSelectTaskbarDisplayMode(int mode, bool dockEnabled, bool taskbarAllowed) noexcept;
void SetTaskbarDisplayMode(DockSettings& dock, int mode);

} // namespace snowdesktop
