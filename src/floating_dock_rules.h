#pragma once

#include "dock_settings.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>

namespace snowdesktop::floating_dock_rules
{

inline constexpr DWORD kWindowExStyle =
    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;

inline constexpr int kEdgeSwipeBandDip = 4;
inline constexpr int kEdgeSwipeTravelDip = 72;
inline constexpr DWORD kEdgeSwipeMaximumDurationMs = 480;

inline bool HasAnySummonTrigger(
    bool hotkeyEnabled, bool edgeSwipeEnabled)
{
    return hotkeyEnabled || edgeSwipeEnabled;
}

inline int ScaleEdgeSwipeDip(int value, UINT dpi)
{
    return std::max(1, MulDiv(
        value, static_cast<int>(std::max<UINT>(96, dpi)), 96));
}

inline bool IsPointOnDockScreenEdge(
    POINT point, const RECT& monitorRect,
    DockPosition position, int edgeBand)
{
    if (IsRectEmpty(&monitorRect) ||
        point.x < monitorRect.left ||
        point.x >= monitorRect.right ||
        point.y < monitorRect.top ||
        point.y >= monitorRect.bottom)
        return false;

    edgeBand = std::max(0, edgeBand);
    switch (position)
    {
    case DockPosition::Top:
        return point.y - monitorRect.top <= edgeBand;
    case DockPosition::Left:
        return point.x - monitorRect.left <= edgeBand;
    case DockPosition::Right:
        return monitorRect.right - 1 - point.x <= edgeBand;
    case DockPosition::Bottom:
    default:
        return monitorRect.bottom - 1 - point.y <= edgeBand;
    }
}

inline bool HasNewPointerButtonPress(
    UINT buttonsDown, UINT previousButtonsDown,
    UINT pressedSinceLastSample)
{
    return pressedSinceLastSample != 0 ||
        (buttonsDown & ~previousButtonsDown) != 0;
}

/**
 * @brief Detects a quick pointer stroke that remains on the Dock-facing edge.
 *
 * The gesture is parallel to the edge: horizontal for top/bottom Docks and
 * vertical for left/right Docks. A completed gesture stays latched until the
 * pointer leaves the edge, preventing one long stroke from opening repeatedly.
 */
class EdgeSwipeDetector
{
public:
    bool Update(
        POINT point, const RECT& monitorRect,
        DockPosition position, DWORD tick,
        int edgeBand, int requiredTravel,
        DWORD maximumDurationMs =
            kEdgeSwipeMaximumDurationMs)
    {
        if (!IsPointOnDockScreenEdge(
                point, monitorRect, position, edgeBand))
        {
            Reset();
            return false;
        }
        if (awaitingEdgeLeave_)
            return false;

        const bool contextChanged =
            !tracking_ ||
            position != position_ ||
            !EqualRect(&monitorRect_, &monitorRect);
        const DWORD elapsed = tick - startTick_;
        if (contextChanged ||
            (tracking_ && elapsed > maximumDurationMs))
        {
            tracking_ = true;
            monitorRect_ = monitorRect;
            position_ = position;
            startPoint_ = point;
            startTick_ = tick;
            return false;
        }

        requiredTravel = std::max(1, requiredTravel);
        const LONG alongEdge =
            position == DockPosition::Top ||
                position == DockPosition::Bottom
            ? point.x - startPoint_.x
            : point.y - startPoint_.y;
        if (std::abs(alongEdge) < requiredTravel)
            return false;

        tracking_ = false;
        awaitingEdgeLeave_ = true;
        return true;
    }

    void Reset()
    {
        tracking_ = false;
        awaitingEdgeLeave_ = false;
        monitorRect_ = {};
        startPoint_ = {};
        startTick_ = 0;
    }

