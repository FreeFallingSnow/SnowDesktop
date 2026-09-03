#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::widget_runtime
{

// Host-wide semantic UI tokens expressed in page CU. Components receive the
// resolved pixel values through ui.metrics(); their individual span never
// participates in this calculation.
struct SemanticUiMetricTokens
{
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

    bool operator==(const SemanticUiMetricTokens&) const = default;
};

inline SemanticUiMetricTokens NormalizeSemanticUiMetricTokens(
    SemanticUiMetricTokens value) noexcept
{
    const auto clamp = [](float candidate, float fallback,
                           float minimum, float maximum) noexcept {
        return std::isfinite(candidate)
            ? std::clamp(candidate, minimum, maximum)
            : fallback;
    };
    const SemanticUiMetricTokens defaults;
    value.spacingXs = clamp(value.spacingXs, defaults.spacingXs, 0.0f, 24.0f);
    value.spacingSm = clamp(value.spacingSm, defaults.spacingSm, 0.0f, 32.0f);
    value.spacingMd = clamp(value.spacingMd, defaults.spacingMd, 0.0f, 40.0f);
    value.spacingLg = clamp(value.spacingLg, defaults.spacingLg, 0.0f, 48.0f);
    value.captionFontSize = clamp(value.captionFontSize,
        defaults.captionFontSize, 6.0f, 28.0f);
    value.bodyFontSize = clamp(value.bodyFontSize,
        defaults.bodyFontSize, 6.0f, 32.0f);
    value.titleFontSize = clamp(value.titleFontSize,
        defaults.titleFontSize, 6.0f, 40.0f);
    value.controlFontSize = clamp(value.controlFontSize,
        defaults.controlFontSize, 6.0f, 32.0f);
    value.compactControlHeight = clamp(value.compactControlHeight,
        defaults.compactControlHeight, 16.0f, 64.0f);
    value.controlHeight = clamp(value.controlHeight,
        defaults.controlHeight, 16.0f, 72.0f);
    value.compactRowHeight = clamp(value.compactRowHeight,
        defaults.compactRowHeight, 16.0f, 72.0f);
    value.rowHeight = clamp(value.rowHeight,
        defaults.rowHeight, 16.0f, 88.0f);
    value.smallIconSize = clamp(value.smallIconSize,
        defaults.smallIconSize, 6.0f, 40.0f);
    value.iconSize = clamp(value.iconSize, defaults.iconSize, 6.0f, 48.0f);
    value.largeIconSize = clamp(value.largeIconSize,
        defaults.largeIconSize, 6.0f, 64.0f);
    value.controlRadius = clamp(value.controlRadius,
        defaults.controlRadius, 0.0f, 28.0f);
    value.strokeWidth = clamp(value.strokeWidth,
        defaults.strokeWidth, 0.5f, 4.0f);
    return value;
}

struct SemanticUiMetrics
{
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
    const float typographyScale = geometryScale *
        std::clamp(textScale, 0.5f, 5.0f);
    SemanticUiMetrics result;
    result.spacingXs = tokens.spacingXs * geometryScale;
    result.spacingSm = tokens.spacingSm * geometryScale;
    result.spacingMd = tokens.spacingMd * geometryScale;
    result.spacingLg = tokens.spacingLg * geometryScale;
    result.captionFontSize = tokens.captionFontSize * typographyScale;
    result.bodyFontSize = tokens.bodyFontSize * typographyScale;
    result.titleFontSize = tokens.titleFontSize * typographyScale;
    result.controlFontSize = tokens.controlFontSize * typographyScale;
    result.compactControlHeight = std::max(
        tokens.compactControlHeight * geometryScale,
        result.controlFontSize + tokens.spacingMd * geometryScale);
    result.controlHeight = std::max(
        tokens.controlHeight * geometryScale,
        result.controlFontSize + tokens.spacingLg * geometryScale);
    result.compactRowHeight = std::max(
        tokens.compactRowHeight * geometryScale,
        result.bodyFontSize + tokens.spacingLg * geometryScale);
    result.rowHeight = std::max(
        tokens.rowHeight * geometryScale,
        result.bodyFontSize + (tokens.spacingMd + tokens.spacingLg) *
            geometryScale);
    result.smallIconSize = tokens.smallIconSize * geometryScale;
    result.iconSize = tokens.iconSize * geometryScale;
    result.largeIconSize = tokens.largeIconSize * geometryScale;
    result.controlRadius = tokens.controlRadius * geometryScale;
    result.strokeWidth = tokens.strokeWidth * geometryScale;
    return result;
}

inline SemanticUiMetrics ResolveSemanticUiMetrics(
    float pageCuScale, float textScale) noexcept
{
    return ResolveSemanticUiMetrics(
        SemanticUiMetricTokens{}, pageCuScale, textScale);
}

} // namespace snowdesktop::widget_runtime
