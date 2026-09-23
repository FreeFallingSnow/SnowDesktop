#pragma once

#include <windows.h>

namespace snowdesktop
{
enum class PendingWindowMessage { None, Ready, Quit };

// Filtered cleanup/IPC drains must never consume the thread's exit request or
// treat WM_QUIT's payload as a heap-owned completion. PeekMessage ignores the
// numeric filter for WM_QUIT, including during reentrant window destruction.
inline PendingWindowMessage TakePendingWindowMessage(
    MSG& message, HWND window, UINT messageId)
{
    if (!PeekMessageW(&message, window, messageId, messageId, PM_REMOVE))
        return PendingWindowMessage::None;
    if (message.message == WM_QUIT)
    {
        PostQuitMessage(static_cast<int>(message.wParam));
        return PendingWindowMessage::Quit;
    }
    return PendingWindowMessage::Ready;
}
}
