#pragma once

#include "constants.h"

#include <algorithm>

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

} // namespace snowdesktop::font_cu_rules