    bool IsTracking() const { return tracking_; }
    bool IsAwaitingEdgeLeave() const
    {
        return awaitingEdgeLeave_;
    }

private:
    bool tracking_ = false;
    bool awaitingEdgeLeave_ = false;
    RECT monitorRect_{};
    POINT startPoint_{};
    DockPosition position_ = DockPosition::Bottom;
    DWORD startTick_ = 0;
};

inline RECT UnionNonEmptyRects(const RECT& first, const RECT& second)
{
    if (IsRectEmpty(&first))
        return second;
    if (IsRectEmpty(&second))
        return first;
    RECT result{};
    UnionRect(&result, &first, &second);
    return result;
}

inline RECT ExpandHostForTitleLayer(
    RECT dockRect, DockPosition position)
{
    // The title chip is at most 260x30 with an 8px gap. Keep this
    // allocation stable while the pointer moves; only the exact title
    // chip is added to the HWND region, so the transparent reserve never
    // receives input.
    constexpr int titleWidthAxisPadding = 134;
    constexpr int titleHeightAxisPadding = 18;
    constexpr int titleWidthAndGap = 272;
    constexpr int titleHeightAndGap = 42;
    switch (position)
    {
    case DockPosition::Top:
        dockRect.left -= titleWidthAxisPadding;
        dockRect.right += titleWidthAxisPadding;
        dockRect.bottom += titleHeightAndGap;
        break;
    case DockPosition::Left:
        dockRect.top -= titleHeightAxisPadding;
        dockRect.bottom += titleHeightAxisPadding;
        dockRect.right += titleWidthAndGap;
        break;
    case DockPosition::Right:
        dockRect.top -= titleHeightAxisPadding;
        dockRect.bottom += titleHeightAxisPadding;
        dockRect.left -= titleWidthAndGap;
        break;
    case DockPosition::Bottom:
    default:
        dockRect.left -= titleWidthAxisPadding;
        dockRect.right += titleWidthAxisPadding;
        dockRect.top -= titleHeightAndGap;
        break;
    }
    return dockRect;
}

inline RECT ExpandForBorderOverdraw(
    RECT visualRect)
{
    // DrawGlassBorder's antialiased outer pass is wider than the logical
    // one-pixel border. Desktop rendering has an unrestricted surface around
    // it; floating layers must explicitly reserve the same pixels.
    constexpr int borderOverdraw = 2;
    if (!IsRectEmpty(&visualRect))
        InflateRect(
            &visualRect,
            borderOverdraw,
            borderOverdraw);
    return visualRect;
}

inline RECT ReserveCollectionPopupEnvelope(
    const RECT& dockRect,
    const RECT& workArea,
    DockPosition position,
    SIZE maximumPopupSize)
{
    if (maximumPopupSize.cx <= 0 ||
        maximumPopupSize.cy <= 0)
        return RECT{};

    constexpr int popupGap = 12;
    constexpr int placementSlack = 48;
    RECT envelope{};
    switch (position)
    {
    case DockPosition::Top:
        envelope = RECT{
            dockRect.left - maximumPopupSize.cx / 2,
            dockRect.top,
            dockRect.right + maximumPopupSize.cx / 2,
            dockRect.bottom + popupGap +
                maximumPopupSize.cy
        };
        break;
    case DockPosition::Left:
        envelope = RECT{
            dockRect.left,
            dockRect.top - maximumPopupSize.cy / 2,
            dockRect.right + popupGap +
                maximumPopupSize.cx,
            dockRect.bottom + maximumPopupSize.cy / 2
        };
        break;
    case DockPosition::Right:
        envelope = RECT{
            dockRect.left - popupGap -
                maximumPopupSize.cx,
            dockRect.top - maximumPopupSize.cy / 2,
            dockRect.right,
            dockRect.bottom + maximumPopupSize.cy / 2
        };
        break;
    case DockPosition::Bottom:
    default:
        envelope = RECT{
            dockRect.left - maximumPopupSize.cx / 2,
            dockRect.top - popupGap -
                maximumPopupSize.cy,
            dockRect.right + maximumPopupSize.cx / 2,
            dockRect.bottom
        };
        break;
    }
    InflateRect(
        &envelope,
        placementSlack,
        placementSlack);

    RECT popupWork = workArea;
    InflateRect(&popupWork, -popupGap, -popupGap);
    RECT clipped{};
    if (!IntersectRect(
            &clipped, &envelope, &popupWork))
        return RECT{};
    return clipped;
}

inline bool ShouldRenderDesktopDock(
    bool floatingDockOwnsVisual,
    bool selectedForFloatingHost)
{
    return !floatingDockOwnsVisual ||
        !selectedForFloatingHost;
}

/**
 * @brief The source copy can be retired only after a valid replacement frame
 * has crossed the compositor presentation barrier.
 */
inline bool ShouldRetireDesktopDockCopy(
    bool floatingFrameReady,
    bool presentationBarrierSucceeded)
{
    return floatingFrameReady &&
        presentationBarrierSucceeded;
}

/**
 * @brief 浮动层接收鼠标时，其 hover 帧只应提交到浮动窗口。
 *
 * 顶层 Dock 已替代对应的桌面 Dock；继续让每个浮动 WM_MOUSEMOVE 排队
 * 重绘桌面层会挤占 UI 线程，并让快速扫过时的放大效果落后于指针。
 */
inline bool ShouldInvalidateDesktopHover(
    bool handlingFloatingDockInput)
{
    return !handlingFloatingDockInput;
}

/**
 * @brief 需要在当前指针消息结束前同步提交的交互状态。
 */
inline bool NeedsImmediatePointerPresent(
    bool itemDragActive,
    bool widgetPreviewActive,
    bool marqueeActive)
{
    return itemDragActive ||
        widgetPreviewActive ||
        marqueeActive;
}

/**
 * @brief Dock 在桌面层和顶层窗口之间切换会改变拖动静态帧内容。
 */
inline bool FloatingVisibilityChangesStaticScene(
    bool wasVisible,
    bool isVisible)
{
    return wasVisible != isVisible;
}

inline bool ShouldCloseCollectionPopup(
    std::size_t openWidgetIndex,
    std::size_t clickedWidgetIndex)
{
    return openWidgetIndex !=
            static_cast<std::size_t>(-1) &&
        openWidgetIndex == clickedWidgetIndex;
}

inline bool ShouldCloseCollectionPopupOnPointerDown(
    std::size_t openWidgetIndex,
    std::size_t pressedDockWidgetIndex,
    bool pointInsidePopup)
{
    if (openWidgetIndex ==
            static_cast<std::size_t>(-1) ||
        pointInsidePopup)
        return false;
    // The collection button that owns the popup is its toggle control. Keep
    // the popup state intact until button release so one click cannot become
    // close-on-down followed by open-on-up.
    return !ShouldCloseCollectionPopup(
        openWidgetIndex,
        pressedDockWidgetIndex);
}

inline POINT WindowPointToDesktopPoint(
    POINT windowPoint, const RECT& desktopSourceRect)
{
    return POINT{
        windowPoint.x + desktopSourceRect.left,
        windowPoint.y + desktopSourceRect.top
    };
}

inline RECT DesktopRectToWindowRect(
    RECT desktopRect, const RECT& desktopSourceRect)
{
    OffsetRect(
        &desktopRect,
        -desktopSourceRect.left,
        -desktopSourceRect.top);
    return desktopRect;
}

inline bool ShouldDismissForPointerDown(
    bool dragging,
    POINT desktopPoint,
    const RECT& dockRect,
    const RECT& popupRect)
{
    if (dragging)
        return false;
    return !PtInRect(&dockRect, desktopPoint) &&
        (IsRectEmpty(&popupRect) ||
            !PtInRect(&popupRect, desktopPoint));
}

inline bool IsPointInVisibleLayer(
    POINT desktopPoint,
    const RECT& dockRect,
    const RECT& popupRect,
    const RECT& tooltipRect)
{
    return PtInRect(
               &dockRect,
               desktopPoint) != FALSE ||
        (!IsRectEmpty(&popupRect) &&
            PtInRect(
                &popupRect,
                desktopPoint) != FALSE) ||
        (!IsRectEmpty(&tooltipRect) &&
            PtInRect(
                &tooltipRect,
                desktopPoint) != FALSE);
}

} // namespace snowdesktop::floating_dock_rules
