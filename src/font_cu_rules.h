#pragma once

#include <algorithm>

namespace snowdesktop::font_cu_rules
{

inline constexpr float Scale(float valueCu, float cellScale)
{
    return std::max(9.0f,
        valueCu * std::max(0.1f, cellScale));
}

} // namespace snowdesktop::font_cu_rules
