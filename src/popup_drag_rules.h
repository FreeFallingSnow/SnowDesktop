#pragma once

#include <algorithm>

namespace snowdesktop::popup_drag_rules
{

/**
 * @brief 为网格弹窗的左右插入指示器保留裁剪空间。
 *
 * 内容本身仍按 viewport 的上下边界裁剪滚动项；只沿水平方向扩展到
 * 弹窗内边距，避免首列 SortBefore 与末列 SortAfter 指示条被裁掉。
 */
template <typename Rect>
constexpr Rect ExpandInsertionClipHorizontally(
    Rect content,
    const Rect& popup,
    long gutter)
{
    gutter = std::max(0L, gutter);
    content.left = std::max(
        popup.left,
        content.left - gutter);
    content.right = std::min(
        popup.right,
        content.right + gutter);
    return content;
}

} // namespace snowdesktop::popup_drag_rules
