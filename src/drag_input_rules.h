#pragma once

#include <cstddef>

namespace snowdesktop::drag_input_rules
{
constexpr bool IsNativeDragActive(
    bool dragSessionActive,
    bool dragTransportActive)
{
    // OLE owns screen-point routing once a transport starts. Before that,
    // captured native drags should follow the physical pointer instead of
    // replaying queued WM_MOUSEMOVE coordinates.
    return dragSessionActive && !dragTransportActive;
}

constexpr bool IsLatencySensitivePointerGesture(
    bool nativeDragActive,
    bool marqueePointerActive,
    bool widgetActionActive,
    bool widgetTargetValid)
{
    return nativeDragActive || marqueePointerActive ||
        (widgetActionActive && widgetTargetValid);
}

constexpr bool ShouldDeferModelReload(
    bool retainedDragContext,
    bool dragTransportActive)
{
    // DragSession keeps Item/Container bindings after DeactivateForDrop so a
    // synchronous Shell target can finish the drop. OLE transport can also
    // remain on the stack after that context ends. Replacing the desktop model
    // in either interval can invalidate the hand-back/drop state or perform an
    // expensive reload inside a nested message loop.
    return retainedDragContext || dragTransportActive;
}

constexpr bool ShouldSampleLivePointer(
    bool latencySensitivePointerActive,
    bool gestureButtonDown)
{
    // A queued move can be dispatched after the physical button was released
    // but before its button-up reaches the queue head. In that interval the
    // queued release remains authoritative; do not sample a later position.
    return latencySensitivePointerActive && gestureButtonDown;
}

constexpr bool IsPointerGestureButtonDown(
    bool middleButtonWidgetMove,
    bool primaryButtonDown,
    bool middleButtonDown)
{
    return middleButtonWidgetMove ? middleButtonDown : primaryButtonDown;
}

constexpr bool IsMarqueePointerGesture(
    bool marqueeActive,
    bool mouseDown,
    bool hasMouseDownHit,
    bool guideActionPending,
    bool widgetActionActive,
    bool middleButtonWidgetMove,
    bool detailColumnResizeActive,
    bool widgetScrollbarDragging,
    bool popupScrollbarDragging,
    bool luaWidgetPanelMouseDown,
    bool hasMarqueeTarget)
{
    if (marqueeActive)
        return true;
    return mouseDown && !hasMouseDownHit &&
        !guideActionPending && !widgetActionActive &&
        !middleButtonWidgetMove && !detailColumnResizeActive &&
        !widgetScrollbarDragging && !popupScrollbarDragging &&
        !luaWidgetPanelMouseDown && hasMarqueeTarget;
}

constexpr bool ShouldSampleFloatingWindowPointer(
    bool nativeDragActive,
    bool primaryButtonDown)
{
    // Floating surfaces normally reconcile hover with the physical cursor.
    // During a native drag, however, a queued move may arrive after the
    // physical release but before WM_LBUTTONUP. Preserve that release barrier
    // by falling back to the queued point until the button-up is dispatched.
    return !nativeDragActive || primaryButtonDown;
}

constexpr bool IsLatencySensitivePointerMessageSurface(
    bool mainDesktopWindow,
    bool floatingDockWindow,
    bool floatingPopupWindow)
{
    return mainDesktopWindow ||
        floatingDockWindow || floatingPopupWindow;
}

constexpr bool ShouldStartQueuedMouseMoveCoalescing(
    bool latencySensitivePointerActive,
    bool nativeDragMessageSurface,
    bool messageIsMouseMove)
{
    return latencySensitivePointerActive &&
        nativeDragMessageSurface && messageIsMouseMove;
}

constexpr bool ShouldCoalesceQueuedMouseMove(
    bool latencySensitivePointerActive,
    bool sameWindow,
    bool nextMessageIsMouseMove)
{
    // The caller only inspects the queue head. This preserves ordering with
    // button, key, timer and window messages while dropping superseded points.
    return latencySensitivePointerActive &&
        sameWindow && nextMessageIsMouseMove;
}

template <typename Message, typename PeekNext, typename RemoveNext,
    typename SameWindow, typename IsMouseMove>
std::size_t CoalesceQueuedMouseMoves(
    bool latencySensitivePointerActive,
    bool nativeDragMessageSurface,
    Message& current,
    PeekNext&& peekNext,
    RemoveNext&& removeNext,
    SameWindow&& sameWindow,
    IsMouseMove&& isMouseMove)
{
    if (!ShouldStartQueuedMouseMoveCoalescing(
            latencySensitivePointerActive,
            nativeDragMessageSurface,
            isMouseMove(current)))
    {
        return 0;
    }

    std::size_t coalesced = 0;
    Message next{};
    while (peekNext(next))
    {
        if (!ShouldCoalesceQueuedMouseMove(
                latencySensitivePointerActive,
                sameWindow(current, next),
                isMouseMove(next)))
        {
            break;
        }
        if (!removeNext(current))
            break;
        ++coalesced;
    }
    return coalesced;
}
}
