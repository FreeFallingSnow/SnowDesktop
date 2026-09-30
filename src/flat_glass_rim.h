#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::flat_glass_rim
{
inline constexpr int kPanelOverdraw = 3;

// Two broad incident reflections taper along the contour as well as around
// its corners. A constant normal alone gives every straight edge one width.
inline float Lighting(float normalX, float normalY, float x, float y)
{
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    const float alignment = -(normalX + normalY) * 0.70710678f;
    const float primaryPosition = 1.0f - (x + y) * 0.5f;
    const float oppositePosition = (x + y) * 0.5f;
    return std::clamp(0.06f +
        0.84f * std::pow(std::max(alignment, 0.0f), 0.65f) *
            std::pow(primaryPosition, 0.75f) +
        0.90f * std::pow(std::max(-alignment, 0.0f), 0.80f) *
            std::pow(oppositePosition, 1.10f), 0.0f, 1.0f);
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
