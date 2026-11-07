#pragma once
#include <windows.h>
#include <algorithm>

namespace snowdesktop::modern_menu::root_placement
{
// Center the real measured panel over its control, then retain full visibility
// on the selected monitor, including negative virtual-desktop coordinates.
inline POINT CenteredAbove(const RECT& anchor, SIZE panel, const RECT& work) noexcept
{
    return {
        std::clamp((anchor.left + anchor.right - panel.cx) / 2,
            work.left, (std::max)(work.left, work.right - panel.cx)),
        std::clamp(anchor.top - panel.cy,
            work.top, (std::max)(work.top, work.bottom - panel.cy))
    };
}
}
