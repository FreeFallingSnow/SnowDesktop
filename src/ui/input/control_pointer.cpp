#include "control_pointer.h"
#include <algorithm>
#include <cmath>

namespace snowdesktop::controlled_pointer
{
POINT Interpolate(POINT from, POINT to, double progress, double curvature)
{
    const double t = std::clamp(progress, 0.0, 1.0);
    const double eased = t * t * (3.0 - 2.0 * t);
    const double dx = static_cast<double>(to.x) - from.x, dy = static_cast<double>(to.y) - from.y;
    const double arc = 4.0 * eased * (1.0 - eased) * std::clamp(curvature, -0.4, 0.4);
    return {static_cast<LONG>(std::lround(from.x + dx * eased - dy * arc)),
        static_cast<LONG>(std::lround(from.y + dy * eased + dx * arc))};
}
bool AbsolutePoint(POINT point, RECT screen, LONG& x, LONG& y)
{
    const auto width = screen.right - screen.left, height = screen.bottom - screen.top;
    if (width < 2 || height < 2 || !PtInRect(&screen, point)) return false;
    x = static_cast<LONG>(std::lround((static_cast<double>(point.x) - screen.left) * 65535.0 / (width - 1)));
    y = static_cast<LONG>(std::lround((static_cast<double>(point.y) - screen.top) * 65535.0 / (height - 1)));
    return true;
}

}
