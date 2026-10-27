#pragma once
#include "status_bar_view.h"
#include <span>

namespace snowdesktop
{
// Reuse each arranged visible hit target, including empty clipped cells.
// The mapping boundary is supplied by the HWND owner; production retains its
// existing per-RECT MapWindowPoints semantics, while offscreen tests translate
// into fixed signed screen coordinates without a desktop host.
template<class MapToScreen>
std::vector<tray::GeometryUpdate> StatusBarTrayGeometries(
    std::span<const StatusBarItem> items, MapToScreen&& mapToScreen, bool clear = false)
{
    std::vector<tray::GeometryUpdate> result;
    for (const auto& item : items)
    {
        if (!item.icon) continue;
        RECT screen = clear ? RECT{} : item.bounds;
        if (!IsRectEmpty(&screen)) mapToScreen(screen);
        result.push_back({item.icon->key, screen});
    }
    return result;
}
}
