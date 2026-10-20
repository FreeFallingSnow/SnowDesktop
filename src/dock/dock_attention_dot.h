#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::dock_attention_dot
{
inline constexpr float kDefaultSizeScale = 1.0f;
inline constexpr float kMinimumSizeScale = 0.5f;
inline constexpr float kMaximumSizeScale = 2.0f;
inline constexpr float kDefaultOffset = 0.0f;
inline constexpr float kMinimumOffset = -0.25f;
inline constexpr float kMaximumOffset = 0.25f;

inline float NormalizeSizeScale(float value) noexcept
{
    return std::isfinite(value)
        ? std::clamp(value, kMinimumSizeScale, kMaximumSizeScale) : kDefaultSizeScale;
}

inline float NormalizeOffset(float value) noexcept
{
    return std::isfinite(value)
        ? std::clamp(value, kMinimumOffset, kMaximumOffset) : kDefaultOffset;
}

struct Geometry
{
    float radius = 0.0f;
    float cornerInset = 0.0f;
};

inline Geometry Resolve(float iconSize, float sizeScale, float offset) noexcept
{
    if (!std::isfinite(iconSize) || iconSize <= 0.0f) return {};
    const float baseRadius = std::min(iconSize * 0.25f,
        std::clamp(iconSize * 0.09f, 3.0f, 6.0f));
    // Size and position stay independent. Positive offsets move diagonally
    // outward from the top-right corner; both follow the rendered icon size.
    return {baseRadius * NormalizeSizeScale(sizeScale),
        baseRadius * 0.675f + 0.5f - iconSize * NormalizeOffset(offset)};
}
} // namespace snowdesktop::dock_attention_dot
