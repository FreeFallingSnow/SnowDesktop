#pragma once
#include "surface_theme.h"

namespace snowdesktop
{
// A bar follows the actual desktop appearance. Popup presets deliberately use
// different opacity/material defaults and must not be substituted here.
inline PersonalizationSettings ResolveStatusBarAppearance(const SurfaceTheme& theme,
    const PersonalizationSettings& global)
{
    if (theme.mode < 0) return global;
    if (theme.mode == 4) return theme.appearance;
    return MakeAppearancePreset(AppearancePresetFromFourThemeSelection(theme.mode));
}
}
