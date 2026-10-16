#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace snowdesktop::layout_spacing_rules
{

inline constexpr float kMinimumScale = 0.10f;
inline constexpr float kMaximumScale = 2.00f;
inline constexpr float kComponentGapBaseline = 12.0f;

// Independent content axes; scalar callers retain the uniform fixed-grid rule.
struct ContentSpacing
{
    float horizontal;
    float vertical;
    constexpr ContentSpacing(float uniform = 1.0f)
        : horizontal(uniform), vertical(uniform) {}
    constexpr ContentSpacing(float x, float y)
        : horizontal(x), vertical(y) {}
};

enum class DeferredChangeAction
{
    None,
    Preview,
    Commit,
};

inline DeferredChangeAction ResolveDeferredChangeAction(
    bool previewPending, bool commitPending)
{
    if (commitPending)
        return DeferredChangeAction::Commit;
    if (previewPending)
        return DeferredChangeAction::Preview;
    return DeferredChangeAction::None;
}

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

// Legacy scrolling rows clamped their gap factor to 50%. Preserve that
// appearance when the new field is absent; explicit new values support 10%.
inline float ResolveStoredRowSpacing(
    const std::optional<float>& value, float legacyScale)
{
    return ClampScale(value.value_or(std::max(0.5f, legacyScale)));
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
        kComponentGapBaseline * std::max(0.1f, pageVisualScale) *
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
    const int available = std::max(0, pageGap);
    const int baseMargin = std::max(
        0, pageMargin - available / 2);
    return baseMargin + ComponentVisualGap(
        available, pageVisualScale, layoutSpacingScale) / 2;
}

} // namespace snowdesktop::layout_spacing_rules
