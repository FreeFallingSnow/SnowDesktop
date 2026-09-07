#pragma once

#include "dock_genie_rules.h"

namespace snowdesktop::quick_navigation_animation_rules
{
struct GeniePoint
{
    double x = 0.0, y = 0.0;
};

struct GenieStripProjection
{
    // Work in strip-local coordinates. Keeping the perspective denominator
    // local avoids cancellation at large screen coordinates and high DPI.
    float sourceX = 0.0f, sourceY = 0.0f;
    float width = 0.0f, height = 0.0f;
    float m11 = 1.0f, m12 = 0.0f, m14 = 0.0f;
    float m21 = 0.0f, m22 = 1.0f, m24 = 0.0f;
    float m41 = 0.0f, m42 = 0.0f, m44 = 1.0f;

    GeniePoint Map(double sourcePointX, double sourcePointY) const noexcept
    {
        const double x = sourcePointX - sourceX;
        const double y = sourcePointY - sourceY;
        const double w = x * m14 + y * m24 + m44;
        return {(x * m11 + y * m21 + m41) / w,
            (x * m12 + y * m22 + m42) / w};
    }
};

inline dock_genie::Rect GenieContentClip(std::size_t index,
    double width, double height, dock_genie::Edge edge)
{
    if (index >= dock_genie::StripCount)
        return {};
    const bool vertical = dock_genie::Vertical(edge);
    const double axis = vertical ? height : width;
    const double begin = axis * static_cast<double>(index) / dock_genie::StripCount;
    const double end = axis * static_cast<double>(index + 1) / dock_genie::StripCount;
    // Alpha content must cover each source pixel once; overlapping adjacent
    // clips composites its translucent background twice and creates dark seams.
    return vertical ? dock_genie::Rect{0.0, begin, width, end}
        : dock_genie::Rect{begin, 0.0, end, height};
}

inline GenieStripProjection GenieProjection(const dock_genie::Rect& window,
    const dock_genie::Rect& dock, dock_genie::Edge edge, double collapsed,
    double width, double height, std::size_t index,
    double hostLeft = 0.0, double hostTop = 0.0) noexcept
{
    namespace genie = dock_genie;
    GenieStripProjection result;
    if (index >= genie::StripCount || width <= 0.0 || height <= 0.0)
        return result;
    const bool vertical = genie::Vertical(edge);
    const auto clip = GenieContentClip(index, width, height, edge);
    result.sourceX = static_cast<float>(clip.left);
    result.sourceY = static_cast<float>(clip.top);
    result.width = static_cast<float>(clip.right) - result.sourceX;
    result.height = static_cast<float>(clip.bottom) - result.sourceY;
    const double begin = static_cast<double>(index) / genie::StripCount;
    const double end = static_cast<double>(index + 1) / genie::StripCount;
    // A zero-length sample yields the exact width/center at this cross-section.
    // Both neighbors use the same sample, instead of unrelated midpoint widths.
    const auto first = genie::StripMatrix(window, dock, edge, collapsed,
        width, height, begin, begin, hostLeft, hostTop);
    const auto last = genie::StripMatrix(window, dock, edge, collapsed,
        width, height, end, end, hostLeft, hostTop);
    const double crossLength = vertical ? width : height;
    const double axisLength = vertical ? result.height : result.width;
    if (axisLength <= 0.0)
        return {};
    const double firstWidth = crossLength * (vertical ? first.m11 : first.m22);
    const double lastWidth = crossLength * (vertical ? last.m11 : last.m22);
    const double firstLeft = vertical ? first.dx : first.dy;
    const double lastLeft = vertical ? last.dx : last.dy;
    const double sourceBegin = vertical ? result.sourceY : result.sourceX;
    const double sourceEnd = sourceBegin + axisLength;
    const double firstAxis = vertical ? first.dy + sourceBegin * first.m22
                                      : first.dx + sourceBegin * first.m11;
    const double lastAxis = vertical ? last.dy + sourceEnd * last.m22
                                     : last.dx + sourceEnd * last.m11;
    const double ratio = firstWidth / lastWidth;
    const double perspective = (ratio - 1.0) / axisLength;
    const double crossShear = (lastLeft * ratio - firstLeft) / axisLength;
    const double axisScale = (lastAxis * ratio - firstAxis) / axisLength;
    if (vertical)
    {
        result.m11 = static_cast<float>(firstWidth / crossLength);
        result.m21 = static_cast<float>(crossShear);
        result.m22 = static_cast<float>(axisScale);
        result.m24 = static_cast<float>(perspective);
        result.m41 = static_cast<float>(firstLeft);
        result.m42 = static_cast<float>(firstAxis);
    }
    else
    {
        result.m11 = static_cast<float>(axisScale);
        result.m12 = static_cast<float>(crossShear);
        result.m22 = static_cast<float>(firstWidth / crossLength);
        result.m14 = static_cast<float>(perspective);
        result.m41 = static_cast<float>(firstAxis);
        result.m42 = static_cast<float>(firstLeft);
    }
    return result;
}

inline dock_genie::Rect GenieBandClip(const GenieStripProjection& projection,
    std::size_t index, dock_genie::Edge edge) noexcept
{
    // Clip only the internal joins. The child bitmap owns antialiasing of the
    // outside silhouette, so leave room around its perimeter for edge coverage.
    const bool vertical = dock_genie::Vertical(edge);
    const double crossPadding = vertical ? projection.width : projection.height;
    const double capPadding = 2.0;
    const double begin = index == 0 ? -capPadding : 0.0;
    const double end = (vertical ? projection.height : projection.width) +
        (index + 1 == dock_genie::StripCount ? capPadding : 0.0);
    return vertical
        ? dock_genie::Rect{-crossPadding, begin, projection.width + crossPadding, end}
        : dock_genie::Rect{begin, -crossPadding, end, projection.height + crossPadding};
}
}
