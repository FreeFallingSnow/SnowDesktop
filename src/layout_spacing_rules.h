#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace snowdesktop::layout_spacing_rules
{

inline constexpr float kMinimumScale = 0.50f;
inline constexpr float kMaximumScale = 2.00f;

inline float ClampScale(float value)
{
    if (!std::isfinite(value)) return 1.0f;
    return std::clamp(value, kMinimumScale, kMaximumScale);
}

inline float ResolveStoredScale(
    const std::optional<float>& iconSpacing,
    const std::optional<float>& legacyComponentSpacing,
    float fallback = 1.0f)
{
    // iconSpacing is canonical whenever it exists. componentSpacing is kept
    // only as a read-only migration source for older layout files.
    if (iconSpacing) return ClampScale(*iconSpacing);
    if (legacyComponentSpacing)
        return ClampScale(*legacyComponentSpacing);
    return ClampScale(fallback);
}

inline int ComponentVisualGap(
    int pageGap, float cellScale, float layoutSpacingScale)
{
    const int available = std::max(0, pageGap);
    const int preferred = std::max(0, static_cast<int>(std::round(
        8.0f * std::max(0.1f, cellScale) *
        ClampScale(layoutSpacingScale))));
    return std::min(available, preferred);
}

inline int ComponentFrameOutset(
    int pageGap, float cellScale, float layoutSpacingScale)
{
    const int available = std::max(0, pageGap);
    return std::max(0, (available - ComponentVisualGap(
        available, cellScale, layoutSpacingScale)) / 2);
}

inline int ComponentEdgeMargin(
    int pageMargin, int pageGap, float cellScale,
    float layoutSpacingScale)
{
    return std::max(0, pageMargin - ComponentFrameOutset(
        pageGap, cellScale, layoutSpacingScale));
}

} // namespace snowdesktop::layout_spacing_rules
