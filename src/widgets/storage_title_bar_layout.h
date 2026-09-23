#pragma once

#include "../types.h"
#include "widget_chrome_rules.h"

namespace snowdesktop::storage_title_bar
{

inline bool UsesTop(const DesktopWidget& widget, bool enabled) noexcept
{
    if (!enabled) return false;
    switch (widget.type)
    {
    case DesktopWidgetType::Collection:
        return widget.scrollContainerMode &&
            (widget.gridSpan.columns > 1 || widget.gridSpan.rows > 1);
    case DesktopWidgetType::FileCategories:
    case DesktopWidgetType::FolderMapping:
    case DesktopWidgetType::CollectionGroup:
    case DesktopWidgetType::FileGroup:
        return true;
    default:
        return false;
    }
}

struct Layout
{
    RECT body{};
    RECT titleBar{};
    RECT resize{};
    LONG contentBottom = 0;
};

inline bool IsCollapsed(const DesktopWidget& widget, bool topEnabled,
    bool itemDragActive, bool externalDragActive, bool widgetMoveActive) noexcept
{
    return UsesTop(widget, topEnabled) && widget.titleBarCollapsed &&
        !itemDragActive && !externalDragActive && !widgetMoveActive;
}

inline RECT VisibleFrame(RECT fullFrame, bool collapsed,
    int titleHeight, int edgeGap) noexcept
{
    if (collapsed && fullFrame.bottom > fullFrame.top)
        fullFrame.bottom = std::min<LONG>(fullFrame.bottom,
            fullFrame.top + titleHeight + 2 * edgeGap);
    return fullFrame;
}

inline bool ExpandOnHover(bool eligible, bool wasExpanded, bool suppressed,
    bool inFrame, bool interactionRetained) noexcept
{
    return eligible && !suppressed &&
        (inFrame || (wasExpanded && interactionRetained));
}

// Reserve both sides equally so the title stays at the widget's true center,
// regardless of the number of action buttons on the right.
inline RECT CenteredTitleRect(RECT bar, int leadingReserve,
    int trailingReserve, int verticalInset) noexcept
{
    const LONG center = bar.left + (bar.right - bar.left) / 2;
    const int reserve = std::max(leadingReserve, trailingReserve);
    return {std::min<LONG>(center, bar.left + reserve),
        bar.top + verticalInset,
        std::max<LONG>(center, bar.right - reserve),
        bar.bottom - verticalInset};
}

// All inputs are scaled pixels. The resize target always uses the original
// bottom-bar height, independently of the tab-height-driven top title bar.
inline Layout Resolve(RECT frame, bool top, int titleHeight,
    int bottomHeight, int bottomReserve, int cornerRadius,
    int minimumInset, int edgeGap) noexcept
{
    const auto bar = [&](int height, bool atTop) -> RECT {
        const int cornerInset = widget_chrome_rules::BottomBarSideInset(
            cornerRadius, height, minimumInset, edgeGap);
        // A top toolbar needs breathing room as well as geometric clearance.
        // Both action groups and the centered title share this symmetric inset.
        const int sideInset = atTop
            ? std::max({cornerInset, 2 * minimumInset,
                std::max(0, cornerRadius) / 2 + minimumInset})
            : cornerInset;
        const int inset = std::min(
            sideInset,
            std::max<int>(0, (frame.right - frame.left - 1) / 2));
        const LONG y = atTop
            ? std::min<LONG>(frame.bottom, frame.top + edgeGap)
            : std::max<LONG>(frame.top, frame.bottom - height - edgeGap);
        return {frame.left + inset, y, frame.right - inset,
            atTop ? std::min<LONG>(frame.bottom, y + height)
                  : frame.bottom - edgeGap};
    };
    Layout result;
    result.titleBar = bar(titleHeight, top);
    result.resize = top ? bar(bottomHeight, false) : result.titleBar;
    result.resize.left = std::max<LONG>(result.resize.left,
        result.resize.right - bottomHeight);
    result.body = frame;
    if (top)
        result.body.top = result.titleBar.bottom;
    else
        result.body.bottom = std::max<LONG>(
            frame.top + bottomReserve, frame.bottom - bottomReserve);
    result.contentBottom = top ? frame.bottom - edgeGap : result.titleBar.top;
    return result;
}

} // namespace snowdesktop::storage_title_bar
