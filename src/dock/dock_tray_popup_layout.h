#pragma once
#include <windows.h>
#include <algorithm>
#include <span>
#include <string_view>

namespace snowdesktop::dock_tray::popup
{
inline std::wstring_view Label(std::wstring_view tooltip, std::wstring_view shortcutName)
{
    return tooltip.empty() ? shortcutName : tooltip;
}
inline RECT Viewport(RECT content, int footerHeight)
{
    content.bottom = (std::max)(content.top, content.bottom - footerHeight);
    return content;
}
inline RECT LaunchButton(const RECT& content, int footerHeight, int gap)
{
    return {content.left, (std::max)(content.top, content.bottom - footerHeight + gap),
        content.right, content.bottom};
}
inline RECT Row(const RECT& viewport, std::span<const int> heights, size_t index, int scroll, int gap)
{
    if (index >= heights.size()) return {};
    LONG top = viewport.top - scroll;
    for (size_t row = 0; row < index; ++row) top += heights[row];
    return {viewport.left, top, viewport.right, top + (std::max)(1, heights[index] - gap)};
}
inline int MaximumScroll(const RECT& viewport, std::span<const int> heights)
{
    int total = 0;
    for (int height : heights) total += height;
    return (std::max)(0, total - static_cast<int>(viewport.bottom - viewport.top));
}
inline int HitTest(POINT point, const RECT& viewport, const RECT& launch,
    std::span<const int> heights, int scroll, int gap)
{
    if (PtInRect(&launch, point)) return static_cast<int>(heights.size());
    if (!PtInRect(&viewport, point)) return -1;
    for (size_t index = 0; index < heights.size(); ++index)
    {
        const auto row = Row(viewport, heights, index, scroll, gap);
        if (PtInRect(&row, point)) return static_cast<int>(index);
    }
    return -1;
}
}
