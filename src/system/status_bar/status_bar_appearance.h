#pragma once
#include "status_bar_settings.h"
#include <array>

namespace snowdesktop
{
// Keep every existing persisted mode stable when adding transparent choices.
inline constexpr int kStatusBarThemeTransparentDarkText = 7;
inline constexpr int kStatusBarThemeTransparentLightText = 8;
inline constexpr std::array<int, 10> StatusBarThemeModes{-1, 0, 1, 5, 6, 2, 3,
    kStatusBarThemeTransparentDarkText, kStatusBarThemeTransparentLightText, 4};
inline int StatusBarThemeSelection(int mode)
{
    const auto found = std::find(StatusBarThemeModes.begin(), StatusBarThemeModes.end(), mode);
    return found == StatusBarThemeModes.end() ? 0 : static_cast<int>(found - StatusBarThemeModes.begin());
}
// A bar follows the actual desktop appearance. Popup presets deliberately use
// different opacity/material defaults and must not be substituted here.
inline PersonalizationSettings ResolveStatusBarAppearance(const SurfaceTheme& theme,
    const PersonalizationSettings& global)
{
    if (theme.mode < 0) return global;
    if (theme.mode == 4) return theme.appearance;
    if (theme.mode == kStatusBarThemeTransparentDarkText || theme.mode == kStatusBarThemeTransparentLightText)
    {
        // Match the taskbar's transparent material while choosing text contrast
        // explicitly; leave all glass, gradients and edge effects disabled.
        auto appearance = MakeAppearancePreset(theme.mode == kStatusBarThemeTransparentDarkText ?
            kAppearancePresetLight : kAppearancePresetDark);
        appearance.backgroundPreset = kAppearancePresetTaskbarTransparent;
        appearance.widgetBgR = appearance.widgetBgG = appearance.widgetBgB = appearance.widgetAlpha = 0;
        appearance.widgetBorderR = appearance.widgetBorderG = appearance.widgetBorderB = appearance.widgetBorderAlpha = 0;
        appearance.gradientEndA = 0;
        appearance.glassEnabled = appearance.acrylicEnabled = appearance.widgetEdgeHighlightEnabled = false;
        appearance.panelGradient.enabled = false;
        return appearance;
    }
    if (theme.mode == 5 || theme.mode == 6)
        return MakeAppearancePreset(theme.mode == 5 ? kAppearancePresetGlassDark : kAppearancePresetGlassLight);
    return MakeAppearancePreset(AppearancePresetFromFourThemeSelection(theme.mode));
}

// Supply the taskbar monitor's state for the bar's own display. Resolving a
// transient scene never changes the user's default or saved custom appearance.
struct StatusBarSceneState
{
    bool noWindow = false;
    bool maximizedWindow = false;
};

inline const SurfaceTheme& ResolveStatusBarSceneTheme(const StatusBarSettings& settings,
    const StatusBarSceneState& state)
{
    if (settings.maximizedWindow.enabled && state.maximizedWindow) return settings.maximizedWindow.theme;
    if (settings.noWindow.enabled && state.noWindow && !state.maximizedWindow) return settings.noWindow.theme;
    return settings.theme;
}

inline PersonalizationSettings ResolveStatusBarAppearance(const StatusBarSettings& settings,
    const PersonalizationSettings& global, const StatusBarSceneState& state)
{
    return ResolveStatusBarAppearance(ResolveStatusBarSceneTheme(settings, state), global);
}
}
