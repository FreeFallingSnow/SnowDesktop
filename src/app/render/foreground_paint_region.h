#pragma once

#include <windows.h>

namespace snowdesktop::foreground_paint_region
{
// The caller supplies a conservative pixel envelope, including any stroke or
// effect overdraw. Unknown/empty geometry keeps the original full draw path.
inline bool Intersects(const RECT* update, RECT bounds, int padding = 2)
{
    if (!update || IsRectEmpty(&bounds)) return true;
    InflateRect(&bounds, padding, padding);
    RECT intersection{};
    return IntersectRect(&intersection, update, &bounds) != FALSE;
}
}
