#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::widget_runtime
{

// Host-level structure expressed in page CU. Detailed component content uses
// its own available rectangle instead of becoming a collection of global knobs.
struct SemanticUiMetricTokens
{
    float titleAreaHeight = 40.0f;

    bool operator==(const SemanticUiMetricTokens&) const = default;
};

inline SemanticUiMetricTokens NormalizeSemanticUiMetricTokens(
    SemanticUiMetricTokens value) noexcept
{
    if (!std::isfinite(value.titleAreaHeight))
        value.titleAreaHeight = SemanticUiMetricTokens{}.titleAreaHeight;
    value.titleAreaHeight = std::clamp(
        value.titleAreaHeight, 24.0f, 64.0f);
    return value;
}

struct SemanticUiMetrics
{
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

inline SemanticUiMetrics ResolveSemanticUiMetrics(
    const SemanticUiMetricTokens& rawTokens,
    float pageCuScale, float textScale) noexcept
{
    const SemanticUiMetricTokens tokens =
        NormalizeSemanticUiMetricTokens(rawTokens);
    const float geometryScale = std::clamp(pageCuScale, 0.1f, 8.0f);
    const float accessibilityScale = std::clamp(textScale, 0.5f, 5.0f);
    const float headerScale = tokens.titleAreaHeight / 40.0f;
    const float typographyScale = geometryScale * accessibilityScale;
    const float headerTypographyScale = typographyScale * headerScale;

    SemanticUiMetrics result;
    result.titleAreaHeight = tokens.titleAreaHeight * geometryScale;
    result.spacingXs = 4.0f * geometryScale;
    result.spacingSm = 8.0f * geometryScale;
    result.spacingMd = 12.0f * geometryScale;
    result.spacingLg = 16.0f * geometryScale;
    result.captionFontSize = 10.0f * typographyScale;
    result.bodyFontSize = 12.0f * typographyScale;
    result.titleFontSize = 14.0f * headerTypographyScale;
    result.controlFontSize = 12.0f * headerTypographyScale;
    result.compactControlHeight = std::max(
        (tokens.titleAreaHeight - 12.0f) * geometryScale,
        result.controlFontSize + 12.0f * geometryScale);
    result.controlHeight = std::max(
        (tokens.titleAreaHeight - 8.0f) * geometryScale,
        result.controlFontSize + 16.0f * geometryScale);
    // Retained for source compatibility. Content rows should normally derive
    // their intrinsic size from text and their component-specific layout.
    result.compactRowHeight = std::max(
        32.0f * geometryScale,
        result.bodyFontSize + 16.0f * geometryScale);
    result.rowHeight = std::max(
        40.0f * geometryScale,
        result.bodyFontSize + 20.0f * geometryScale);
    result.smallIconSize = 12.0f * headerScale * geometryScale;
    result.iconSize = 16.0f * headerScale * geometryScale;
    result.largeIconSize = 20.0f * headerScale * geometryScale;
    result.controlRadius = 8.0f * geometryScale;
    result.strokeWidth = 1.0f * geometryScale;
    return result;
}

inline SemanticUiMetrics ResolveSemanticUiMetrics(
    float pageCuScale, float textScale) noexcept
{
    return ResolveSemanticUiMetrics(
        SemanticUiMetricTokens{}, pageCuScale, textScale);
}

} // namespace snowdesktop::widget_runtime
