#pragma once

#include "dock/dock_settings.h"
#include "general_settings.h"
#include "navigation/navigation_settings.h"

#include <cstring>

namespace snowdesktop::settings_update_rules
{

inline bool IsGeneralShortcutOnlyCommit(
    const GeneralSettings& before,
    const GeneralSettings& after) noexcept
{
    const bool shortcutChanged =
        before.forceRestartHotkeyEnabled != after.forceRestartHotkeyEnabled ||
        before.forceRestartHotkeyModifiers != after.forceRestartHotkeyModifiers ||
        before.forceRestartHotkeyVirtualKey != after.forceRestartHotkeyVirtualKey ||
        before.softwareDesktopHotkeyEnabled != after.softwareDesktopHotkeyEnabled ||
        before.softwareDesktopHotkeyModifiers != after.softwareDesktopHotkeyModifiers ||
        before.softwareDesktopHotkeyVirtualKey != after.softwareDesktopHotkeyVirtualKey ||
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
        before.showAdvancedSettings == after.showAdvancedSettings &&
        before.runtimeModules == after.runtimeModules &&
        before.autoStartEnabled == after.autoStartEnabled &&
        before.softwareDesktopEnabled == after.softwareDesktopEnabled &&
        before.diagnosticLogModules == after.diagnosticLogModules &&
        before.demoModeEnabled == after.demoModeEnabled &&
        before.doubleClickHideDesktop == after.doubleClickHideDesktop &&
        before.contextMenuExpandQuickActions == after.contextMenuExpandQuickActions &&
        before.contextMenuHidePageManagement == after.contextMenuHidePageManagement &&
        before.contextMenuHideMoreManagement == after.contextMenuHideMoreManagement &&
        before.quickNavTheme == after.quickNavTheme &&
        before.collectionPopupTheme == after.collectionPopupTheme &&
        before.quickNavigationAppearance == after.quickNavigationAppearance &&
        before.collectionPopupAppearance == after.collectionPopupAppearance &&
        before.globalQuickNavigationAppearance == after.globalQuickNavigationAppearance &&
        before.globalCollectionPopupAppearance == after.globalCollectionPopupAppearance &&
        before.font == after.font &&
        before.tooltipFontSize == after.tooltipFontSize &&
        before.dockEnabled == after.dockEnabled &&
        before.statusBar == after.statusBar &&
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

inline bool IsGeneralLoggingOnlyCommit(const GeneralSettings& before,
    const GeneralSettings& after) noexcept
{
    return before.diagnosticLogModules != after.diagnosticLogModules &&
        before.showAdvancedSettings == after.showAdvancedSettings &&
        before.runtimeModules == after.runtimeModules &&
        before.autoStartEnabled == after.autoStartEnabled &&
        before.softwareDesktopEnabled == after.softwareDesktopEnabled &&
        before.softwareDesktopHotkeyEnabled == after.softwareDesktopHotkeyEnabled &&
        before.softwareDesktopHotkeyModifiers == after.softwareDesktopHotkeyModifiers &&
        before.softwareDesktopHotkeyVirtualKey == after.softwareDesktopHotkeyVirtualKey &&
        before.forceRestartHotkeyEnabled == after.forceRestartHotkeyEnabled &&
        before.forceRestartHotkeyModifiers == after.forceRestartHotkeyModifiers &&
        before.forceRestartHotkeyVirtualKey == after.forceRestartHotkeyVirtualKey &&
        before.demoModeEnabled == after.demoModeEnabled &&
        before.doubleClickHideDesktop == after.doubleClickHideDesktop &&
        before.desktopPassthroughHotkeyEnabled == after.desktopPassthroughHotkeyEnabled &&
        before.desktopPassthroughHotkeyModifiers == after.desktopPassthroughHotkeyModifiers &&
        before.desktopPassthroughHotkeyVirtualKey == after.desktopPassthroughHotkeyVirtualKey &&
        before.pageNavigationKeyboardEnabled == after.pageNavigationKeyboardEnabled &&
        before.pageNavigationPreviousModifiers == after.pageNavigationPreviousModifiers &&
        before.pageNavigationPreviousVirtualKey == after.pageNavigationPreviousVirtualKey &&
        before.pageNavigationNextModifiers == after.pageNavigationNextModifiers &&
        before.pageNavigationNextVirtualKey == after.pageNavigationNextVirtualKey &&
        before.quickNavTheme == after.quickNavTheme &&
        before.collectionPopupTheme == after.collectionPopupTheme &&
        before.quickNavigationAppearance == after.quickNavigationAppearance &&
        before.collectionPopupAppearance == after.collectionPopupAppearance &&
        before.globalQuickNavigationAppearance == after.globalQuickNavigationAppearance &&
        before.globalCollectionPopupAppearance == after.globalCollectionPopupAppearance &&
        before.dockEnabled == after.dockEnabled &&
        before.statusBar == after.statusBar &&
        before.widgetDeveloperToolsEnabled == after.widgetDeveloperToolsEnabled &&
        before.animationMode == after.animationMode &&
        before.popupAnimationEffect == after.popupAnimationEffect &&
        before.animationSpeed == after.animationSpeed &&
        before.animationFrameLimit == after.animationFrameLimit &&
        before.animationEnergySaver == after.animationEnergySaver &&
        before.animationOnBattery == after.animationOnBattery &&
        before.calendarDisplay == after.calendarDisplay &&
        before.shellExtensions == after.shellExtensions &&
        before.contextMenuExpandQuickActions == after.contextMenuExpandQuickActions &&
        before.contextMenuHidePageManagement == after.contextMenuHidePageManagement &&
        before.contextMenuHideMoreManagement == after.contextMenuHideMoreManagement &&
        before.font == after.font &&
        before.tooltipFontSize == after.tooltipFontSize &&
        std::strcmp(before.language, after.language) == 0;
}

inline bool IsNavigationShortcutOnlyCommit(
    const NavigationSettings& before,
    const NavigationSettings& after)
{
    const bool shortcutChanged = before.enabled != after.enabled ||
        before.modifiers != after.modifiers ||
        before.virtualKey != after.virtualKey;
    NavigationSettings comparable = before;
    comparable.enabled = after.enabled;
    comparable.modifiers = after.modifiers;
    comparable.virtualKey = after.virtualKey;
    return shortcutChanged && comparable == after;
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
