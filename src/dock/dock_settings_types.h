#pragma once

#include "dock_layout_settings.h"
#include "dock_attention_dot.h"
#include "dock_fullscreen_types.h"
#include "dock_tray_types.h"
#include "dock_settings_defaults.h"
#include "theme/personalization_types.h"

#include <windows.h>
#include <vector>

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

struct DockSettings : DockLayoutSettings
{
    bool operator==(const DockSettings&) const = default;

    // Legacy field name retained in the persisted format. This now controls
    // only the hotkey trigger; edge-swipe invocation is independently enabled.
    bool floatingShortcutMode = true;
    UINT floatingHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT floatingHotkeyVirtualKey = 'D';
    bool floatingEdgeSwipeEnabled = true;
    bool summonOnAppMinimize = true;
    int edgeRevealGesture = 0; // 0=swipe, 1=hover; preserve existing swipe preferences.
    int edgeHoverDelayMs = snowdesktop::dock_settings_rules::kDefaultEdgeHoverDelayMs;
    DockFullscreenPolicy fullscreenPolicy = DockFullscreenPolicy::FullProtection;
    std::vector<DockFullscreenException> fullscreenExceptions;
    std::vector<DockTrayRule> trayApplications = snowdesktop::dock_tray::Defaults();
    // Running applications remain enabled; task thumbnails are optional.
    // Keep the persisted names for compatibility with existing preferences.
    bool showRunningApps = true;
    bool showWindowPreviews = true;
    bool singleClickLaunchItems = false;
    bool startMenuKeepTaskbarHidden = false;
    bool followComponentAppearance = true;
    bool followComponentCornerRadius = true;
    float cornerRadius = 12.0f;
    float attentionDotSizeScale = snowdesktop::dock_attention_dot::kDefaultSizeScale;
    float attentionDotOffset = snowdesktop::dock_attention_dot::kDefaultOffset;
    int appearancePreset = kAppearancePresetCustom;
    PersonalizationSettings customAppearance;
    int hoverEffect = 2;
    float hoverScale = 1.28f;
    int launchEffect = 1;
    int windowEffect = 1;
    bool systemTaskbarAutoHide = false;
    bool suppressSystemTaskbar = false;
    bool suppressSystemTaskbarAllMonitors = false;
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
