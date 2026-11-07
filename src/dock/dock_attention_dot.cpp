#include "dock_attention_dot.h"

#include <algorithm>
#include <cmath>

namespace snowdesktop::dock_attention_dot
{
float NormalizeSizeScale(float value) noexcept
{
    return std::isfinite(value)
        ? std::clamp(value, kMinimumSizeScale, kMaximumSizeScale) : kDefaultSizeScale;
}

float NormalizeOffset(float value) noexcept
{
    return std::isfinite(value)
        ? std::clamp(value, kMinimumOffset, kMaximumOffset) : kDefaultOffset;
}

Geometry Resolve(float iconSize, float sizeScale, float offset) noexcept
{
    if (!std::isfinite(iconSize) || iconSize <= 0.0f) return {};
    const float baseRadius = std::min(iconSize * 0.25f,
        std::clamp(iconSize * 0.09f, 3.0f, 6.0f));
    // Size and position stay independent. Positive offsets move diagonally
    // outward from the top-right corner; both follow the rendered icon size.
    return {baseRadius * NormalizeSizeScale(sizeScale),
        baseRadius * 0.675f + 0.5f - iconSize * NormalizeOffset(offset)};
}
}
