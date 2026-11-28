#pragma once
#include "dock/dock_settings.h"
#include "settings/general_settings.h"
#include <optional>
#include <string_view>

namespace snowdesktop
{
// Host-private presets edit only the fields advertised by the settings page.
// Layout and animation are deliberately separate actions.
inline bool ApplyDesktopStylePreset(std::wstring_view key, GeneralSettings& general, DockSettings& dock,
    DockPosition companionPosition = DockPosition::Bottom, bool companionAttached = false)
{
    if (key != L"native" && key != L"taskbar-dock" && key != L"island" && key != L"merged" && key != L"side")
        return false;
    if (key == L"taskbar-dock" && companionPosition != DockPosition::Bottom &&
        companionPosition != DockPosition::Top && companionPosition != DockPosition::Left &&
        companionPosition != DockPosition::Right) return false;
    dock.reserveScreenSpace = key == L"side";
    dock.suppressSystemTaskbarAllMonitors = false;
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
        dock.position = companionPosition;
        dock.edgeAttached = companionAttached;
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

inline bool HasDesktopStyleMonitorOverrides(const GeneralSettings& general, const DockSettings& dock)
{
    const auto& bar = general.statusBar;
    return dock.lastMonitorOverride != DockMonitorOverride{} || dock.otherMonitorOverride != DockMonitorOverride{} ||
        bar.lastMonitorOverride != StatusBarMonitorOverride{} || bar.otherMonitorOverride != StatusBarMonitorOverride{} ||
        dock.monitorVisibility != kDefaultBarMonitorVisibility || bar.monitorVisibility != kDefaultBarMonitorVisibility;
}

inline std::optional<BarMonitorRole> DesktopStyleMonitorTarget(const GeneralSettings& general, const DockSettings& dock,
    std::optional<BarMonitorRole> requested)
{
    if (requested) return requested;
    return HasDesktopStyleMonitorOverrides(general, dock) ? std::optional{BarMonitorRole::First} : std::nullopt;
}

template <typename T, typename Override>
void SetDesktopStyleMonitorField(BarMonitorRole role, T& shared, Override& last, Override& other,
    std::optional<T> Override::* member, T previous, T next)
{
    if (previous == next) return;
    if (role == BarMonitorRole::First)
    {
        if (!(last.*member)) last.*member = previous;
        if (!(other.*member)) other.*member = previous;
        shared = next;
    }
    else
        (role == BarMonitorRole::Last ? last : other).*member = next;
}

inline void SetDesktopStyleMonitorVisibility(BarMonitorRole role, bool visible,
    DockMonitorScope scope, bool& enabled, int& visibility, std::size_t monitorCount)
{
    const bool legacyFallback = enabled && role == BarMonitorRole::First && monitorCount == 1 &&
        scope == DockMonitorScope::Last && visibility == kDefaultBarMonitorVisibility;
    const int bit = BarMonitorRoleBit(role);
    const int before = enabled ? BarMonitorVisibility(scope, visibility) : 0;
    const int after = visible ? before | bit : before & ~bit;
    if (after == before && !legacyFallback) return;
    enabled = after != 0;
    visibility = !legacyFallback && after == BarMonitorVisibility(scope, kDefaultBarMonitorVisibility)
        ? kDefaultBarMonitorVisibility : after;
}

inline bool DesktopStyleDockVisible(const GeneralSettings& general, const DockSettings& dock,
    std::optional<BarMonitorRole> target, std::size_t monitorCount)
{
    return general.dockEnabled && (!target ||
        BarMonitorVisible(dock.monitorScope, dock.monitorVisibility, *target, monitorCount));
}

inline bool ApplyScopedDesktopStylePreset(std::wstring_view key, GeneralSettings& general, DockSettings& dock,
    DockPosition companionPosition, bool companionAttached, std::optional<BarMonitorRole> requested,
    std::size_t monitorCount)
{
    const auto role = DesktopStyleMonitorTarget(general, dock, requested);
    if (!role)
        return ApplyDesktopStylePreset(key, general, dock, companionPosition, companionAttached);
    const auto oldDock = dock.MonitorLayout(*role);
    const auto oldBar = general.statusBar.MonitorLayout(*role);
    auto presetDock = dock;
    static_cast<DockLayoutSettings&>(presetDock) = oldDock;
    auto presetGeneral = general;
    presetGeneral.statusBar.position = oldBar.position;
    if (!ApplyDesktopStylePreset(key, presetGeneral, presetDock, companionPosition, companionAttached))
        return false;

    const auto setDock = [&](auto& shared, auto member, auto previous, auto next) {
        SetDesktopStyleMonitorField(*role, shared, dock.lastMonitorOverride, dock.otherMonitorOverride, member, previous, next);
    };
    setDock(dock.position, &DockMonitorOverride::position, oldDock.position, presetDock.position);
    setDock(dock.edgeAttached, &DockMonitorOverride::edgeAttached, oldDock.edgeAttached, presetDock.edgeAttached);
    setDock(dock.reserveScreenSpace, &DockMonitorOverride::reserveScreenSpace, oldDock.reserveScreenSpace, presetDock.reserveScreenSpace);
    setDock(dock.showOnlyWhenSummoned, &DockMonitorOverride::showOnlyWhenSummoned, oldDock.showOnlyWhenSummoned, presetDock.showOnlyWhenSummoned);
    setDock(dock.allowDesktopContentOverlap, &DockMonitorOverride::allowDesktopContentOverlap, oldDock.allowDesktopContentOverlap, presetDock.allowDesktopContentOverlap);
    auto& bar = general.statusBar;
    SetDesktopStyleMonitorField(*role, bar.position, bar.lastMonitorOverride, bar.otherMonitorOverride,
        &StatusBarMonitorOverride::position, oldBar.position, presetGeneral.statusBar.position);
    // Keep a single connected screen's legacy last-screen fallback until a
    // preset explicitly changes that screen's visibility.
    if (DesktopStyleDockVisible(general, dock, role, monitorCount) != presetGeneral.dockEnabled)
        SetDesktopStyleMonitorVisibility(*role, presetGeneral.dockEnabled,
            dock.monitorScope, general.dockEnabled, dock.monitorVisibility, monitorCount);
    if ((bar.enabled && BarMonitorVisible(bar.monitorScope, bar.monitorVisibility, *role, monitorCount)) != presetGeneral.statusBar.enabled)
        SetDesktopStyleMonitorVisibility(*role, presetGeneral.statusBar.enabled, bar.monitorScope, bar.enabled, bar.monitorVisibility, monitorCount);
    return true;
}

inline bool ApplyScopedDesktopStyleAnimations(std::wstring_view key, const GeneralSettings& general,
    DockSettings& dock, std::optional<BarMonitorRole> requested)
{
    const auto role = DesktopStyleMonitorTarget(general, dock, requested);
    if (!role) return ApplyDesktopStyleAnimations(key, dock);
    const auto old = dock.MonitorLayout(*role);
    auto recommended = dock;
    if (!ApplyDesktopStyleAnimations(key, recommended)) return false;
    SetDesktopStyleMonitorField(*role, dock.hoverEffect, dock.lastMonitorOverride, dock.otherMonitorOverride,
        &DockMonitorOverride::hoverEffect, old.hoverEffect, recommended.hoverEffect);
    SetDesktopStyleMonitorField(*role, dock.launchEffect, dock.lastMonitorOverride, dock.otherMonitorOverride,
        &DockMonitorOverride::launchEffect, old.launchEffect, recommended.launchEffect);
    return true;
}

inline bool NeedsDesktopStyleAnimations(std::wstring_view key, const DockSettings& current)
{
    auto recommended = current;
    return ApplyDesktopStyleAnimations(key, recommended) &&
        (animation::NormalizeHoverEffect(current.hoverEffect) != recommended.hoverEffect ||
            animation::NormalizeLaunchEffect(current.launchEffect) != recommended.launchEffect);
}
}
