#pragma once

#include <windows.h>
#include <algorithm>
#include <optional>

namespace snowdesktop::usage_guide
{
// All text remains in one document; only the viewport moves on small screens.
struct PanelScroll
{
    int offset = 0, maximum = 0;
    void Arrange(int contentHeight, int viewportHeight)
    {
        maximum = std::max(0, contentHeight - viewportHeight);
        offset = std::clamp(offset, 0, maximum);
    }
    void By(int pixels) { offset = std::clamp(offset + pixels, 0, maximum); }
    void ToFraction(double value)
    { offset = static_cast<int>(std::clamp(value, 0.0, 1.0) * maximum); }
};
// Screen coordinates survive changes to the desktop host's virtual origin.
// Only an explicit drag changes the anchor, except when the work area shrinks
// or the panel grows beyond its edge. Menus and desktop objects are not inputs.
struct PanelPlacement
{
    std::optional<POINT> anchor;
    std::optional<POINT> dragOffset;

    RECT Arrange(RECT workArea, LONG width, LONG height, LONG margin)
    {
        const LONG left = workArea.left + margin, top = workArea.top + margin;
        const LONG right = std::max(left, workArea.right - margin - width);
        const LONG bottom = std::max(top, workArea.bottom - margin - height);
        if (!anchor) anchor = POINT{right, bottom};
        anchor = POINT{std::clamp(anchor->x, left, right), std::clamp(anchor->y, top, bottom)};
        return {anchor->x, anchor->y, anchor->x + width, anchor->y + height};
    }
    void BeginDrag(POINT pointer)
    {
        if (anchor) dragOffset = POINT{pointer.x - anchor->x, pointer.y - anchor->y};
    }
    bool DragTo(POINT pointer, bool primaryButtonDown, bool ownsCapture)
    {
        // A queued move may arrive before button-up, or after capture loss.
        // Never apply its position once the physical gesture has ended.
        if (!primaryButtonDown || !ownsCapture) EndDrag();
        if (!dragOffset) return false;
        anchor = POINT{pointer.x - dragOffset->x, pointer.y - dragOffset->y};
        return true;
    }
    void EndDrag() { dragOffset.reset(); }
};
}
