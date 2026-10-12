#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace snowdesktop::dock_genie
{
// Strips share the window's DWM image or one uploaded fallback snapshot.
inline constexpr std::size_t StripCount = 128;
enum class Edge { Bottom = 0, Top = 1, Left = 2, Right = 3 };
struct Rect { double left, top, right, bottom; };
struct Matrix
{
    float m11 = 1, m12 = 0, m21 = 0, m22 = 1, dx = 0, dy = 0;
};
inline double Smooth(double value) noexcept
{
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}
inline double Mix(double from, double to, double amount) noexcept
{
    return from + (to - from) * amount;
}
inline bool Vertical(Edge edge) noexcept
{
    return edge == Edge::Bottom || edge == Edge::Top;
}
inline double Opacity(double collapsed) noexcept
{
    return 1.0 - Smooth((collapsed - 0.85) / 0.15);
}
// Collapsed=0 is exactly the window; collapsed=1 is exactly the Dock target.
// The near edge reaches the mouth first. Cross-sections progressively narrow
// toward it, then the far edge is pulled in. This is a spatial deformation,
// not an alternative easing curve on a uniformly scaled rectangle.
struct Section { double axis, center, width; };
inline Section SectionAt(const Rect& window, const Rect& dock, Edge edge,
    double collapsed, double sectionPosition) noexcept
{
    const bool vertical = Vertical(edge);
    const double pull = Smooth(collapsed / 0.55);
    const double travel = Smooth((collapsed - 0.20) / 0.80);
    const double windowLow = vertical ? window.top : window.left;
    const double windowHigh = vertical ? window.bottom : window.right;
    const double dockLow = vertical ? dock.top : dock.left;
    const double dockHigh = vertical ? dock.bottom : dock.right;
    // A Dock on another monitor can be on the opposite side of the source
    // window from its configured screen edge. Pull the physically nearer
    // endpoint first so the two axis endpoints never cross during transit.
    const double axisDirection = (dockLow + dockHigh) - (windowLow + windowHigh);
    const bool nearAtStart = axisDirection < 0.0 ||
        (axisDirection == 0.0 && (edge == Edge::Top || edge == Edge::Left));
    const double low = Mix(windowLow, dockLow, nearAtStart ? pull : travel);
    const double high = Mix(windowHigh, dockHigh, nearAtStart ? travel : pull);
    const double windowCenter = vertical ? (window.left + window.right) * 0.5
                                        : (window.top + window.bottom) * 0.5;
    const double dockCenter = vertical ? (dock.left + dock.right) * 0.5
                                      : (dock.top + dock.bottom) * 0.5;
    const double windowCross = vertical ? window.right - window.left
                                       : window.bottom - window.top;
    const double dockCross = vertical ? dock.right - dock.left : dock.bottom - dock.top;
    const auto blendAt = [&](double position) {
        const double proximity = nearAtStart ? 1.0 - position : position;
        return travel + (1.0 - travel) * pull * Smooth(proximity);
    };
    const double blend = blendAt(sectionPosition);
    return {Mix(low, high, sectionPosition), Mix(windowCenter, dockCenter, blend),
        std::max(1.0, Mix(windowCross, dockCross, blend))};
}

inline Matrix StripMatrix(const Rect& window, const Rect& dock,
    Edge edge, double collapsed, double sourceWidth, double sourceHeight,
    double begin, double end, double hostLeft, double hostTop) noexcept
{
    const bool vertical = Vertical(edge);
    const double sourceAxis = std::max(1.0, vertical ? sourceHeight : sourceWidth);
    const double sourceCross = std::max(1.0, vertical ? sourceWidth : sourceHeight);
    const auto first = SectionAt(window, dock, edge, collapsed, begin);
    const auto last = SectionAt(window, dock, edge, collapsed, end);
    const auto middle = SectionAt(window, dock, edge, collapsed, (begin + end) * 0.5);
    const double axisScale = std::max(1.0, SectionAt(window, dock, edge, collapsed, 1.0).axis -
        SectionAt(window, dock, edge, collapsed, 0.0).axis) / sourceAxis;
    const double crossScale = middle.width / sourceCross;
    const double shear = (last.center - first.center) /
        std::max(0.0001, (end - begin) * sourceAxis);
    const double crossOffset = first.center - middle.width * 0.5 - shear * begin * sourceAxis;
    const double low = SectionAt(window, dock, edge, collapsed, 0.0).axis;
    Matrix result;
    if (vertical)
    {
        result.m11 = static_cast<float>(crossScale);
        result.m21 = static_cast<float>(shear);
        result.m22 = static_cast<float>(axisScale);
        result.dx = static_cast<float>(crossOffset - hostLeft);
        result.dy = static_cast<float>(low - hostTop);
    }
    else
    {
        result.m11 = static_cast<float>(axisScale);
        result.m12 = static_cast<float>(shear);
        result.m22 = static_cast<float>(crossScale);
        result.dx = static_cast<float>(low - hostLeft);
        result.dy = static_cast<float>(crossOffset - hostTop);
    }
    return result;
}

struct ProjectiveMatrix
{
    double m11 = 1, m12 = 0, m14 = 0, m21 = 0, m22 = 1, m24 = 0;
    double dx = 0, dy = 0, w = 1;
};

// Map all four strip corners to a trapezoid. Sampling each boundary once by
// its normalized position makes neighbouring silhouettes meet exactly, unlike
// the constant-width affine approximation. The geometry is independent of the
// source image and works with live DWM visuals as well as fallback bitmaps.
inline ProjectiveMatrix StripProjectiveMatrix(const Rect& window, const Rect& dock,
    Edge edge, double collapsed, double sourceWidth, double sourceHeight,
    double begin, double end, double hostLeft, double hostTop) noexcept
{
    const bool vertical = Vertical(edge);
    const double sourceAxis = std::max(1.0, vertical ? sourceHeight : sourceWidth);
    const double sourceCross = std::max(1.0, vertical ? sourceWidth : sourceHeight);
    const auto first = SectionAt(window, dock, edge, collapsed, begin);
    const auto last = SectionAt(window, dock, edge, collapsed, end);
    const double crossHost = vertical ? hostLeft : hostTop;
    const double axisHost = vertical ? hostTop : hostLeft;
    const double left0 = first.center - first.width * 0.5 - crossHost;
    const double left1 = last.center - last.width * 0.5 - crossHost;
    const double axis0 = first.axis - axisHost;
    const double axis1 = last.axis - axisHost;
    const double span = std::max(0.0001, (end - begin) * sourceAxis);
    const double origin = begin * sourceAxis;
    const double perspective = first.width / last.width - 1.0;
    const double crossSlope = (left1 * (1.0 + perspective) - left0) / span;
    const double axisSlope = (axis1 * (1.0 + perspective) - axis0) / span;
    ProjectiveMatrix matrix;
    if (vertical)
    {
        matrix.m11 = first.width / sourceCross;
        matrix.m21 = crossSlope;
        matrix.m22 = axisSlope;
        matrix.m24 = perspective / span;
        matrix.dx = left0 - crossSlope * origin;
        matrix.dy = axis0 - axisSlope * origin;
        matrix.w = 1.0 - matrix.m24 * origin;
    }
    else
    {
        matrix.m11 = axisSlope;
        matrix.m12 = crossSlope;
        matrix.m14 = perspective / span;
        matrix.m22 = first.width / sourceCross;
        matrix.dx = axis0 - axisSlope * origin;
        matrix.dy = left0 - crossSlope * origin;
        matrix.w = 1.0 - matrix.m14 * origin;
    }
    return matrix;
}
}
