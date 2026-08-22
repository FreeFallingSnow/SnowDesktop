#pragma once

namespace snowdesktop::drag_hint_rules
{
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

constexpr bool ShouldReuseRaster(
    bool rasterValid,
    bool sameText,
    unsigned cachedDpi,
    unsigned currentDpi)
{
    return rasterValid && sameText &&
        cachedDpi != 0 && cachedDpi == currentDpi;
}

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
