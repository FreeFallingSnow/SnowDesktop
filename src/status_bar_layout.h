#pragma once
#include <windows.h>
#include <algorithm>
#include <span>
#include <vector>

namespace snowdesktop
{
// Center the clock on the monitor. Preserve rightmost controls at narrow
// widths; omit whole overflow items instead of partial, ambiguous targets.
inline std::vector<RECT> StatusBarHorizontalLayout(LONG width, LONG height,
    LONG padding, LONG leftWidth, LONG clockWidth, std::span<const LONG> rightWidths)
{
    const LONG end = std::max(0L, width - padding);
    clockWidth = std::clamp(clockWidth, 0L, std::max(0L, width - 2 * padding));
    const LONG center = (width - clockWidth) / 2;
    std::vector<RECT> result(2 + rightWidths.size());
    if (clockWidth) result[1] = {center, 0, center + clockWidth, height};
    const LONG leftEnd = std::min(padding + leftWidth, clockWidth ? center - padding : end);
    if (leftEnd > padding) result[0] = {padding, 0, leftEnd, height};
    const LONG minimum = clockWidth ? center + clockWidth + padding : leftEnd + padding;
    LONG cursor = end;
    for (std::size_t i = rightWidths.size(); i > 0; --i)
    {
        const LONG extent = std::max(0L, rightWidths[i - 1]);
        if (extent > 0 && cursor - extent >= minimum)
        {
            result[i + 1] = {cursor - extent, 0, cursor, height};
            cursor -= extent;
        }
    }
    return result;
}
}
