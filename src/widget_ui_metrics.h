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
    float rowHeight = 40.0f;

    bool operator==(const SemanticUiMetricTokens&) const = default;
};

inline SemanticUiMetricTokens NormalizeSemanticUiMetricTokens(
    SemanticUiMetricTokens value) noexcept
{
    if (!std::isfinite(value.rowHeight))
        value.rowHeight = SemanticUiMetricTokens{}.rowHeight;
    value.rowHeight = std::clamp(value.rowHeight, 24.0f, 64.0f);
    return value;
}

struct SemanticUiMetrics
{
    float layoutRowHeight = 40.0f;
    // Compatibility alias for packages authored before row-unit semantics.
    float titleAreaHeight = 40.0f;
    float spacingXs = 4.0f;
    float spacingSm = 8.0f;
    float spacingMd = 12.0f;
    float spacingLg = 16.0f;
    float captionFontSize = 10.0f;
    float bodyFontSize = 12.0f;
    float titleFontSize = 14.0f;
    float controlFontSize = 12.0f;
    float compactControlHeight = 28.0f;
    float controlHeight = 32.0f;
    float compactRowHeight = 32.0f;
    float rowHeight = 40.0f;
    float smallIconSize = 12.0f;
    float iconSize = 16.0f;
    float largeIconSize = 20.0f;
    float controlRadius = 8.0f;
    float strokeWidth = 1.0f;
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
    const float resolvedRowHeight = tokens.rowHeight * normalizedRowScale;
    const float metricScale = resolvedRowHeight / 40.0f;
    const float semanticGeometryScale = geometryScale * metricScale;
    const float typographyScale = semanticGeometryScale * accessibilityScale;

    SemanticUiMetrics result;
    result.layoutRowHeight = resolvedRowHeight * geometryScale;
    result.titleAreaHeight = result.layoutRowHeight;
    result.spacingXs = 4.0f * semanticGeometryScale;
    result.spacingSm = 8.0f * semanticGeometryScale;
    result.spacingMd = 12.0f * semanticGeometryScale;
    result.spacingLg = 16.0f * semanticGeometryScale;
    result.captionFontSize = 10.0f * typographyScale;
    result.bodyFontSize = 12.0f * typographyScale;
    result.titleFontSize = 14.0f * typographyScale;
    result.controlFontSize = 12.0f * typographyScale;
    result.compactControlHeight = std::max(
        28.0f * semanticGeometryScale,
        result.controlFontSize + 12.0f * semanticGeometryScale);
    result.controlHeight = std::max(
        32.0f * semanticGeometryScale,
        result.controlFontSize + 16.0f * semanticGeometryScale);
    // Retained for source compatibility. Content rows should normally derive
    // their intrinsic size from text and their component-specific layout.
    result.compactRowHeight = std::max(
        32.0f * semanticGeometryScale,
        result.bodyFontSize + 16.0f * semanticGeometryScale);
    result.rowHeight = std::max(
        40.0f * semanticGeometryScale,
        result.bodyFontSize + 20.0f * semanticGeometryScale);
    result.smallIconSize = 12.0f * semanticGeometryScale;
    result.iconSize = 16.0f * semanticGeometryScale;
    result.largeIconSize = 20.0f * semanticGeometryScale;
    result.controlRadius = 8.0f * semanticGeometryScale;
    result.strokeWidth = 1.0f * semanticGeometryScale;
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
