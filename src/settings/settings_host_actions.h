#pragma once

#include "settings_action_result.h"
#include "settings_fwd.h"
#include "dock/dock_layout_settings.h"
#include <windows.h>

#include <cstdint>
#include <string>

namespace snowdesktop
{

/**
 * Application-side adapter replacing per-control window callbacks.
 *
 * During migration, an adapter can forward these consolidated events to the
 * existing DesktopApp callbacks.  The WinUI backend consumes the same events.
 */
class SettingsHostActions
{
public:
    enum class HotkeyTarget : std::uint8_t
    {
        None,
        QuickNavigation,
        DesktopPassthrough,
        FloatingDock,
        PagePrevious,
        PageNext,
        SoftwareDesktop,
        ForceRestart,
    };

    enum class Action : std::uint8_t
    {
        ApplyLanguage,
        RegisterHotkeys,
        ApplyDock,
        ApplyTaskbar,
        ApplyDesktopLayout,
        ApplyCategories,
        RefreshDesktop,
        RefreshWidgets,
        AddWidgetToDesktop,
        ReloadWidgetInstance,
        RestartExplorer,
        RestartApplication,
        ExitApplication,
        OpenDataDirectory,
        SetAutoStartEnabled,
        CheckForUpdates,
        // Value 16 belonged to the removed network-update cancellation.
        OpenProject = 17,
        OpenLicense,
        OpenThirdPartyNotices,
        SetAnimationDiagnostics,
        TriggerCrashTest,
        ProbeHotkeyAvailability,
        SetTemporaryGridInitialization,
        StartUsageGuidePractice,
        SetUsageGuideExpanded,
        SetDebugProfileEnabled,
        SetDebugDesktopDirectory,
        ClearDebugProfile,
        ApplyDesktopStylePreset,
        ApplyDesktopStyleAnimations,
        GetDockRunningApplications,
        GetDockTrayApplications,
        CreateDebugEnvironment,
        SelectDebugEnvironment,
        RenameDebugEnvironment,
    };

    struct Request
    {
        Action action = Action::RefreshDesktop;
        std::wstring widgetInstanceId;
        std::wstring value;
        bool boolValue = false;
        HotkeyTarget hotkeyTarget = HotkeyTarget::None;
        UINT modifiers = 0;
        UINT virtualKey = 0;
        // Host-private desktop style draft, applied atomically with the preset.
        DockPosition desktopStyleDockPosition = DockPosition::Bottom;
        bool desktopStyleDockAttached = false;
    };

    virtual ~SettingsHostActions() = default;

    virtual SettingsActionResult OnSettingsPreview(
        const SettingsSnapshot& snapshot,
        SettingsDomain domains) = 0;
    /** Apply host-owned state before the controller writes native stores. */
    virtual SettingsActionResult OnSettingsCommitted(
        const SettingsSnapshot& snapshot,
        SettingsDomain domains) = 0;
    virtual SettingsActionResult OnSettingsRouteChanged(
        const SettingsRoute& route) = 0;
    virtual SettingsActionResult Invoke(const Request& request) = 0;
};

} // namespace snowdesktop
