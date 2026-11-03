#pragma once

#include <string>
#include <vector>

struct DockTrayRule
{
    bool operator==(const DockTrayRule&) const = default;
    std::wstring executable;
    bool matchPath = false;
    bool enabled = true;
    bool doubleClick = true;
};

namespace snowdesktop::dock_tray
{
enum class ClickAction { NormalDock, WaitForDoubleClick, OpenPicker, ActivateTray, Consume };

// Default persisted preferences, independent of matching and click policies.
std::vector<DockTrayRule> Defaults();
}
