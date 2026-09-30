#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::flat_glass_rim
{
inline constexpr int kPanelOverdraw = 3;

// A rotated angular distribution places the weak sectors on the upper-right
// and lower-left edges. Normalized coordinates keep those sectors at the same
// relative contour positions on wide Docks and square icon plates.
inline float Lighting(float x, float y)
{
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    constexpr float kReflectionRotation = 3.14159265358979323846f / 3.0f;
    const float angle = std::atan2(y - 0.5f, x - 0.5f);
    const float alignment = std::cos(angle - kReflectionRotation);
    return std::clamp(0.12f +
        0.86f * std::pow(std::max(alignment, 0.0f), 0.85f) +
        0.66f * std::pow(std::max(-alignment, 0.0f), 0.90f), 0.0f, 1.0f);
}

inline float WidthScale(float lighting)
{
    return 0.30f + 0.85f * lighting;
}

// Bright reflection segments are wider, with a compact soft falloff. The flat
// body is not shaded by a broad shoulder or surrounded by a uniform glow.
inline float Intensity(float distance, float depth, float lighting)
{
    if (depth <= 0.0f) return 0.0f;
    const auto smooth = [](float lower, float upper, float value) {
        const float t = std::clamp(
            (value - lower) / (upper - lower), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float localDepth = depth * WidthScale(lighting);
    const float rimPosition = (distance - localDepth * 0.25f) / (localDepth * 0.40f);
    const float haloPosition = distance / (localDepth * 0.85f);
    const float rim = std::exp(-0.5f * rimPosition * rimPosition);
    const float halo = std::exp(-0.5f * haloPosition * haloPosition);
    const float outsideFade = 1.0f - smooth(
        1.5f, static_cast<float>(kPanelOverdraw), std::max(-distance, 0.0f));
    return std::clamp((0.86f * rim + 0.22f * halo) * lighting *
        outsideFade, 0.0f, 1.0f);
}

inline float Occlusion(float distance, float depth, float lighting)
{
    if (depth <= 0.0f || distance <= 0.0f) return 0.0f;
    const float localDepth = depth * WidthScale(lighting);
    const float seam = (distance - (localDepth * 0.95f + 0.30f)) /
        std::max(0.25f, localDepth * 0.22f);
    return lighting * std::exp(-0.5f * seam * seam);
}
}
