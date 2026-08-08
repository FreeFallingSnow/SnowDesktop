#pragma once

#include <algorithm>

namespace snowdesktop::popup_drag_rules
{

enum class DropPreviewLayer
{
    Background,
    Popup,
};

/** Keeps non-popup hit feedback below a foreground popup. */
constexpr DropPreviewLayer ResolveDropPreviewLayer(
    bool popupTarget)
{
    return popupTarget
        ? DropPreviewLayer::Popup
        : DropPreviewLayer::Background;
}

/** Uses the same forgiving icon-sized activation area for every popup host. */
template <typename Rect>
constexpr Rect HandoffActivationBounds(
    Rect iconBounds)
{
    iconBounds.left -= 4;
    iconBounds.top -= 2;
    iconBounds.right += 4;
    iconBounds.bottom += 4;
    return iconBounds;
}

/** Selected source items must remain sortable instead of targeting themselves. */
constexpr bool CanHandoffToItem(
    bool itemAvailable,
    bool itemSelected)
{
    return itemAvailable && !itemSelected;
}

/** Popup handoff activation may be icon-sized, but its feedback is cell-sized. */
template <typename Rect>
constexpr Rect HandoffIndicatorBounds(
    const Rect& itemBounds)
{
    return itemBounds;
}

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
