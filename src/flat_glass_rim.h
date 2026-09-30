#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::flat_glass_rim
{
// Icons and panels share a thin sheet profile. Size changes its contour, while
// the reflected light remains confined to the rim instead of a broad bevel.
inline float Intensity(float distance, float depth, float alignment)
{
    if (depth <= 0.0f) return 0.0f;
    const auto smooth = [](float lower, float upper, float value) {
        const float t = std::clamp(
            (value - lower) / (upper - lower), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float crest = 1.0f - smooth(0.02f, 1.0f, distance / depth);
    const float shoulder = 1.0f - smooth(0.05f, 1.0f,
        distance / (depth * 1.25f));
    return std::pow(std::max(alignment, 0.0f), 0.65f) *
            (0.96f * crest + 0.04f * shoulder) +
        0.40f * std::pow(std::max(-alignment, 0.0f), 0.80f) *
            (0.22f * crest);
}
}
