#pragma once

#include "ui/menu/modern_menu.h"

#include <algorithm>

namespace snowdesktop::tray_context_menu
{

enum class TrayEdge
{
    Bottom,
    Top,
    Left,
    Right,
};

inline TrayEdge ResolveTrayEdge(
    const RECT& surface, const MONITORINFO& monitorInfo)
{
    const int surfaceWidth = surface.right - surface.left;
    const int surfaceHeight = surface.bottom - surface.top;
    const int monitorWidth =
        monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    const int monitorHeight =
        monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
    if (surfaceWidth >= monitorWidth / 2 &&
        surfaceWidth >= surfaceHeight)
    {
        return surface.top + surface.bottom >=
                monitorInfo.rcMonitor.top + monitorInfo.rcMonitor.bottom
            ? TrayEdge::Bottom : TrayEdge::Top;
    }
    if (surfaceHeight >= monitorHeight / 2 &&
        surfaceHeight > surfaceWidth)
    {
        return surface.left + surface.right >=
                monitorInfo.rcMonitor.left + monitorInfo.rcMonitor.right
            ? TrayEdge::Right : TrayEdge::Left;
    }

    // The hidden-icons flyout need not follow the reserved taskbar edge
    // (for example with a relocated or auto-hidden taskbar). Its actual
    // bounds decide which side has room, otherwise an above-only viewport
    // can collapse to a single row beside a flyout at the top of the screen.
    const RECT& work = monitorInfo.rcWork;
    if (work.top == monitorInfo.rcMonitor.top &&
        work.bottom == monitorInfo.rcMonitor.bottom &&
        (work.left > monitorInfo.rcMonitor.left ||
            work.right < monitorInfo.rcMonitor.right))
    {
        const LONG leftSpace = std::max(0L,
            std::min(surface.left, work.right) - work.left);
        const LONG rightSpace = std::max(0L,
            work.right - std::max(surface.right, work.left));
        return leftSpace >= rightSpace ? TrayEdge::Right : TrayEdge::Left;
    }
    const LONG aboveSpace = std::max(0L,
        std::min(surface.top, work.bottom) - work.top);
    const LONG belowSpace = std::max(0L,
        work.bottom - std::max(surface.bottom, work.top));
    return aboveSpace >= belowSpace ? TrayEdge::Bottom : TrayEdge::Top;
}

inline void PlaceMenuAwayFromTraySurface(
    snowdesktop::modern_menu::Options& options,
    POINT screenPoint,
    const RECT& surface,
    const MONITORINFO& monitorInfo,
    int gap)
{
    switch (ResolveTrayEdge(surface, monitorInfo))
    {
    case TrayEdge::Bottom:
    {
        const int edge = std::min(
            surface.top, monitorInfo.rcWork.bottom) - gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::AboveAnchorRect;
        options.anchorRect = {
            screenPoint.x, edge, screenPoint.x + 1, edge };
        break;
    }
    case TrayEdge::Top:
    {
        const int edge = std::max(
            surface.bottom, monitorInfo.rcWork.top) + gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::BelowAnchorRect;
        options.anchorRect = {
            screenPoint.x, edge, screenPoint.x + 1, edge };
        break;
    }
    case TrayEdge::Right:
    {
        const int edge = std::min(
            surface.left, monitorInfo.rcWork.right) - gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::LeftOfAnchorRect;
        options.anchorRect = {
            edge, screenPoint.y, edge, screenPoint.y + 1 };
        break;
    }
    case TrayEdge::Left:
    {
        const int edge = std::max(
            surface.right, monitorInfo.rcWork.left) + gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::RightOfAnchorRect;
        options.anchorRect = {
            edge, screenPoint.y, edge, screenPoint.y + 1 };
        break;
    }
    }
}

} // namespace snowdesktop::tray_context_menu
