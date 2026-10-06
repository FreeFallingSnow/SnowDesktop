#pragma once

#include "dock_settings_rules.h"
#include "dock_layout_settings.h"
#include "theme/personalization.h"
#include "settings/animation_settings.h"
#include "taskbar_hook/taskbar_autohide_trace.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

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
    bool operator==(const SystemTaskbarDynamicRule&) const = default;

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
    bool protectAutoHideActivation = false;
    bool shellPanelVisible = false;
    bool suppressTaskbar = false;
};

inline void ConfigureSystemTaskbarTargetProtection(
    SystemTaskbarTargetAppearance& target,
    bool suppressionRequested, bool protectActivation, bool hasDock) noexcept
{
    // Permanent hiding follows Dock placement. Passive activation from a
    // minimized application can reach any taskbar, including screens without
    // a Dock; intentional shell-panel access still releases that protection.
    target.suppressTaskbar = suppressionRequested && hasDock;
    target.protectAutoHideActivation = protectActivation && !target.shellPanelVisible;
}

struct DockSettings : DockLayoutSettings
{
    bool operator==(const DockSettings&) const = default;

    // Legacy field name retained in the persisted format. This now controls
    // only the hotkey trigger; edge-swipe invocation is independently enabled.
    bool floatingShortcutMode = true;
    UINT floatingHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT floatingHotkeyVirtualKey = 'D';
    bool floatingEdgeSwipeEnabled = true;
    int edgeRevealGesture = 0; // 0=swipe, 1=hover; preserve existing swipe preferences.
    bool floatingEdgeSwipeBlockFullscreen = true;
    // Running applications remain enabled; task thumbnails are optional.
    // Keep the persisted names for compatibility with existing preferences.
    bool showRunningApps = true;
    bool showWindowPreviews = true;
    bool followComponentAppearance = true;
    int appearancePreset = kAppearancePresetCustom;
    PersonalizationSettings customAppearance;
    int hoverEffect = 2;
    float hoverScale = 1.28f;
    int launchEffect = 1;
    int windowEffect = 1;
    bool systemTaskbarAutoHide = false;
    bool suppressSystemTaskbar = false;
    int systemTaskbarAlignment = 1; // 0=靠左, 1=居中
    bool systemTaskbarBackdropEnabled = false;
    bool systemTaskbarFollowPersonalization = true;
    int systemTaskbarContentTheme = -1; // -1=跟随全局, 0=浅色, 1=深色
    // Windows 10 shares one shell theme across taskbars and system panels.
    int classicTaskbarSystemTheme = -1; // -1=match appearance, 0=light, 1=dark
    PersonalizationSettings systemTaskbarAppearance =
        PersonalizationSettings::AcrylicDarkPreset();
    SystemTaskbarDynamicRule systemTaskbarVisibleWindow;
    SystemTaskbarDynamicRule systemTaskbarMaximizedWindow;
    SystemTaskbarDynamicRule systemTaskbarShellUi;
};

inline bool ShowDockWindowsButton(const DockSettings& settings) noexcept
{
    return settings.showWindowsButton;
}

inline bool ShouldProtectAutoHideTaskbar(const DockSettings& settings,
    bool dockEnabled, bool autoHideEnabled) noexcept
{
    // floatingShortcutMode enables a summon hotkey; it does not replace the
    // ordinary bottom Dock. Appearance preferences are independent as well.
    // Permanent hiding temporarily enables Shell auto-hide while preserving
    // the user's original preference for restoration. Protect that override
    // too, including taskbars on monitors without a Dock.
    return dockEnabled && settings.position == DockPosition::Bottom &&
        (autoHideEnabled || settings.suppressSystemTaskbar);
}

inline PersonalizationSettings ResolveDockAppearance(const DockSettings& settings, const PersonalizationSettings& global)
{
    auto value = settings.followComponentAppearance ? global :
        settings.appearancePreset == kAppearancePresetCustom ? settings.customAppearance : MakeAppearancePreset(settings.appearancePreset);
    value.cornerRadius = global.cornerRadius;
    return value;
}

