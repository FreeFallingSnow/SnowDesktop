#pragma once

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

constexpr bool ShouldSampleLivePointer(
    bool nativeDragActive,
    bool primaryButtonDown)
{
    // A queued move can be dispatched after the physical button was released
    // but before WM_LBUTTONUP reaches the queue head. In that interval the
    // release message remains authoritative; do not sample a later position.
    return nativeDragActive && primaryButtonDown;
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
}
