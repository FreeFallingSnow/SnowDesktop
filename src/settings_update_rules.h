#pragma once

#include "dock_settings.h"
#include "general_settings.h"
#include "navigation_settings.h"

#include <cstring>

namespace snowdesktop::settings_update_rules
{

inline bool IsGeneralShortcutOnlyCommit(
    const GeneralSettings& before,
    const GeneralSettings& after) noexcept
{
    const bool shortcutChanged =
        before.desktopPassthroughHotkeyEnabled !=
            after.desktopPassthroughHotkeyEnabled ||
        before.desktopPassthroughHotkeyModifiers !=
            after.desktopPassthroughHotkeyModifiers ||
        before.desktopPassthroughHotkeyVirtualKey !=
            after.desktopPassthroughHotkeyVirtualKey ||
        before.pageNavigationKeyboardEnabled !=
            after.pageNavigationKeyboardEnabled ||
        before.pageNavigationPreviousModifiers !=
            after.pageNavigationPreviousModifiers ||
        before.pageNavigationPreviousVirtualKey !=
            after.pageNavigationPreviousVirtualKey ||
        before.pageNavigationNextModifiers !=
            after.pageNavigationNextModifiers ||
        before.pageNavigationNextVirtualKey !=
            after.pageNavigationNextVirtualKey;
    return shortcutChanged &&
        before.autoStartEnabled == after.autoStartEnabled &&
        before.softwareDesktopEnabled == after.softwareDesktopEnabled &&
        before.demoModeEnabled == after.demoModeEnabled &&
        before.doubleClickHideDesktop == after.doubleClickHideDesktop &&
        before.quickNavTheme == after.quickNavTheme &&
        before.collectionPopupTheme == after.collectionPopupTheme &&
        before.quickNavigationAppearance == after.quickNavigationAppearance &&
        before.collectionPopupAppearance == after.collectionPopupAppearance &&
        before.dockEnabled == after.dockEnabled &&
        before.animationMode == after.animationMode &&
        before.popupAnimationEffect == after.popupAnimationEffect &&
        before.animationSpeed == after.animationSpeed &&
        before.animationFrameLimit == after.animationFrameLimit &&
        before.animationEnergySaver == after.animationEnergySaver &&
        before.animationOnBattery == after.animationOnBattery &&
        before.widgetDeveloperToolsEnabled ==
            after.widgetDeveloperToolsEnabled &&
        std::strcmp(before.language, after.language) == 0;
}

inline bool IsNavigationShortcutOnlyCommit(
    const NavigationSettings& before,
    const NavigationSettings& after) noexcept
{
    const bool shortcutChanged = before.enabled != after.enabled ||
        before.modifiers != after.modifiers ||
        before.virtualKey != after.virtualKey;
    return shortcutChanged &&
        before.desktopViewMode == after.desktopViewMode;
}

inline bool IsFloatingDockShortcutOnlyCommit(
    const DockSettings& before,
    const DockSettings& after) noexcept
{
    const bool shortcutChanged =
        before.floatingShortcutMode != after.floatingShortcutMode ||
        before.floatingHotkeyModifiers != after.floatingHotkeyModifiers ||
        before.floatingHotkeyVirtualKey != after.floatingHotkeyVirtualKey;
    if (!shortcutChanged)
        return false;

    DockSettings comparableBefore = before;
    DockSettings comparableAfter = after;
    NormalizeDockSettings(comparableBefore);
    NormalizeDockSettings(comparableAfter);
    comparableBefore.floatingShortcutMode =
        comparableAfter.floatingShortcutMode;
    comparableBefore.floatingHotkeyModifiers =
        comparableAfter.floatingHotkeyModifiers;
    comparableBefore.floatingHotkeyVirtualKey =
        comparableAfter.floatingHotkeyVirtualKey;
    return comparableBefore == comparableAfter;
}

} // namespace snowdesktop::settings_update_rules
