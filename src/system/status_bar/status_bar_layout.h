#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop
{
inline bool StatusBarUsesTwoLineClock(bool merged, float height, float scale)
{
    return merged && height >= 40.f * scale;
}
inline std::wstring StatusBarClockDisplay(std::wstring_view value, bool twoLineClock)
{
    std::wstring result(value);
    if (twoLineClock)
        if (const auto separator = result.find(L"   "); separator != std::wstring::npos)
            result.replace(separator, 3, L"\n");
    return result;
}
inline RECT StatusBarCompactTarget(RECT bounds, float scale, bool twoLineClock)
{
    if (IsRectEmpty(&bounds)) return bounds;
    const LONG height = (std::min)(bounds.bottom - bounds.top,
        static_cast<LONG>(std::ceil((twoLineClock ? 40.f : 32.f) * scale)));
    bounds.top += (bounds.bottom - bounds.top - height) / 2;
    bounds.bottom = bounds.top + height;
    return bounds;
}
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
struct MergedStatusBarLayout
{
    RECT center{}, clock{}, notifications{};
    std::vector<RECT> right;
};
// The Dock keeps its symmetric reservation. The clock and notification target
// form the last group on the complete bar; tray items never follow them.
inline MergedStatusBarLayout LayoutMergedStatusBar(LONG width, LONG height, float scale,
    LONG padding, LONG clockWidth, LONG notificationWidth, std::span<const LONG> rightWidths,
    std::optional<std::size_t> controlIndex)
{
    MergedStatusBarLayout result;
    result.center = MergedStatusBarCenter(width, height, scale);
    const LONG centerWidth = result.center.right - result.center.left;
    clockWidth = std::max(0L, clockWidth);
    notificationWidth = std::max(0L, notificationWidth);
    if (controlIndex && *controlIndex >= rightWidths.size()) controlIndex.reset();
    const LONG controlWidth = controlIndex ? std::max(0L, rightWidths[*controlIndex]) : 0;
    std::vector<LONG> widths(rightWidths.begin(), rightWidths.end());
    widths.push_back(clockWidth + notificationWidth);
    LONG edge = std::max(0L, padding);
    const auto arrange = [&] { return StatusBarHorizontalLayout(width, height, edge,
        std::span<const LONG>{}, centerWidth, widths); };
    auto bounds = arrange();
    const auto missingControl = [&] { return controlWidth && IsRectEmpty(&bounds[*controlIndex + 1]); };
    const auto missingEssential = [&] {
        return (notificationWidth && IsRectEmpty(&bounds.back())) || missingControl();
    };
    if (missingEssential())
    {
        // Retain the existing narrow-layout rule: first remove the date as a
        // whole, then optional tray items, without clipping any hit target.
        clockWidth = 0; widths.back() = notificationWidth; bounds = arrange();
        for (std::size_t i = 0; missingEssential() && i < rightWidths.size(); ++i)
            if (!controlIndex || i != *controlIndex) { widths[i] = 0; bounds = arrange(); }
        if (missingEssential())
        {
            const LONG available = width - result.center.right;
            edge = std::min(edge, std::max(0L, (available - notificationWidth - controlWidth) / 2));
            bounds = arrange();
        }
        // At an extreme width where only the system controls fit, keep their
        // existing priority. A visible notification still always ends the bar.
        if (missingControl()) { widths.back() = 0; bounds = arrange(); }
    }
    result.center = bounds.front();
    result.right.assign(bounds.begin() + 1, bounds.end() - 1);
    const auto group = bounds.back();
    if (!IsRectEmpty(&group))
    {
        if (clockWidth) result.clock = {group.left, 0, group.left + clockWidth, height};
        if (notificationWidth) result.notifications = {group.left + clockWidth, 0, group.right, height};
    }
    return result;
}
}
