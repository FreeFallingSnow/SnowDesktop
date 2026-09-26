#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <windows.h>

namespace snowdesktop
{
enum class NativeTooltipPlacement { Below, Above };

// Identity owns the hover deadline; content does not. Shared by the native
// host and the status-bar preview so fresh samples cannot freeze old text.
struct NativeTooltipState
{
    std::string key;
    std::wstring text;
    std::wstring title;
    std::uint64_t readyAt = 0;

    bool Enter(std::string next, std::wstring label,
        std::wstring heading = {}, std::uint64_t now = 0, unsigned delay = 0)
    {
        const bool changed = key != next;
        if (changed) { key = std::move(next); readyAt = now + delay; }
        if (text != label) text = std::move(label);
        if (title != heading) title = std::move(heading);
        return changed;
    }
    void Leave() { key.clear(); text.clear(); title.clear(); readyAt = 0; }
};

// All coordinates are physical screen pixels. Keep the entire popup on the
// anchor monitor, including negative-origin monitors and narrow work areas.
inline RECT PlaceNativeTooltip(RECT anchor, SIZE size, RECT work,
    NativeTooltipPlacement placement, LONG gap, LONG margin)
{
    margin = std::max(0L, std::min({margin,
        (work.right - work.left - 1) / 2, (work.bottom - work.top - 1) / 2}));
    const LONG left = work.left + margin, right = work.right - margin;
    const LONG top = work.top + margin, bottom = work.bottom - margin;
    const LONG width = std::clamp(size.cx, 1L, std::max(1L, right - left));
    const LONG height = std::clamp(size.cy, 1L, std::max(1L, bottom - top));
    const LONG below = anchor.bottom + gap, above = anchor.top - gap - height;
    LONG y = placement == NativeTooltipPlacement::Above ? above : below;
    if (placement == NativeTooltipPlacement::Above && y < top && below + height <= bottom) y = below;
    if (placement == NativeTooltipPlacement::Below && y + height > bottom && above >= top) y = above;
    const LONG x = std::clamp(anchor.left + (anchor.right - anchor.left - width) / 2,
        left, std::max(left, right - width));
    y = std::clamp(y, top, std::max(top, bottom - height));
    return {x, y, x + width, y + height};
}
}
