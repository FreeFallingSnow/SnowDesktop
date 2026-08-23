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

/** List rows insert vertically; icon grids insert horizontally. */
template <typename Rect, typename Point>
constexpr bool IsAfterInsertionMidpoint(
    const Rect& itemBounds,
    const Point& point,
    bool listMode)
{
    return listMode
        ? point.y >= itemBounds.top +
            (itemBounds.bottom - itemBounds.top) / 2
        : point.x >= itemBounds.left +
            (itemBounds.right - itemBounds.left) / 2;
}

/**
 * Measures a pointer against a leading/trailing insertion edge while keeping
 * the perpendicular distance inside the visible portion of a clipped item.
 */
template <typename Rect, typename Point>
constexpr long long InsertionEdgeDistanceSquared(
    const Rect& itemBounds,
    const Rect& visibleBounds,
    const Point& point,
    bool listMode,
    bool after,
    long gutter)
{
    gutter = std::max(0L, gutter);
    long long primary = 0;
    long long perpendicular = 0;
    if (listMode)
    {
        const long edge = after
            ? itemBounds.bottom + gutter
            : itemBounds.top - gutter;
        primary = static_cast<long long>(point.y) - edge;
        if (point.x < visibleBounds.left)
            perpendicular = static_cast<long long>(
                visibleBounds.left) - point.x;
        else if (point.x >= visibleBounds.right)
            perpendicular = static_cast<long long>(point.x) -
                visibleBounds.right + 1;
    }
    else
    {
        const long edge = after
            ? itemBounds.right + gutter
            : itemBounds.left - gutter;
        primary = static_cast<long long>(point.x) - edge;
        if (point.y < visibleBounds.top)
            perpendicular = static_cast<long long>(
                visibleBounds.top) - point.y;
        else if (point.y >= visibleBounds.bottom)
            perpendicular = static_cast<long long>(point.y) -
                visibleBounds.bottom + 1;
    }
    return primary * primary + perpendicular * perpendicular;
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

/** Keeps the first/last horizontal insertion bars visible for list rows. */
template <typename Rect>
constexpr Rect ExpandInsertionClipVertically(
    Rect content,
    const Rect& popup,
    long gutter)
{
    gutter = std::max(0L, gutter);
    content.top = std::max(
        popup.top,
        content.top - gutter);
    content.bottom = std::min(
        popup.bottom,
        content.bottom + gutter);
    return content;
}

} // namespace snowdesktop::popup_drag_rules
