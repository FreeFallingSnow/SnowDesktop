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
    bool nativeDragActive,
    bool primaryButtonDown)
{
    // A queued move can be dispatched after the physical button was released
    // but before WM_LBUTTONUP reaches the queue head. In that interval the
    // release message remains authoritative; do not sample a later position.
    return nativeDragActive && primaryButtonDown;
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

constexpr bool IsNativeDragMessageSurface(
    bool mainDesktopWindow,
    bool floatingDockWindow,
    bool floatingPopupWindow)
{
    return mainDesktopWindow ||
        floatingDockWindow || floatingPopupWindow;
}

constexpr bool ShouldStartQueuedMouseMoveCoalescing(
    bool nativeDragActive,
    bool nativeDragMessageSurface,
    bool messageIsMouseMove)
{
    return nativeDragActive &&
        nativeDragMessageSurface && messageIsMouseMove;
}

constexpr bool ShouldCoalesceQueuedMouseMove(
    bool nativeDragActive,
    bool sameWindow,
    bool nextMessageIsMouseMove)
{
    // The caller only inspects the queue head. This preserves ordering with
    // button, key, timer and window messages while dropping superseded points.
    return nativeDragActive && sameWindow && nextMessageIsMouseMove;
}

template <typename Message, typename PeekNext, typename RemoveNext,
    typename SameWindow, typename IsMouseMove>
std::size_t CoalesceQueuedMouseMoves(
    bool nativeDragActive,
    bool nativeDragMessageSurface,
    Message& current,
    PeekNext&& peekNext,
    RemoveNext&& removeNext,
    SameWindow&& sameWindow,
    IsMouseMove&& isMouseMove)
{
    if (!ShouldStartQueuedMouseMoveCoalescing(
            nativeDragActive,
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
                nativeDragActive,
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
