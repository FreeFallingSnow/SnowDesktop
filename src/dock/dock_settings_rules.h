#pragma once

#include "dock_settings_defaults.h"
#include <cstdint>
#include <optional>

namespace snowdesktop::dock_settings_rules
{
int NormalizeEdgeHoverDelayMs(double delay) noexcept;

std::optional<bool> ResolveClassicTaskbarSystemLightTheme(
    int preference, bool appearanceEnabled, int contentTheme) noexcept;

bool ShouldRevealTaskbarForShellPanel(
    bool taskViewVisible, bool shellPanelVisible, bool onPanelMonitor) noexcept;

bool ShellPanelVisibility(bool launcherVisible, bool dockStartActive, bool otherPanelVisible) noexcept;

bool ProtectDockStart(bool dockEnabled, bool keepTaskbarHidden, bool hiddenTaskbarMode) noexcept;

std::uint64_t StartMenuLeaseDeadline(bool active, std::uint64_t now) noexcept;

void NormalizeAlwaysEnabledFeatures(
    bool& showRunningApps,
    bool& /*showWindowPreviews*/) noexcept;

bool IsDesktopContentOverlapEnabled(
    bool showOnlyWhenSummoned,
    bool allowDesktopContentOverlap) noexcept;

bool IsFloatingEdgeSwipeEnabled(
    bool showOnlyWhenSummoned,
    bool floatingEdgeSwipeEnabled) noexcept;

void MigrateSummonOnlyLinkedPreferencesToBase(
    bool showOnlyWhenSummoned,
    bool linkedPreferencesAreBase,
    bool& allowDesktopContentOverlap,
    bool& floatingEdgeSwipeEnabled) noexcept;

void DisableSummonOnlyWhenPrerequisiteDisabled(
    bool prerequisiteEnabled,
    bool& showOnlyWhenSummoned) noexcept;

bool ShouldReserveDesktopWorkArea(
    bool showOnlyWhenSummoned,
    bool allowDesktopContentOverlap) noexcept;
}
