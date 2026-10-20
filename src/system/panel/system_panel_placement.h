#pragma once
#include "dock/dock_layout_settings.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace snowdesktop
{
enum class SystemPanelAlignment { IconRight, IconCenter, ScreenRight };

inline RECT PlaceSystemPanel(RECT anchor, SIZE size, RECT work, DockPosition edge,
    float scale, SystemPanelAlignment alignment)
{
    const LONG gap = static_cast<LONG>(std::lround(6 * scale));
    // Keep both screen edges inset, even when the panel follows an edge icon.
    const LONG inset = (std::min)(static_cast<LONG>(std::lround(8 * scale)),
        (std::max)(0L, (work.right - work.left - size.cx) / 2));
    LONG left = alignment == SystemPanelAlignment::ScreenRight ? work.right - inset - size.cx :
        alignment == SystemPanelAlignment::IconCenter ? (anchor.left + anchor.right - size.cx) / 2 : anchor.right - size.cx;
    LONG top = edge == DockPosition::Bottom ? anchor.top - size.cy - gap : anchor.bottom + gap;
    left = std::clamp(left, work.left + inset,
        (std::max)(work.left + inset, work.right - inset - size.cx));
    top = std::clamp(top, work.top, (std::max)(work.top, work.bottom - size.cy));
    return {left, top, left + size.cx, top + size.cy};
}
}
