#pragma once

#include "dock_settings_rules.h"
#include "personalization.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

constexpr float kDockMinimumScale = 0.50f;
constexpr float kDockMaximumScale = 1.00f;

inline float ClampDockScale(float scale)
{
    return std::clamp(scale, kDockMinimumScale, kDockMaximumScale);
}

enum class DockPosition
{
    Bottom = 0,
    Top = 1,
    Left = 2,
    Right = 3
};

enum class DockMonitorScope
{
    First = 0,
    Last = 1,
    All = 2
};

enum class SystemTaskbarBackdropRuntimeState
{
    Disabled,
    Loading,
    Active,
    Unsupported,
    Failed
};

enum class SystemTaskbarThemeMode
{
    Native = 0,
    FollowGlobal = 1,
    Dark = 2,
    Light = 3,
    GlassDark = 4,
    GlassLight = 5,
    AcrylicDark = 6,
    AcrylicLight = 7,
    Custom = 8,
    Transparent = 9
};

struct SystemTaskbarDynamicRule
{
    bool enabled = false;
    SystemTaskbarThemeMode themeMode = SystemTaskbarThemeMode::Native;
    int contentTheme = -1; // -1=follow selected theme
    PersonalizationSettings appearance =
        PersonalizationSettings::AcrylicDarkPreset();
};

struct SystemTaskbarTargetAppearance
{
    HWND taskbar = nullptr;
    bool enabled = false;
    PersonalizationSettings appearance =
        PersonalizationSettings::DarkPreset();
};

struct DockSettings
{
    DockPosition position = DockPosition::Bottom;
    bool edgeAttached = false;
    // Legacy field name retained in the persisted format. This now controls
    // only the hotkey trigger; edge-swipe invocation is independently enabled.
    bool floatingShortcutMode = true;
    UINT floatingHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT floatingHotkeyVirtualKey = 'D';
    bool floatingEdgeSwipeEnabled = true;
    DockMonitorScope monitorScope = DockMonitorScope::First;
    bool showWindowsButton = true;
    // Legacy persisted fields kept for layout compatibility. Running
    // applications and hover previews are now unconditional Dock features.
    bool showRunningApps = true;
    bool showWindowPreviews = true;
    bool showFrequentItems = false;
    bool keepWhenDesktopHidden = false;
    int frequentItemCount = 3;
    float thicknessScale = 1.0f;
    bool systemTaskbarAutoHide = false;
    int systemTaskbarAlignment = 1; // 0=靠左, 1=居中
    bool systemTaskbarBackdropEnabled = false;
    bool systemTaskbarFollowPersonalization = true;
    int systemTaskbarContentTheme = -1; // -1=跟随全局, 0=浅色, 1=深色
    PersonalizationSettings systemTaskbarAppearance =
        PersonalizationSettings::AcrylicDarkPreset();
    SystemTaskbarDynamicRule systemTaskbarVisibleWindow;
    SystemTaskbarDynamicRule systemTaskbarMaximizedWindow;
    SystemTaskbarDynamicRule systemTaskbarShellUi;
};

inline void NormalizeDockSettings(DockSettings& settings) noexcept
{
    snowdesktop::dock_settings_rules::NormalizeAlwaysEnabledFeatures(
        settings.showRunningApps,
        settings.showWindowPreviews);
}

std::wstring GetDockSettingsPath();
bool IsSystemTaskbarAutoHideEnabled();
bool SetSystemTaskbarAutoHideEnabled(bool enabled);
bool IsSystemTaskbarAlignmentCentered();
bool SetSystemTaskbarAlignmentCentered(bool centered);
bool IsWindowsSystemLightThemeEnabled();
bool SetWindowsSystemLightThemeEnabled(bool enabled);
bool RestartWindowsExplorer();
PersonalizationSettings MakeTransparentTaskbarAppearance();
SystemTaskbarBackdropRuntimeState GetSystemTaskbarBackdropRuntimeState();
void NotifySystemTaskbarCreated();
bool ApplySystemTaskbarBackdrop(bool hookEnabled, bool defaultEnabled,
    const PersonalizationSettings& defaultAppearance,
    const std::vector<SystemTaskbarTargetAppearance>& targets = {});
bool LoadDockSettings(const wchar_t* path, DockSettings& settings);
bool SaveDockSettings(const wchar_t* path, const DockSettings& settings);
