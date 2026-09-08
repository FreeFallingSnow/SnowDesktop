#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::widget_runtime
{

// Host-level structure expressed in page CU. Detailed component content uses
// multiples of the resolved row unit instead of becoming a collection of
// global knobs.
struct SemanticUiMetricTokens
{
    float rowHeight = 28.0f;

    bool operator==(const SemanticUiMetricTokens&) const = default;
};

inline SemanticUiMetricTokens NormalizeSemanticUiMetricTokens(
    SemanticUiMetricTokens value) noexcept
{
    if (!std::isfinite(value.rowHeight))
        value.rowHeight = SemanticUiMetricTokens{}.rowHeight;
    value.rowHeight = std::clamp(value.rowHeight, 18.0f, 48.0f);
    return value;
}

struct SemanticUiMetrics
{
    float layoutRowHeight = 28.0f;
};

inline float ResolveSemanticRowScale(int gridRows) noexcept
{
    // Two rows and below use the page baseline. Taller widgets grow slowly.
    // Width never participates in vertical row or control sizing.
    return std::sqrt(std::max(1.0f,
        static_cast<float>(std::max(1, gridRows)) / 2.0f));
}

inline SemanticUiMetrics ResolveSemanticUiMetrics(
    const SemanticUiMetricTokens& rawTokens,
    float pageCuScale, float textScale,
    float rowScale = 1.0f) noexcept
{
    const SemanticUiMetricTokens tokens =
        NormalizeSemanticUiMetricTokens(rawTokens);
    const float geometryScale = std::clamp(pageCuScale, 0.1f, 8.0f);
    const float accessibilityScale = std::clamp(textScale, 0.5f, 5.0f);
    const float normalizedRowScale = std::isfinite(rowScale)
        ? std::max(1.0f, rowScale) : 1.0f;

    SemanticUiMetrics result;
    result.layoutRowHeight = tokens.rowHeight * geometryScale *
        normalizedRowScale * accessibilityScale;
    return result;
}

inline SemanticUiMetrics ResolveSemanticUiMetrics(
    float pageCuScale, float textScale,
    float rowScale = 1.0f) noexcept
{
    return ResolveSemanticUiMetrics(
        SemanticUiMetricTokens{}, pageCuScale, textScale, rowScale);
}

} // namespace snowdesktop::widget_runtime
