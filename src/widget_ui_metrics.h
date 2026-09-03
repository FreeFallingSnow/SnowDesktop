#pragma once

#include <algorithm>

namespace snowdesktop::widget_runtime
{

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
    float dpiScale, float textScale) noexcept
{
    const float geometryScale = std::clamp(dpiScale, 0.5f, 8.0f);
    const float typographyScale = geometryScale *
        std::clamp(textScale, 0.5f, 5.0f);
    SemanticUiMetrics result;
    result.spacingXs = 4.0f * geometryScale;
    result.spacingSm = 8.0f * geometryScale;
    result.spacingMd = 12.0f * geometryScale;
    result.spacingLg = 16.0f * geometryScale;
    result.captionFontSize = 10.0f * typographyScale;
    result.bodyFontSize = 12.0f * typographyScale;
    result.titleFontSize = 14.0f * typographyScale;
    result.controlFontSize = 12.0f * typographyScale;
    result.compactControlHeight = std::max(
        28.0f * geometryScale,
        result.controlFontSize + 12.0f * geometryScale);
    result.controlHeight = std::max(
        32.0f * geometryScale,
        result.controlFontSize + 16.0f * geometryScale);
    result.compactRowHeight = std::max(
        32.0f * geometryScale,
        result.bodyFontSize + 16.0f * geometryScale);
    result.rowHeight = std::max(
        40.0f * geometryScale,
        result.bodyFontSize + 20.0f * geometryScale);
    result.smallIconSize = 12.0f * geometryScale;
    result.iconSize = 16.0f * geometryScale;
    result.largeIconSize = 20.0f * geometryScale;
    result.controlRadius = 8.0f * geometryScale;
    result.strokeWidth = 1.0f * geometryScale;
    return result;
}

} // namespace snowdesktop::widget_runtime
