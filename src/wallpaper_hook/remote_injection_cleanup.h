#pragma once

#include <windows.h>

namespace snow::wallpaper_hook
{

// The auxiliary process remains the allocation's owner after its caller stops
// waiting. Closing a thread handle does not cancel a remote LoadLibrary call.
template<class Wait>
bool WaitAndCompleteRemoteInjection(HANDLE process, HANDLE thread,
    void* remotePath, DWORD initialWaitMs, Wait&& waitForThread) noexcept
{
    DWORD wait = waitForThread(thread, initialWaitMs);
    if (wait != WAIT_OBJECT_0)
        wait = waitForThread(thread, INFINITE);
    DWORD module = 0;
    const bool loaded = wait == WAIT_OBJECT_0 &&
        GetExitCodeThread(thread, &module) && module != 0;
    // A failed wait is not evidence that the thread stopped using its argument.
    // If the helper is killed, this target-process allocation also stays alive.
    if (wait == WAIT_OBJECT_0 && remotePath)
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(process);
    return loaded;
}

inline bool CompleteRemoteInjection(HANDLE process, HANDLE thread,
    void* remotePath, DWORD initialWaitMs = 10000) noexcept
{
    return WaitAndCompleteRemoteInjection(process, thread, remotePath,
        initialWaitMs, [](HANDLE handle, DWORD milliseconds) {
            return WaitForSingleObject(handle, milliseconds);
        });
}

} // namespace snow::wallpaper_hook
