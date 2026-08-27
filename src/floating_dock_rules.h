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

// 被动 Dock hover 的同步提交限频窗口。hover 必须跟手，但也不需要每个
// WM_MOUSEMOVE 都同步重绘整个浮动 Dock。
inline constexpr ULONGLONG kPointerFrameIntervalMs = 8;

inline bool HasAnySummonTrigger(
    bool hotkeyEnabled, bool edgeSwipeEnabled)
{
    return hotkeyEnabled || edgeSwipeEnabled;
}

inline bool ShouldSummonForDockSurface(
    bool sourceBelongsToDock,
    bool floatingDockVisible)
{
    return sourceBelongsToDock && !floatingDockVisible;
}

inline bool ShouldUseFloatingDockLogicalForeground(
    bool keyboardSessionActive,
    bool actualWindowOwnedByCurrentProcess,
    bool shellFileOperationInFlight,
    bool actualWindowIsTaskWindow)
{
    const bool shellTransientWindow =
        shellFileOperationInFlight &&
        !actualWindowIsTaskWindow;
    return keyboardSessionActive &&
        (actualWindowOwnedByCurrentProcess ||
            shellTransientWindow ||
            !actualWindowIsTaskWindow);
}

inline bool ShouldRefocusFloatingDockKeyboardSession(
    bool floatingDockVisible,
    bool keyboardSessionActive,
    int shellFileOperationInFlight,
    int shellPopupMenuLayerDepth)
{
    return floatingDockVisible && keyboardSessionActive &&
        shellFileOperationInFlight == 0 &&
        shellPopupMenuLayerDepth == 0;
}

inline bool ShouldFloatingDockBeTopmost(
    bool floatingDockVisible,
    int shellPopupMenuLayerDepth)
{
    return floatingDockVisible && shellPopupMenuLayerDepth == 0;
}

inline bool ShouldChangeFloatingDockTopmost(
    bool currentlyTopmost,
    bool shouldBeTopmost)
{
    return currentlyTopmost != shouldBeTopmost;
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

/** @brief 常驻 DockHost 只要处于活动状态且未关闭事务挂起就可以继续绘制。 */
inline bool ShouldRenderFloatingDockFrame(
    bool dockHostActive,
    bool closePending)
{
    return dockHostActive && !closePending;
}

/**
 * @brief 仅当 Dock 已降回桌面层且不存在待完成事务时执行后续动作。
 *
 * 常驻 Host 在桌面态继续可见，不能再把 HWND 可见性当成关闭完成条件。
 */
inline bool CanRunPostCloseActionImmediately(
    bool floatingDockVisible,
    bool closePending)
{
    return !floatingDockVisible &&
        !closePending;
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
    bool itemDragFeedbackChanged,
    bool widgetPreviewActive,
    bool marqueeActive)
{
    return itemDragFeedbackChanged ||
        widgetPreviewActive ||
        marqueeActive;
}

/**
 * @brief 限制被动 Dock hover 的同步提交频率，同时保留拖动帧的即时反馈。
 *
 * 指针反馈必须同步提交，不能改成等待 UiAnimationScheduler 的下一帧。
 * f29a882 曾把所有 hover/拖拽帧改为 EnsureUiAnimationFrame()，导致快速
 * 扫过时 Dock 放大和拖拽虚影明显落后指针。
 */
inline bool ShouldPresentPointerFrame(
    ULONGLONG now,
    ULONGLONG lastPresent,
    bool forceImmediate)
{
    return forceImmediate ||
        lastPresent == 0 ||
        now < lastPresent ||
        now - lastPresent >= kPointerFrameIntervalMs;
}

inline UINT RemainingPointerFrameDelay(
    ULONGLONG now,
    ULONGLONG lastPresent)
{
    if (lastPresent == 0 || now < lastPresent)
        return 0;
    const ULONGLONG elapsed = now - lastPresent;
    if (elapsed >= kPointerFrameIntervalMs)
        return 0;
    return static_cast<UINT>(
        kPointerFrameIntervalMs - elapsed);
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

inline RECT DockAssociatedPopupInteractionRect(
    bool anchoredToDock,
    const RECT& hostedPopupRect)
{
    return anchoredToDock
        ? hostedPopupRect
        : RECT{};
}

inline bool ShouldDismissForPointerDown(
    bool dragging,
    bool contextMenuActive,
    POINT desktopPoint,
    const RECT& dockRect,
    const RECT& popupRect,
    const RECT& previewRect,
    const RECT& siblingInteractionRect = RECT{})
{
    if (dragging || contextMenuActive)
        return false;
    // While a menu owns the mouse loop, its item presses are outside the Dock
    // rects but must still reach the menu command instead of tearing the host
    // down. The menu itself handles outside-click dismissal.
    // Dock-owned auxiliary surfaces receive their own clicks without tearing
    // down the floating host. Other top-level surfaces decide independently.
    if (PtInRect(&dockRect, desktopPoint) ||
        (!IsRectEmpty(&previewRect) &&
            PtInRect(&previewRect, desktopPoint)) ||
        (!IsRectEmpty(&siblingInteractionRect) &&
            PtInRect(&siblingInteractionRect, desktopPoint)))
        return false;
    return IsRectEmpty(&popupRect) ||
        !PtInRect(&popupRect, desktopPoint);
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

inline bool IsTooltipOnlyPoint(
    POINT desktopPoint,
    const RECT& dockRect,
    const RECT& popupRect,
    const RECT& tooltipRect)
{
    if (IsRectEmpty(&tooltipRect) ||
        !PtInRect(&tooltipRect, desktopPoint))
        return false;
    if (PtInRect(&dockRect, desktopPoint))
        return false;
    return IsRectEmpty(&popupRect) ||
        !PtInRect(&popupRect, desktopPoint);
}

} // namespace snowdesktop::floating_dock_rules
