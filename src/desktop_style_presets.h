#pragma once
#include "dock_settings.h"
#include "general_settings.h"
#include <string_view>

namespace snowdesktop
{
// Host-private presets edit only the fields advertised by the settings page.
// Layout and animation are deliberately separate actions.
inline bool ApplyDesktopStylePreset(std::wstring_view key, GeneralSettings& general, DockSettings& dock)
{
    if (key != L"native" && key != L"taskbar-dock" && key != L"island" && key != L"merged" && key != L"side")
        return false;
    dock.reserveScreenSpace = key == L"side";
    if (key == L"native")
    {
        general.dockEnabled = false;
        general.statusBar.enabled = false;
        dock.suppressSystemTaskbar = false;
        dock.systemTaskbarAutoHide = false;
        dock.systemTaskbarBackdropEnabled = true;
        return true;
    }
    general.dockEnabled = true;
    dock.systemTaskbarAutoHide = true;
    if (key == L"taskbar-dock")
    {
        general.statusBar.enabled = false;
        dock.suppressSystemTaskbar = false;
        dock.showOnlyWhenSummoned = false;
        dock.floatingEdgeSwipeEnabled = true;
        return true;
    }
    general.statusBar.enabled = true;
    general.statusBar.position = key == L"merged" ? DockPosition::Bottom : DockPosition::Top;
    general.statusBar.monitorScope = dock.monitorScope;
    dock.position = key == L"side" ? DockPosition::Left : DockPosition::Bottom;
    dock.edgeAttached = key != L"island";
    dock.showOnlyWhenSummoned = false;
    dock.allowDesktopContentOverlap = false;
    dock.suppressSystemTaskbar = true;
    return true;
}

inline bool ApplyDesktopStyleAnimations(std::wstring_view key, DockSettings& dock)
{
    if (key != L"island" && key != L"merged" && key != L"side") return false;
    dock.hoverEffect = key == L"island" ? 2 : 1;
    dock.launchEffect = key == L"island" ? 1 : 2;
    return true;
}
}
