#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace snowdesktop::blur_sampling
{
inline constexpr int kMaximumBackdropDeviation = 48;
inline constexpr LONG kMaximumBackdropMargin = 3 * kMaximumBackdropDeviation;

inline LONG KernelMargin(float standardDeviation) noexcept
{
    const float radius = std::isfinite(standardDeviation)
        ? std::clamp(standardDeviation, 0.0f, static_cast<float>(kMaximumBackdropDeviation)) : 0.0f;
    return static_cast<LONG>(std::ceil(3.0f * radius));
}

inline RECT SampleBounds(const RECT& display, const RECT& animationEnvelope, LONG margin) noexcept
{
    RECT bounds{};
    if (UnionRect(&bounds, &display, &animationEnvelope))
        InflateRect(&bounds, margin, margin);
    return bounds;
}
}
