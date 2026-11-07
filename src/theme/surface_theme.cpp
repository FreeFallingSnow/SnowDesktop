#include "surface_theme.h"
#include "personalization.h"
#include "edge_light_codec.h"
#include "common/json_value.h"
#include <cmath>
#include <locale>
#include <sstream>

namespace snowdesktop
{
void SelectSurfaceThemeMode(SurfaceTheme& theme, int mode,
    const PersonalizationSettings& effectiveAppearance)
{
    if (mode == 4 && theme.mode != 4)
    {
        theme.appearance = effectiveAppearance;
        theme.appearance.backgroundPreset = kAppearancePresetCustom;
        theme.customized = true;
    }
    theme.mode = mode;
}

int GlobalSurfaceThemeSelection(const PersonalizationSettings& global)
{
    if (global.backgroundPreset != kAppearancePresetCustom)
        return FourThemeSelectionFromAppearancePreset(global.backgroundPreset);
    return (global.contentTheme == 1 ? 1 : 0) + (global.glassEnabled ? 2 : 0);
}

bool IsCustomSurfaceTheme(const SurfaceTheme& theme, const PersonalizationSettings& global)
{
    return theme.mode == 4 || (theme.mode == -1 && global.backgroundPreset == kAppearancePresetCustom);
}

PersonalizationSettings ResolveSurfaceTheme(const SurfaceTheme& theme,
    const PersonalizationSettings& global, int legacySelection, bool quickNavigation,
    const SurfaceTheme* globalBinding)
{
    if (theme.mode == -1 && globalBinding && globalBinding->customized)
        return globalBinding->appearance;
    if (theme.mode == 4) return theme.appearance;
    if (IsCustomSurfaceTheme(theme, global))
        return global;
    const int selection = theme.mode == -2
        ? (global.backgroundPreset == kAppearancePresetCustom ? NormalizeFourThemeSelection(legacySelection) : GlobalSurfaceThemeSelection(global))
        : theme.mode == -1 ? GlobalSurfaceThemeSelection(global) : NormalizeFourThemeSelection(theme.mode);
    const int preset = AppearancePresetFromFourThemeSelection(selection);
    return quickNavigation ? MakeQuickNavigationAppearancePreset(preset) : MakeCollectionPopupAppearancePreset(preset);
}

bool DecodePanelAppearance(const JsonValue& input, PersonalizationSettings& output,
    bool includeBottomBarOpacity)
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
    if (const auto* opacity = includeBottomBarOpacity ? input.Find("gradientEndOpacity") : nullptr)
    {
        if (!opacity->IsNumber() || !std::isfinite(opacity->number) || opacity->number < 0 || opacity->number > 1) valid = false;
        else value.gradientEndA = static_cast<float>(opacity->number);
    }
    if (const auto* gradient = input.Find("gradient"))
        valid = DecodePanelGradient(*gradient, value.panelGradient) && valid;
    if (const auto* light = input.Find("edgeLight")) valid = DecodeEdgeLight(*light, value.edgeLight) && valid;
    if (valid) output = value;
    return valid;
}

std::string EncodePanelAppearance(const PersonalizationSettings& value,
    bool includeBottomBarOpacity)
{
    std::ostringstream output; output.imbue(std::locale::classic()); output.precision(9);
    output << '{';
    bool valid = (value.contentTheme == 0 || value.contentTheme == 1) &&
        (!includeBottomBarOpacity || (std::isfinite(value.gradientEndA) && value.gradientEndA >= 0 && value.gradientEndA <= 1));
    VisitPanelAppearanceFields([&](auto key, auto field, double minimum, double maximum) {
        const double number = value.*field;
        if (!std::isfinite(number) || number < minimum || number > maximum) valid = false;
        output << '"' << key << "\":" << number << ',';
    });
    VisitPanelAppearanceFlags([&](auto key, auto field) { output << '"' << key << "\":" << (value.*field ? "true" : "false") << ','; });
    const auto gradient = EncodePanelGradient(value.panelGradient);
    const auto light = EncodeEdgeLight(value.edgeLight);
    if (!valid || gradient.empty() || light.empty()) return {};
    if (includeBottomBarOpacity) output << "\"gradientEndOpacity\":" << value.gradientEndA << ',';
    output << "\"edgeLight\":" << light << ',';
    output << "\"contentTheme\":" << value.contentTheme << ",\"gradient\":" << gradient << '}';
    return output.str();
}

bool DecodeSurfaceTheme(const JsonValue& input, SurfaceTheme& output, bool includeStatusBarPresets)
{
    if (!input.IsObject()) return false;
    SurfaceTheme value;
    if (const auto* mode = input.Find("mode"))
    {
        if (!mode->IsNumber() || mode->number < -2 || mode->number > (includeStatusBarPresets ? 8 : 4) || std::floor(mode->number) != mode->number) return false;
        value.mode = static_cast<int>(mode->number);
    }
    if (const auto* customized = input.Find("customized"))
    {
        if (!customized->IsBoolean()) return false;
        value.customized = customized->boolean;
    }
    if (const auto* appearance = input.Find("appearance"))
        if (!DecodePanelAppearance(*appearance, value.appearance, true)) return false;
    output = value;
    return true;
}

std::string EncodeSurfaceTheme(const SurfaceTheme& value, bool includeStatusBarPresets)
{
    const auto appearance = EncodePanelAppearance(value.appearance, true);
    if (value.mode < -2 || value.mode > (includeStatusBarPresets ? 8 : 4) || appearance.empty()) return {};
    return "{\"mode\":" + std::to_string(value.mode) + ",\"customized\":" + (value.customized ? "true" : "false") + ",\"appearance\":" + appearance + '}';
}
}
