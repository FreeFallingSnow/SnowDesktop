#pragma once
#include "surface_theme_types.h"
#include <string>

struct JsonValue;

namespace snowdesktop
{
void SelectSurfaceThemeMode(SurfaceTheme& theme, int mode,
    const PersonalizationSettings& effectiveAppearance);

int GlobalSurfaceThemeSelection(const PersonalizationSettings& global);

bool IsCustomSurfaceTheme(const SurfaceTheme& theme, const PersonalizationSettings& global);

PersonalizationSettings ResolveSurfaceTheme(const SurfaceTheme& theme,
    const PersonalizationSettings& global, int legacySelection, bool quickNavigation,
    const SurfaceTheme* globalBinding = nullptr);

template<class Visit> void VisitPanelAppearanceFields(Visit visit)
{
    visit("backgroundR", &PersonalizationSettings::widgetBgR, 0., 1.);
    visit("backgroundG", &PersonalizationSettings::widgetBgG, 0., 1.);
    visit("backgroundB", &PersonalizationSettings::widgetBgB, 0., 1.);
    visit("opacity", &PersonalizationSettings::widgetAlpha, 0., 1.);
    visit("borderR", &PersonalizationSettings::widgetBorderR, 0., 1.);
    visit("borderG", &PersonalizationSettings::widgetBorderG, 0., 1.);
    visit("borderB", &PersonalizationSettings::widgetBorderB, 0., 1.);
    visit("borderOpacity", &PersonalizationSettings::widgetBorderAlpha, 0., 1.);
    visit("borderWidth", &PersonalizationSettings::widgetBorderWidth, .5, 4.);
    visit("highlightWidth", &PersonalizationSettings::widgetEdgeHighlightWidth, .5, 4.);
    visit("highlightStrength", &PersonalizationSettings::widgetEdgeHighlightStrength, 0., 1.);
    visit("blurRadius", &PersonalizationSettings::glassBlurRadius, 4., 48.);
    visit("cornerRadius", &PersonalizationSettings::cornerRadius, 0., 100.);
}
template<class Visit> void VisitPanelAppearanceFlags(Visit visit)
{
    visit("glass", &PersonalizationSettings::glassEnabled);
    visit("acrylic", &PersonalizationSettings::acrylicEnabled);
    visit("highlight", &PersonalizationSettings::widgetEdgeHighlightEnabled);
}

// Taskbar material storage deliberately excludes the legacy widget bottom-bar
// opacity. Complete theme and surface snapshots explicitly opt into that field.
bool DecodePanelAppearance(const JsonValue& input, PersonalizationSettings& output,
    bool includeBottomBarOpacity = false);

std::string EncodePanelAppearance(const PersonalizationSettings& value,
    bool includeBottomBarOpacity = false);

bool DecodeSurfaceTheme(const JsonValue& input, SurfaceTheme& output, bool includeStatusBarPresets = false);

std::string EncodeSurfaceTheme(const SurfaceTheme& value, bool includeStatusBarPresets = false);
}
