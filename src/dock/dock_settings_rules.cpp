#include "dock_settings_rules.h"

#include <algorithm>
#include <cmath>

namespace snowdesktop::dock_settings_rules
{
int NormalizeEdgeHoverDelayMs(double delay) noexcept
{
    if (!std::isfinite(delay)) return kDefaultEdgeHoverDelayMs;
    return static_cast<int>(std::round(std::clamp(delay,
        static_cast<double>(kMinimumEdgeHoverDelayMs),
        static_cast<double>(kMaximumEdgeHoverDelayMs))));
}

std::optional<bool> ResolveClassicTaskbarSystemLightTheme(
    int preference, bool appearanceEnabled, int contentTheme) noexcept
{
    if (preference == 0) return true;
    if (preference == 1) return false;
    // Native appearance has no SnowDesktop foreground to match. Leave the
    // Windows theme alone. Dark text (contentTheme == 1) needs a light shell.
    if (!appearanceEnabled) return std::nullopt;
    return contentTheme == 1;
}

bool ShouldRevealTaskbarForShellPanel(
    bool taskViewVisible, bool shellPanelVisible, bool onPanelMonitor) noexcept
{
    return taskViewVisible || (shellPanelVisible && onPanelMonitor);
}

bool ShellPanelVisibility(bool launcherVisible, bool dockStartActive, bool otherPanelVisible) noexcept
{
    return otherPanelVisible || (launcherVisible && !dockStartActive);
}

bool ProtectDockStart(bool dockEnabled, bool keepTaskbarHidden, bool hiddenTaskbarMode) noexcept
{
    return dockEnabled && keepTaskbarHidden && hiddenTaskbarMode;
}

std::uint64_t StartMenuLeaseDeadline(bool active, std::uint64_t now) noexcept
{
    // Let Shell finish hiding the native bar after Start closes. Native edge
    // and keyboard reveal still release this guard immediately.
    return now + (active ? 3000 : 500);
}

void NormalizeAlwaysEnabledFeatures(
    bool& showRunningApps,
    bool& /*showWindowPreviews*/) noexcept
{
    showRunningApps = true;
    // Window previews remain a default-on user preference.
}

bool IsDesktopContentOverlapEnabled(
    bool showOnlyWhenSummoned,
    bool allowDesktopContentOverlap) noexcept
{
    return showOnlyWhenSummoned || allowDesktopContentOverlap;
}

bool IsFloatingEdgeSwipeEnabled(
    bool showOnlyWhenSummoned,
    bool floatingEdgeSwipeEnabled) noexcept
{
    return showOnlyWhenSummoned || floatingEdgeSwipeEnabled;
}

void MigrateSummonOnlyLinkedPreferencesToBase(
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

void DisableSummonOnlyWhenPrerequisiteDisabled(
    bool prerequisiteEnabled,
    bool& showOnlyWhenSummoned) noexcept
{
    if (!prerequisiteEnabled)
        showOnlyWhenSummoned = false;
}

bool ShouldReserveDesktopWorkArea(
    bool showOnlyWhenSummoned,
    bool allowDesktopContentOverlap) noexcept
{
    return !IsDesktopContentOverlapEnabled(
        showOnlyWhenSummoned,
        allowDesktopContentOverlap);
}
}
