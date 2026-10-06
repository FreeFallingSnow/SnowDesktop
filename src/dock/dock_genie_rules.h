#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace snowdesktop::dock_genie
{
// One uploaded snapshot is shared by all strips. No per-frame capture/upload.
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
inline int EffectiveEffect(int requested, bool snapshotAvailable) noexcept
{
    return requested == 3 && !snapshotAvailable ? 1 : requested;
}

// Collapsed=0 is exactly the window; collapsed=1 is exactly the Dock target.
// The near edge reaches the mouth first. Cross-sections progressively narrow
// toward it, then the far edge is pulled in. This is a spatial deformation,
// not an alternative easing curve on a uniformly scaled rectangle.
inline Matrix StripMatrix(const Rect& window, const Rect& dock,
    Edge edge, double collapsed, double sourceWidth, double sourceHeight,
    double begin, double end, double hostLeft, double hostTop) noexcept
{
    const bool vertical = Vertical(edge);
    const double pull = Smooth(collapsed / 0.55);
    const double travel = Smooth((collapsed - 0.20) / 0.80);
    const double sourceAxis = std::max(1.0, vertical ? sourceHeight : sourceWidth);
    const double sourceCross = std::max(1.0, vertical ? sourceWidth : sourceHeight);
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
    const double centerBegin = Mix(windowCenter, dockCenter, blendAt(begin));
    const double centerEnd = Mix(windowCenter, dockCenter, blendAt(end));
    const double cross = std::max(1.0,
        Mix(windowCross, dockCross, blendAt((begin + end) * 0.5)));
    const double axisScale = std::max(1.0, high - low) / sourceAxis;
    const double crossScale = cross / sourceCross;
    const double shear = (centerEnd - centerBegin) /
        std::max(0.0001, (end - begin) * sourceAxis);
    const double crossOffset = centerBegin - cross * 0.5 - shear * begin * sourceAxis;
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
}
