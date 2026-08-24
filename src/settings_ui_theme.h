#pragma once

#include "general_settings.h"

#include <cstdint>

namespace snowdesktop::settings_ui
{

enum class EffectiveTheme
{
    Light,
    Dark,
    HighContrast,
};

enum class NavigationMode
{
    Expanded,
    Compact,
};

struct AccentPalette
{
    std::uint32_t base = 0;
    std::uint32_t hover = 0;
    std::uint32_t pressed = 0;
    std::uint32_t foreground = 0;
};

EffectiveTheme ResolveTheme(
    SettingsWindowTheme preference,
    bool systemUsesLightTheme,
    bool highContrast) noexcept;

bool ShouldUseMica(
    bool systemSupportsMica,
    bool transparencyEnabled,
    bool highContrast) noexcept;

bool ShouldAnimate(
    bool systemAnimationsEnabled,
    bool highContrast) noexcept;

NavigationMode ResolveNavigationMode(float clientWidthDip) noexcept;

std::uint32_t BlendRgb(
    std::uint32_t first,
    std::uint32_t second,
    float secondAmount) noexcept;

double ContrastRatio(
    std::uint32_t first,
    std::uint32_t second) noexcept;

AccentPalette MakeAccentPalette(
    std::uint32_t accent,
    bool lightTheme) noexcept;

} // namespace snowdesktop::settings_ui