inline void SelectDockAppearance(DockSettings& settings, bool followGlobal,
    int preset, const PersonalizationSettings& global)
{
    if (!followGlobal && preset == kAppearancePresetCustom &&
        (settings.followComponentAppearance || settings.appearancePreset != kAppearancePresetCustom))
    {
        settings.customAppearance = ResolveDockAppearance(settings, global);
        settings.customAppearance.backgroundPreset = kAppearancePresetCustom;
    }
    settings.followComponentAppearance = followGlobal;
    if (!followGlobal) settings.appearancePreset = preset;
}

inline void NormalizeDockSettings(DockSettings& settings) noexcept
{
    switch (settings.appearancePreset)
    {
    case kAppearancePresetDark: case kAppearancePresetLight:
    case kAppearancePresetGlassDark: case kAppearancePresetGlassLight:
    case kAppearancePresetGlassTransparent:
    case kAppearancePresetAcrylicDark: case kAppearancePresetAcrylicLight:
    case kAppearancePresetCustom: break;
    default: settings.appearancePreset = kAppearancePresetCustom; break;
    }
    settings.hoverEffect = snowdesktop::animation::NormalizeHoverEffect(settings.hoverEffect);
    settings.hoverScale = snowdesktop::animation::NormalizeHoverScale(settings.hoverScale);
    settings.launchEffect = snowdesktop::animation::NormalizeLaunchEffect(settings.launchEffect);
    settings.windowEffect = snowdesktop::animation::NormalizeWindowEffect(settings.windowEffect);
    settings.edgeRevealGesture = settings.edgeRevealGesture == 1 ? 1 : 0;
    snowdesktop::dock_settings_rules::NormalizeAlwaysEnabledFeatures(
        settings.showRunningApps,
        settings.showWindowPreviews);
}

std::wstring GetDockSettingsPath();
bool IsSystemTaskbarAutoHideEnabled();
bool RequestSystemTaskbarAutoHideEnabled(bool enabled);
bool IsSystemTaskbarAlignmentCentered();
bool RequestSystemTaskbarAlignmentCentered(bool centered);
bool IsWindowsSystemLightThemeEnabled();
bool RequestWindowsSystemLightThemeEnabled(bool enabled);
bool RestartWindowsExplorer();
inline PersonalizationSettings MakeTransparentTaskbarAppearance()
{
    auto appearance = PersonalizationSettings::DarkPreset();
    appearance.widgetBgR = appearance.widgetBgG = appearance.widgetBgB = 0.f;
    appearance.widgetBorderR = appearance.widgetBorderG = appearance.widgetBorderB = 0.f;
    appearance.widgetAlpha = appearance.widgetBorderAlpha = appearance.gradientEndA = 0.f;
    appearance.widgetEdgeHighlightEnabled = false;
    appearance.backgroundPreset = kAppearancePresetTaskbarTransparent;
    appearance.glassEnabled = appearance.acrylicEnabled = false;
    return appearance;
}
SystemTaskbarBackdropRuntimeState GetSystemTaskbarBackdropRuntimeState();
SystemTaskbarBackdropRuntimeState GetSystemTaskbarSuppressionRuntimeState();
bool IsClassicSystemTaskbar();
void NotifySystemTaskbarCreated();
LONG DrainSystemTaskbarAutoHideTrace(
    std::array<snowdesktop::taskbar_hook::AutoHideTraceRecord,
        snowdesktop::taskbar_hook::kAutoHideTraceCapacity>& records, LONG& dropped);
bool ApplySystemTaskbarBackdrop(bool hookEnabled, bool defaultEnabled,
    const PersonalizationSettings& defaultAppearance,
    const std::vector<SystemTaskbarTargetAppearance>& targets = {},
    bool appearanceEnabled = true, bool suppressTaskbar = false);
bool LoadDockSettings(const wchar_t* path, DockSettings& settings);
bool SaveDockSettings(const wchar_t* path, const DockSettings& settings);
