#pragma once

#include <windows.h>
#include <memory>

namespace snowdesktop::workshop_watch
{
// A worker owns this snapshot, including a separate reference to the event.
// It must never retain the DesktopApp that started it.
struct ThreadContext
{
    HANDLE stopEvent = nullptr;
    HWND notifyWindow = nullptr;

    ~ThreadContext() { if (stopEvent) CloseHandle(stopEvent); }
    ThreadContext() = default;
    ThreadContext(const ThreadContext&) = delete;
    ThreadContext& operator=(const ThreadContext&) = delete;
};

struct ThreadHandles
{
    HANDLE thread = nullptr;
    HANDLE stopEvent = nullptr;
};

// The entry point takes ownership of ThreadContext even if it starts after
// StopThread. Failure leaves no handles or context for the caller to retire.
inline ThreadHandles StartThread(HWND notifyWindow, LPTHREAD_START_ROUTINE entry)
{
    auto context = std::make_unique<ThreadContext>();
    const HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) return {};
    if (!DuplicateHandle(GetCurrentProcess(), stopEvent, GetCurrentProcess(),
            &context->stopEvent, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        CloseHandle(stopEvent);
        return {};
    }
    context->notifyWindow = notifyWindow;
    const HANDLE thread = CreateThread(nullptr, 0, entry, context.get(), 0, nullptr);
    if (!thread)
    {
        CloseHandle(stopEvent);
        return {};
    }
    context.release();
    return {thread, stopEvent};
}

inline void StopThread(HANDLE thread, HANDLE stopEvent)
{
    if (stopEvent) SetEvent(stopEvent);
    // Directory opening, discovery, or cancellation can block in the OS. Only
    // the self-contained worker waits for its I/O and keeps OVERLAPPED storage
    // alive; the UI thread releases its own references without joining it.
    if (thread) CloseHandle(thread);
    if (stopEvent) CloseHandle(stopEvent);
}
}
