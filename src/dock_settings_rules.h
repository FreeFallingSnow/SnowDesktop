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

inline void NormalizeSummonOnlyDependencies(
    bool showOnlyWhenSummoned,
    bool& allowDesktopContentOverlap,
    bool& floatingEdgeSwipeEnabled) noexcept
{
    if (!showOnlyWhenSummoned)
        return;
    allowDesktopContentOverlap = true;
    floatingEdgeSwipeEnabled = true;
}

inline void DisableSummonOnlyWhenPrerequisiteDisabled(
    bool prerequisiteEnabled,
    bool& showOnlyWhenSummoned) noexcept
{
    if (!prerequisiteEnabled)
        showOnlyWhenSummoned = false;
}

inline bool ShouldReserveDesktopWorkArea(
    bool allowDesktopContentOverlap) noexcept
{
    return !allowDesktopContentOverlap;
}

} // namespace snowdesktop::dock_settings_rules
