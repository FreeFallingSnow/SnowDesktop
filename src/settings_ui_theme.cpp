#include "settings_ui_theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace snowdesktop::settings_ui
{
namespace
{
double LinearChannel(unsigned value) noexcept
{
    const double normalized = static_cast<double>(value) / 255.0;
    return normalized <= 0.04045
        ? normalized / 12.92
        : std::pow((normalized + 0.055) / 1.055, 2.4);
}

double RelativeLuminance(std::uint32_t color) noexcept
{
    const unsigned red = (color >> 16) & 0xffu;
    const unsigned green = (color >> 8) & 0xffu;
    const unsigned blue = color & 0xffu;
    return 0.2126 * LinearChannel(red) +
        0.7152 * LinearChannel(green) +
        0.0722 * LinearChannel(blue);
}
}

EffectiveTheme ResolveTheme(
    SettingsWindowTheme preference,
    bool systemUsesLightTheme,
    bool highContrast) noexcept
{
    if (highContrast)
        return EffectiveTheme::HighContrast;
    if (preference == SettingsWindowTheme::Light)
        return EffectiveTheme::Light;
    if (preference == SettingsWindowTheme::Dark)
        return EffectiveTheme::Dark;
    return systemUsesLightTheme
        ? EffectiveTheme::Light
        : EffectiveTheme::Dark;
}

bool ShouldUseMica(
    bool systemSupportsMica,
    bool transparencyEnabled,
    bool highContrast) noexcept
{
    return systemSupportsMica && transparencyEnabled && !highContrast;
}

bool ShouldAnimate(
    bool systemAnimationsEnabled,
    bool highContrast) noexcept
{
    return systemAnimationsEnabled && !highContrast;
}

NavigationMode ResolveNavigationMode(float clientWidthDip) noexcept
{
    return clientWidthDip < 720.0f
        ? NavigationMode::Compact
        : NavigationMode::Expanded;
}

CjkFontFamily ResolvePreferredCjkFont(
    std::string_view effectiveLanguage) noexcept
{
    std::string language(effectiveLanguage);
    std::transform(language.begin(), language.end(), language.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });

    if (language == "ja" || language.starts_with("ja-"))
        return CjkFontFamily::YuGothic;
    if (language == "ko" || language.starts_with("ko-"))
        return CjkFontFamily::MalgunGothic;
    if (language == "zh" || language.starts_with("zh-"))
    {
        const bool traditional =
            language.find("hant") != std::string::npos ||
            language.find("-tw") != std::string::npos ||
            language.find("-hk") != std::string::npos ||
            language.find("-mo") != std::string::npos;
        return traditional
            ? CjkFontFamily::MicrosoftJhengHei
            : CjkFontFamily::MicrosoftYaHei;
    }
    return CjkFontFamily::MicrosoftYaHei;
}

std::uint32_t BlendRgb(
    std::uint32_t first,
    std::uint32_t second,
    float secondAmount) noexcept
{
    const float amount = std::clamp(secondAmount, 0.0f, 1.0f);
    const auto blendChannel = [amount](unsigned a, unsigned b) {
        return static_cast<unsigned>(std::clamp(
            std::lround(static_cast<double>(a) * (1.0 - amount) +
                static_cast<double>(b) * amount), 0l, 255l));
    };
    const unsigned red = blendChannel(
        (first >> 16) & 0xffu, (second >> 16) & 0xffu);
    const unsigned green = blendChannel(
        (first >> 8) & 0xffu, (second >> 8) & 0xffu);
    const unsigned blue = blendChannel(
        first & 0xffu, second & 0xffu);
    return (red << 16) | (green << 8) | blue;
}

double ContrastRatio(
    std::uint32_t first,
    std::uint32_t second) noexcept
{
    const double firstLuminance = RelativeLuminance(first);
    const double secondLuminance = RelativeLuminance(second);
    const double light = std::max(firstLuminance, secondLuminance);
    const double dark = std::min(firstLuminance, secondLuminance);
    return (light + 0.05) / (dark + 0.05);
}

AccentPalette MakeAccentPalette(
    std::uint32_t accent,
    bool lightTheme) noexcept
{
    accent &= 0x00ffffffu;
    const std::uint32_t white = 0x00ffffffu;
    const std::uint32_t black = 0x00000000u;
    AccentPalette result;
    result.base = accent;
    result.hover = BlendRgb(accent,
        lightTheme ? black : white, lightTheme ? 0.10f : 0.12f);
    result.pressed = BlendRgb(accent,
        lightTheme ? black : white, lightTheme ? 0.20f : 0.22f);
    result.foreground = ContrastRatio(accent, white) >= 4.5
        ? white : black;
    return result;
}

} // namespace snowdesktop::settings_ui
