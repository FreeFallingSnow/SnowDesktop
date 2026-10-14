#pragma once
#include <windows.h>
#include "dock/dock_layout_settings.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop
{
// Invisible pointer target at the physical right edge of either bar layout.
inline RECT StatusBarShowDesktopTarget(LONG width, LONG height, float scale)
{
    if (width <= 0 || height <= 0 || !std::isfinite(scale) || scale <= 0) return {};
    const LONG extent = static_cast<LONG>(std::ceil(8.f * scale));
    return {std::max(0L, width - extent), 0, width, height};
}

// Per-monitor, transient state. Neither scrolling nor resizing changes the
// persisted pinned identities or their ordering.
struct StatusBarTrayScroll
{
    RECT viewport{};
    LONG contentWidth = 0, offset = 0;
    int wheelRemainder = 0;

    LONG Maximum() const { return std::max(0L, contentWidth - (viewport.right - viewport.left)); }
    void Arrange(RECT bounds, LONG content)
    {
        viewport = bounds; contentWidth = std::max(0L, content);
        offset = IsRectEmpty(&viewport) ? 0 : std::clamp(offset, 0L, Maximum());
        if (!Maximum()) wheelRemainder = 0;
    }
    bool Move(LONG distance)
    {
        const LONG next = std::clamp(offset + distance, 0L, Maximum());
        if (next == offset || IsRectEmpty(&viewport)) return false;
        offset = next; return true;
    }
    bool Wheel(int delta, LONG step)
    {
        wheelRemainder += delta;
        const int ticks = wheelRemainder / WHEEL_DELTA;
        wheelRemainder %= WHEEL_DELTA;
        return Move(-ticks * step);
    }
    int EdgeDirection(POINT point, LONG edge) const
    {
        if (!PtInRect(&viewport, point) || !Maximum()) return 0;
        edge = std::min(edge, (viewport.right - viewport.left) / 3);
        if (point.x < viewport.left + edge && offset > 0) return -1;
        if (point.x >= viewport.right - edge && offset < Maximum()) return 1;
        return 0;
    }
    bool Reveal(RECT contentBounds)
    {
        if (IsRectEmpty(&viewport) || IsRectEmpty(&contentBounds)) return false;
        if (contentBounds.left < viewport.left) return Move(contentBounds.left - viewport.left);
        if (contentBounds.right > viewport.right) return Move(contentBounds.right - viewport.right);
        return false;
    }
};

// Clip only the shared edge. Corners and panels separated by a gap keep it.
inline std::vector<RECT> StatusBarEdgeSegments(RECT frame, DockPosition position, RECT dock)
{
    const bool horizontal = position == DockPosition::Top || position == DockPosition::Bottom;
    const bool touching = !IsRectEmpty(&dock) &&
        (position == DockPosition::Top ? std::abs(frame.bottom - dock.top) <= 1 :
         position == DockPosition::Bottom ? std::abs(frame.top - dock.bottom) <= 1 :
         position == DockPosition::Left ? std::abs(frame.right - dock.left) <= 1 : std::abs(frame.left - dock.right) <= 1);
    const LONG begin = horizontal ? frame.left : frame.top;
    const LONG end = horizontal ? frame.right : frame.bottom;
    const LONG cutBegin = std::max(begin, horizontal ? dock.left : dock.top);
    const LONG cutEnd = std::min(end, horizontal ? dock.right : dock.bottom);
    if (!touching || cutBegin >= cutEnd) return {frame};
    std::vector<RECT> segments;
    if (cutBegin > begin)
    {
        auto first = frame;
        if (horizontal) first.right = cutBegin; else first.bottom = cutBegin;
        segments.push_back(first);
    }
    if (cutEnd < end)
    {
        auto last = frame;
        if (horizontal) last.left = cutEnd; else last.top = cutEnd;
        segments.push_back(last);
    }
    return segments;
}

inline bool StatusBarUsesTwoLineClock(bool merged)
{
    return merged;
}
inline std::wstring StatusBarClockDisplay(std::wstring_view value, bool twoLineClock)
{
    std::wstring result(value);
    if (twoLineClock)
        if (const auto separator = result.find(L"   "); separator != std::wstring::npos)
            result = result.substr(separator + 3) + L"\n" + result.substr(0, separator);
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
    LONG padding, std::span<const LONG> leftWidths, LONG clockWidth, std::span<const LONG> rightWidths,
    LONG trailingInset = 0)
{
    const LONG end = std::max(0L, width - std::max(padding, trailingInset));
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
// The clock and notification target form the last group on the complete bar.
// Merged bars also keep the Dock's symmetric central reservation.
inline MergedStatusBarLayout LayoutRightStatusBar(LONG width, LONG height, float scale,
    LONG padding, LONG clockWidth, LONG notificationWidth, std::span<const LONG> rightWidths,
    std::optional<std::size_t> controlIndex, bool merged = false, LONG trailingInset = 0)
{
    MergedStatusBarLayout result;
    if (merged) result.center = MergedStatusBarCenter(width, height, scale);
    const LONG centerWidth = result.center.right - result.center.left;
    clockWidth = std::max(0L, clockWidth);
    notificationWidth = std::max(0L, notificationWidth);
    if (controlIndex && *controlIndex >= rightWidths.size()) controlIndex.reset();
    const LONG controlWidth = controlIndex ? std::max(0L, rightWidths[*controlIndex]) : 0;
    std::vector<LONG> widths(rightWidths.begin(), rightWidths.end());
    widths.push_back(clockWidth + notificationWidth);
    LONG edge = std::max(0L, padding);
    const auto arrange = [&] { return StatusBarHorizontalLayout(width, height, edge,
        std::span<const LONG>{}, centerWidth, widths, trailingInset); };
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
inline MergedStatusBarLayout LayoutMergedStatusBar(LONG width, LONG height, float scale,
    LONG padding, LONG clockWidth, LONG notificationWidth, std::span<const LONG> rightWidths,
    std::optional<std::size_t> controlIndex)
{
    return LayoutRightStatusBar(width, height, scale, padding, clockWidth, notificationWidth,
        rightWidths, controlIndex, true);
}
}
