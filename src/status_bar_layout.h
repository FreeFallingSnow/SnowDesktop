#pragma once
#include <windows.h>
#include <algorithm>
#include <span>
#include <vector>

namespace snowdesktop
{
// Keep the center symmetric. Overflowing side items are omitted whole by the
// normal layout, while the Dock uses its existing horizontal scrolling.
inline RECT MergedStatusBarCenter(LONG width, LONG height, float scale)
{
    const auto side = static_cast<LONG>(std::clamp(480.f * scale, width * .22f, width * .42f));
    return {side, 0, std::max(side, width - side), height};
}
// Center the clock on the monitor. Preserve rightmost controls at narrow
// widths; omit whole overflow items instead of partial, ambiguous targets.
inline std::vector<RECT> StatusBarHorizontalLayout(LONG width, LONG height,
    LONG padding, std::span<const LONG> leftWidths, LONG clockWidth, std::span<const LONG> rightWidths)
{
    const LONG end = std::max(0L, width - padding);
    clockWidth = std::clamp(clockWidth, 0L, std::max(0L, width - 2 * padding));
    const LONG center = (width - clockWidth) / 2;
    std::vector<RECT> result(leftWidths.size() + 1 + rightWidths.size());
    if (clockWidth) result[leftWidths.size()] = {center, 0, center + clockWidth, height};
    LONG leftEnd = padding;
    for (std::size_t i = 0; i < leftWidths.size(); ++i)
    {
        const LONG extent = std::max(0L, leftWidths[i]);
        if (extent > 0 && leftEnd + extent <= (clockWidth ? center - padding : end))
        {
            result[i] = {leftEnd, 0, leftEnd + extent, height};
            leftEnd += extent;
        }
    }
    const LONG minimum = clockWidth ? center + clockWidth + padding : leftEnd + padding;
    LONG cursor = end;
    for (std::size_t i = rightWidths.size(); i > 0; --i)
    {
        const LONG extent = std::max(0L, rightWidths[i - 1]);
        if (extent > 0 && cursor - extent >= minimum)
        {
            result[leftWidths.size() + i] = {cursor - extent, 0, cursor, height};
            cursor -= extent;
        }
    }
    return result;
}
inline std::vector<RECT> StatusBarHorizontalLayout(LONG width, LONG height,
    LONG padding, LONG leftWidth, LONG clockWidth, std::span<const LONG> rightWidths)
{
    return StatusBarHorizontalLayout(width, height, padding,
        std::span<const LONG>(&leftWidth, 1), clockWidth, rightWidths);
}
}
