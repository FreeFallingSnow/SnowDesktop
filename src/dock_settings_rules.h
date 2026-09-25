#pragma once

#include <optional>

namespace snowdesktop::dock_settings_rules
{

inline std::optional<bool> ResolveClassicTaskbarSystemLightTheme(
    int preference, bool appearanceEnabled, int contentTheme) noexcept
{
    if (preference == 0) return true;
    if (preference == 1) return false;
    // Native appearance has no SnowDesktop foreground to match. Leave the
    // Windows theme alone. Dark text (contentTheme == 1) needs a light shell.
    if (!appearanceEnabled) return std::nullopt;
    return contentTheme == 1;
}

inline bool ShouldRevealTaskbarForShellPanel(
    bool taskViewVisible, bool shellPanelVisible, bool onPanelMonitor) noexcept
{
    return taskViewVisible || (shellPanelVisible && onPanelMonitor);
}

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
