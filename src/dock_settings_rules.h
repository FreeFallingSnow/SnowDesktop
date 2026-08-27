#pragma once

namespace snowdesktop::dock_settings_rules
{

inline void NormalizeAlwaysEnabledFeatures(
    bool& showRunningApps,
    bool& showWindowPreviews) noexcept
{
    showRunningApps = true;
    showWindowPreviews = true;
}

inline bool ShouldReserveDesktopWorkArea(
    bool allowDesktopContentOverlap) noexcept
{
    return !allowDesktopContentOverlap;
}

} // namespace snowdesktop::dock_settings_rules
