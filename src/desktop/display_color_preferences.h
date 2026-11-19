#pragma once

#include <cstdint>

namespace snowdesktop::display_color
{
enum class Compensation : std::uint8_t
{
    Off,
    Highlights,
    HighlightsAndShadows,
};

inline constexpr Compensation CompensationFromInt(
    int value, Compensation fallback = Compensation::Highlights) noexcept
{
    switch (value)
    {
    case 0: return Compensation::Off;
    case 1: return Compensation::Highlights;
    case 2: return Compensation::HighlightsAndShadows;
    default: return fallback;
    }
}
} // namespace snowdesktop::display_color
