#pragma once

#include <windows.h>
#include <algorithm>
#include <optional>

namespace snowdesktop::start_menu
{
// Screen pixels throughout. These values deliberately match DockPosition,
// but the Shell bridge has no dependency on the desktop model or its settings.
enum class Edge : int { Bottom, Top, Left, Right };
struct Placement
{
    RECT screen{}, work{}, avoid{};
    Edge edge = Edge::Bottom;
    int gap = 8;
};

inline bool ValidRect(const RECT& r) noexcept
{
    return r.right > r.left && r.bottom > r.top;
}

inline std::optional<RECT> Position(const Placement& p, SIZE menu) noexcept
{
    if (!ValidRect(p.screen) || !ValidRect(p.work) || !ValidRect(p.avoid) ||
        menu.cx <= 0 || menu.cy <= 0 || p.gap < 0 || p.gap > 256 ||
        p.edge < Edge::Bottom || p.edge > Edge::Right) return {};
    RECT area{};
    if (!IntersectRect(&area, &p.screen, &p.work)) return {};
    RECT intersection{};
    if (!IntersectRect(&intersection, &p.screen, &p.avoid)) return {};
    switch (p.edge)
    {
    case Edge::Bottom: area.bottom = std::min(area.bottom, p.avoid.top - p.gap); break;
    case Edge::Top: area.top = std::max(area.top, p.avoid.bottom + p.gap); break;
    case Edge::Left: area.left = std::max(area.left, p.avoid.right + p.gap); break;
    case Edge::Right: area.right = std::min(area.right, p.avoid.left - p.gap); break;
    }
    if (!ValidRect(area) || menu.cx > area.right - area.left || menu.cy > area.bottom - area.top)
        return {}; // Never shrink/crop a system menu to manufacture a fit.
    LONG x = std::clamp(p.screen.left + (p.screen.right - p.screen.left - menu.cx) / 2,
        area.left, area.right - menu.cx);
    LONG y = std::clamp(p.screen.top + (p.screen.bottom - p.screen.top - menu.cy) / 2,
        area.top, area.bottom - menu.cy);
    switch (p.edge)
    {
    case Edge::Bottom: y = area.bottom - menu.cy; break;
    case Edge::Top: y = area.top; break;
    case Edge::Left: x = area.left; break;
    case Edge::Right: x = area.right - menu.cx; break;
    }
    return RECT{x, y, x + menu.cx, y + menu.cy};
}
}
