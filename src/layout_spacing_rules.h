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

inline float ComponentGapResponseScale(float layoutSpacingScale)
{
    const float spacing = ClampScale(layoutSpacingScale);
    if (spacing <= 1.0f) return spacing;

    // Preserve the established compact 100% baseline, but give the upper
    // half of the shared slider enough range for widget frames to visibly
    // separate before the page gap becomes the limiting constraint.
    return 1.0f + (spacing - 1.0f) * 2.0f;
}

inline int ComponentVisualGap(
    int pageGap, float pageVisualScale, float layoutSpacingScale)
{
    const int available = std::max(0, pageGap);
    const int preferred = std::max(0, static_cast<int>(std::round(
        8.0f * std::max(0.1f, pageVisualScale) *
        ComponentGapResponseScale(layoutSpacingScale))));
    return std::min(available, preferred);
}

inline int ComponentFrameOutset(
    int pageGap, float pageVisualScale, float layoutSpacingScale)
{
    const int available = std::max(0, pageGap);
    return std::max(0, (available - ComponentVisualGap(
        available, pageVisualScale, layoutSpacingScale)) / 2);
}

inline int ComponentEdgeMargin(
    int pageMargin, int pageGap, float pageVisualScale,
    float layoutSpacingScale)
{
    return std::max(0, pageMargin - ComponentFrameOutset(
        pageGap, pageVisualScale, layoutSpacingScale));
}

} // namespace snowdesktop::layout_spacing_rules
