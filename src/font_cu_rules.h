#pragma once

#include "constants.h"

#include <algorithm>
#include <optional>

namespace snowdesktop::font_cu_rules
{

inline constexpr float CellScale(int cellWidth, int cellHeight)
{
    return std::max(0.1f, std::min(
        static_cast<float>(std::max(1, cellWidth)) /
            static_cast<float>(kCellWidth),
        static_cast<float>(std::max(1, cellHeight)) /
            static_cast<float>(kMinCellHeight)));
}

inline constexpr float Scale(float valueCu, float cellScale)
{
    return std::max(9.0f,
        valueCu * std::max(0.1f, cellScale));
}

inline constexpr bool IsConfigSize(float valueCu)
{
    return valueCu >= kMinimumItemFontSizeCu &&
        valueCu <= kMaximumItemFontSizeCu;
}

inline constexpr float LegacyPointsToCu(float valuePoints)
{
    return std::clamp(
        valuePoints * 96.0f / 72.0f,
        kMinimumItemFontSizeCu,
        kMaximumItemFontSizeCu);
}

inline std::optional<float> ResolveStoredSize(
    const std::optional<float>& valueCu,
    const std::optional<float>& legacyValuePoints)
{
    if (valueCu && IsConfigSize(*valueCu))
        return valueCu;
    if (legacyValuePoints &&
        *legacyValuePoints >= 10.0f &&
        *legacyValuePoints <= 24.0f)
    {
        return LegacyPointsToCu(*legacyValuePoints);
    }
    return std::nullopt;
}

} // namespace snowdesktop::font_cu_rules
