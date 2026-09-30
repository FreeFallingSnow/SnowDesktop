#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::flat_glass_rim
{
inline constexpr int kPanelOverdraw = 3;

// A clear sheet has a visible reflected rim, a soft halo and an adjacent dark
// seam. Keeping the seam distinct avoids shading the entire edge like a lens.
inline float Intensity(float distance, float depth, float alignment)
{
    if (depth <= 0.0f) return 0.0f;
    const auto smooth = [](float lower, float upper, float value) {
        const float t = std::clamp(
            (value - lower) / (upper - lower), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float rimPosition = (distance - depth * 0.35f) / (depth * 0.72f);
    const float haloPosition = distance / (depth * 1.65f);
    const float rim = std::exp(-0.5f * rimPosition * rimPosition);
    const float halo = std::exp(-0.5f * haloPosition * haloPosition);
    const float outsideFade = 1.0f - smooth(
        1.5f, static_cast<float>(kPanelOverdraw), std::max(-distance, 0.0f));
    const float lighting = 0.70f +
        0.25f * std::pow(std::max(alignment, 0.0f), 0.65f) +
        0.05f * std::pow(std::max(-alignment, 0.0f), 0.80f);
    return std::clamp((0.78f * rim + 0.34f * halo) * lighting *
        outsideFade, 0.0f, 1.0f);
}

inline float Occlusion(float distance, float depth)
{
    if (depth <= 0.0f || distance <= 0.0f) return 0.0f;
    const float seam = (distance - (depth * 1.65f + 0.5f)) /
        std::max(0.40f, depth * 0.28f);
    return std::exp(-0.5f * seam * seam);
}
}
