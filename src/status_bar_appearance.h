#pragma once
#include "status_bar_settings.h"
#include <array>

namespace snowdesktop
{
// Keep the persisted 0..4 modes unchanged when inserting glass choices.
inline constexpr std::array<int, 8> StatusBarThemeModes{-1, 0, 1, 5, 6, 2, 3, 4};
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
    if (theme.mode == 5 || theme.mode == 6)
        return MakeAppearancePreset(theme.mode == 5 ? kAppearancePresetGlassDark : kAppearancePresetGlassLight);
    return MakeAppearancePreset(AppearancePresetFromFourThemeSelection(theme.mode));
}

// Supply the taskbar monitor's state for the bar's own display. Resolving a
// transient scene never changes the user's default or saved custom appearance.
struct StatusBarSceneState
{
    bool shellUi = false;
    bool maximizedWindow = false;
    bool visibleWindow = false;
};

inline const SurfaceTheme& ResolveStatusBarSceneTheme(const StatusBarSettings& settings,
    const StatusBarSceneState& state)
{
    if (settings.shellUi.enabled && state.shellUi) return settings.shellUi.theme;
    if (settings.maximizedWindow.enabled && state.maximizedWindow) return settings.maximizedWindow.theme;
    if (settings.visibleWindow.enabled && state.visibleWindow) return settings.visibleWindow.theme;
    return settings.theme;
}

inline PersonalizationSettings ResolveStatusBarAppearance(const StatusBarSettings& settings,
    const PersonalizationSettings& global, const StatusBarSceneState& state)
{
    return ResolveStatusBarAppearance(ResolveStatusBarSceneTheme(settings, state), global);
}
}
