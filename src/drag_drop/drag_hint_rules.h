#pragma once

namespace snowdesktop::drag_hint_rules
{
constexpr const char* DockInsertionHintKey(bool sameDock, bool external,
    bool componentPayload, bool control)
{
    if (sameDock) return "core.drag.release_adjust_order";
    if (componentPayload) return "core.drag.move_widget_dock";
    return external || control ? "core.dock.release_dock_map_full"
        : "core.dock.release_move_dock_ctrl";
}

struct Point
{
    long x = 0;
    long y = 0;
};

struct Size
{
    long width = 0;
    long height = 0;
};

struct Rect
{
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
};

constexpr long ClampAxis(
    long desired,
    long size,
    long workStart,
    long workEnd,
    long margin)
{
    if (workEnd <= workStart || size <= 0)
        return desired;

    const long low = workStart + margin;
    const long high = workEnd - size - margin;
    if (high < low)
    {
        // The work area is smaller than the hint plus its margins. Centering
        // avoids invalid clamp bounds and distributes overflow on both sides.
        return workStart + ((workEnd - workStart) - size) / 2;
    }
    if (desired < low)
        return low;
    if (desired > high)
        return high;
    return desired;
}

constexpr Point ResolveWindowPosition(
    Point anchor,
    Size size,
    Rect workArea,
    long offsetX,
    long offsetY,
    long margin)
{
    return {
        ClampAxis(anchor.x + offsetX, size.width,
            workArea.left, workArea.right, margin),
        ClampAxis(anchor.y + offsetY, size.height,
            workArea.top, workArea.bottom, margin),
    };
}
}
