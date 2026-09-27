#pragma once
#include <windows.h>
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace snowdesktop::popup_round_geometry
{
// Physical-pixel geometry is shared by the content and backdrop layers. Keep
// fractional radii after DPI conversion; only the HWND visibility fence is
// integral. Its conservative outline must contain the antialiased edge.
inline D2D1_ROUNDED_RECT Resolve(const RECT& frame, float radius, float offsetY = 0)
{
    if (!std::isfinite(offsetY)) offsetY = 0;
    D2D1_RECT_F bounds{static_cast<float>(frame.left), static_cast<float>(frame.top) + offsetY,
        static_cast<float>(frame.right), static_cast<float>(frame.bottom) + offsetY};
    const float limit = (std::max)(0.f, (std::min)(bounds.right - bounds.left, bounds.bottom - bounds.top) / 2);
    radius = std::isfinite(radius) ? std::clamp(radius, 0.f, limit) : 0;
    return {bounds, radius, radius};
}

inline RECT WindowFence(const RECT& frame, float offsetY = 0)
{
    if (!std::isfinite(offsetY)) offsetY = 0;
    const auto coordinate = [](double value) {
        return static_cast<LONG>(std::clamp(value,
            static_cast<double>((std::numeric_limits<LONG>::min)()),
            static_cast<double>((std::numeric_limits<LONG>::max)())));
    };
    // One physical pixel retains AA coverage and the centered panel outline.
    // The actual rounded shape belongs to the D2D/Composition alpha clip.
    return {coordinate(static_cast<double>(frame.left) - 1),
        coordinate(std::floor(static_cast<double>(frame.top) + offsetY) - 1),
        coordinate(static_cast<double>(frame.right) + 1),
        coordinate(std::ceil(static_cast<double>(frame.bottom) + offsetY) + 1)};
}

// Caller owns the returned region. Retain a physical pixel around the alpha
// contour, while removing the transparent corner from the HWND itself (an
// HTTRANSPARENT reply alone does not forward input across UI threads).
inline HRGN CreateWindowFence(const RECT& frame, float radius, float offsetY = 0)
{
    const auto fence = WindowFence(frame, offsetY);
    const auto shape = Resolve(frame, radius, offsetY);
    const int diameter = static_cast<int>(std::floor((std::max)(0.f, shape.radiusX - 1.f) * 2));
    return diameter > 0 ? CreateRoundRectRgn(fence.left, fence.top, fence.right, fence.bottom, diameter, diameter)
        : CreateRectRgnIndirect(&fence);
}

inline bool Contains(const D2D1_ROUNDED_RECT& rounded, D2D1_POINT_2F point)
{
    const auto& frame = rounded.rect;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < frame.left ||
        point.y < frame.top || point.x >= frame.right || point.y >= frame.bottom) return false;
    const float radius = rounded.radiusX;
    if (radius <= 0) return true;
    const float x = point.x - std::clamp(point.x, frame.left + radius, frame.right - radius);
    const float y = point.y - std::clamp(point.y, frame.top + radius, frame.bottom - radius);
    return x * x + y * y <= radius * radius;
}
}
