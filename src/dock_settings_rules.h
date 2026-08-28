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

// Summon-only display temporarily requires both linked features. Resolve the
// effective state at use sites so the persisted user preferences stay intact.
inline bool IsDesktopContentOverlapEnabled(
    bool showOnlyWhenSummoned,
    bool allowDesktopContentOverlap) noexcept
{
    return showOnlyWhenSummoned || allowDesktopContentOverlap;
}

inline bool IsFloatingEdgeSwipeEnabled(
    bool showOnlyWhenSummoned,
    bool floatingEdgeSwipeEnabled) noexcept
{
    return showOnlyWhenSummoned || floatingEdgeSwipeEnabled;
}

inline void MigrateSummonOnlyLinkedPreferencesToBase(
    bool showOnlyWhenSummoned,
    bool linkedPreferencesAreBase,
    bool& allowDesktopContentOverlap,
    bool& floatingEdgeSwipeEnabled) noexcept
{
    if (!showOnlyWhenSummoned || linkedPreferencesAreBase ||
        !allowDesktopContentOverlap || !floatingEdgeSwipeEnabled)
        return;
    allowDesktopContentOverlap = false;
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
    bool showOnlyWhenSummoned,
    bool allowDesktopContentOverlap) noexcept
{
    return !IsDesktopContentOverlapEnabled(
        showOnlyWhenSummoned,
        allowDesktopContentOverlap);
}

} // namespace snowdesktop::dock_settings_rules
