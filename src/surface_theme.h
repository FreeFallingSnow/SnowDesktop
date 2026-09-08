#pragma once
#include "personalization.h"

namespace snowdesktop
{
struct SurfaceTheme
{
    // -2 preserves the old conditional override; -1 follows the global preset;
    // 0..3 are the existing four presets; 4 selects the independent appearance.
    int mode = -1;
    bool customized = false;
    PersonalizationSettings appearance;
    friend bool operator==(const SurfaceTheme&, const SurfaceTheme&) = default;
};

inline int GlobalSurfaceThemeSelection(const PersonalizationSettings& global)
{
    if (global.backgroundPreset != kAppearancePresetCustom)
        return FourThemeSelectionFromAppearancePreset(global.backgroundPreset);
    return (global.contentTheme == 1 ? 1 : 0) + (global.glassEnabled ? 2 : 0);
}

inline PersonalizationSettings ResolveSurfaceTheme(const SurfaceTheme& theme,
    const PersonalizationSettings& global, int legacySelection, bool quickNavigation)
{
    if (theme.mode == 4) return theme.appearance;
    const int selection = theme.mode == -2
        ? (global.backgroundPreset == kAppearancePresetCustom ? NormalizeFourThemeSelection(legacySelection) : GlobalSurfaceThemeSelection(global))
        : theme.mode == -1 ? GlobalSurfaceThemeSelection(global) : NormalizeFourThemeSelection(theme.mode);
    const int preset = AppearancePresetFromFourThemeSelection(selection);
    return quickNavigation ? MakeQuickNavigationAppearancePreset(preset) : MakeCollectionPopupAppearancePreset(preset);
}

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

inline bool DecodePanelAppearance(const JsonValue& input, PersonalizationSettings& output)
{
    if (!input.IsObject()) return false;
    PersonalizationSettings value;
    value.backgroundPreset = kAppearancePresetCustom;
    bool valid = true;
    VisitPanelAppearanceFields([&](auto key, auto field, double minimum, double maximum) {
        if (const auto* json = input.Find(key))
        {
            if (!json->IsNumber() || !std::isfinite(json->number) || json->number < minimum || json->number > maximum) valid = false;
            else value.*field = static_cast<float>(json->number);
        }
    });
    VisitPanelAppearanceFlags([&](auto key, auto field) {
        if (const auto* json = input.Find(key))
        {
            if (!json->IsBoolean()) valid = false;
            else value.*field = json->boolean;
        }
    });
    if (const auto* theme = input.Find("contentTheme"))
    {
        if (!theme->IsNumber() || (theme->number != 0 && theme->number != 1)) valid = false;
        else value.contentTheme = static_cast<int>(theme->number);
    }
    if (const auto* gradient = input.Find("gradient"))
        valid = DecodePanelGradient(*gradient, value.panelGradient) && valid;
    if (valid) output = value;
    return valid;
}

inline std::string EncodePanelAppearance(const PersonalizationSettings& value)
{
    std::ostringstream output; output.imbue(std::locale::classic()); output.precision(9);
    output << '{';
    bool valid = value.contentTheme == 0 || value.contentTheme == 1;
    VisitPanelAppearanceFields([&](auto key, auto field, double minimum, double maximum) {
        const double number = value.*field;
        if (!std::isfinite(number) || number < minimum || number > maximum) valid = false;
        output << '"' << key << "\":" << number << ',';
    });
    VisitPanelAppearanceFlags([&](auto key, auto field) { output << '"' << key << "\":" << (value.*field ? "true" : "false") << ','; });
    const auto gradient = EncodePanelGradient(value.panelGradient);
    if (!valid || gradient.empty()) return {};
    output << "\"contentTheme\":" << value.contentTheme << ",\"gradient\":" << gradient << '}';
    return output.str();
}

inline bool DecodeSurfaceTheme(const JsonValue& input, SurfaceTheme& output)
{
    if (!input.IsObject()) return false;
    SurfaceTheme value;
    if (const auto* mode = input.Find("mode"))
    {
        if (!mode->IsNumber() || mode->number < -2 || mode->number > 4 || std::floor(mode->number) != mode->number) return false;
        value.mode = static_cast<int>(mode->number);
    }
    if (const auto* customized = input.Find("customized"))
    {
        if (!customized->IsBoolean()) return false;
        value.customized = customized->boolean;
    }
    if (const auto* appearance = input.Find("appearance"))
        if (!DecodePanelAppearance(*appearance, value.appearance)) return false;
    output = value;
    return true;
}

inline std::string EncodeSurfaceTheme(const SurfaceTheme& value)
{
    const auto appearance = EncodePanelAppearance(value.appearance);
    if (value.mode < -2 || value.mode > 4 || appearance.empty()) return {};
    return "{\"mode\":" + std::to_string(value.mode) + ",\"customized\":" + (value.customized ? "true" : "false") + ",\"appearance\":" + appearance + '}';
}
}
