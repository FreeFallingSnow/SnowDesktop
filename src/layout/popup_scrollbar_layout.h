#pragma once

#include <windows.h>
#include <algorithm>

namespace snowdesktop::popup_scrollbar_layout
{
// Keep the content's vertical scroll range, but anchor the scrollbar to the
// panel edge. Content padding belongs to the icon/label layout, not the track.
inline RECT Viewport(const RECT& panel, const RECT& content) noexcept
{
    if (IsRectEmpty(&panel) || IsRectEmpty(&content)) return {};
    RECT viewport{
        std::clamp(content.left, panel.left, panel.right),
        std::clamp(content.top, panel.top, panel.bottom),
        panel.right,
        std::clamp(content.bottom, panel.top, panel.bottom)};
    return IsRectEmpty(&viewport) ? RECT{} : viewport;
}
}
