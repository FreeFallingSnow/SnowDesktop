#pragma once

#include "taskbar_native.h"
#include <dwmapi.h>
#include <array>
#include <span>

namespace snowdesktop::taskbar_hook
{
// Reuse the XAML connection while attaching native suppression on every owner
// thread. Ready is a one-time XAML notification, not an acknowledgement of each
// subsequent native attachment (for example after toggling suppression).
inline DWORD ConnectTaskbarThreads(HWND primary, std::span<const HWND> taskbars,
    DWORD processId, HMODULE module, HOOKPROC procedure, HANDLE ready,
    HANDLE explorer, HANDLE cancel, bool appearanceConnected,
    DWORD readyTimeout = 35000)
{
    const auto interrupted = [&]() -> DWORD {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)
            return ERROR_CANCELLED;
        if (explorer && WaitForSingleObject(explorer, 0) == WAIT_OBJECT_0)
            return ERROR_PROCESS_ABORTED;
        return ERROR_SUCCESS;
    };
    const auto attach = [&](HWND window, HHOOK& hook) -> DWORD {
        if (const DWORD error = interrupted()) return error;
        DWORD owner = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &owner);
        if (!thread || owner != processId) return ERROR_INVALID_WINDOW_HANDLE;
        hook = SetWindowsHookExW(WH_CALLWNDPROC, procedure, module, thread);
        if (!hook) return GetLastError();
        DWORD_PTR ignored = 0;
        if (!SendMessageTimeoutW(window, WM_NULL, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored))
        {
            const DWORD error = GetLastError();
            return error ? error : ERROR_TIMEOUT;
        }
        return ERROR_SUCCESS;
    };
    struct ScopedHook
    {
        HHOOK value = nullptr;
        ~ScopedHook() { if (value) UnhookWindowsHookEx(value); }
    } primaryHook;
    if (const DWORD error = attach(primary, primaryHook.value)) return error;
    // Do not gate secondary attachment on the first-ever XAML ready event.
    // The caller also supplies cached handles for temporarily reparented bars.
    for (HWND window : taskbars)
    {
        if (window == primary) continue;
        ScopedHook hook;
        if (const DWORD error = attach(window, hook.value)) return error;
    }
    if (const DWORD error = interrupted()) return error;
    if (appearanceConnected) return ERROR_SUCCESS;

    std::array<HANDLE, 3> handles{};
    DWORD count = 0;
    const DWORD cancelIndex = cancel ? count : MAXDWORD;
    if (cancel) handles[count++] = cancel;
    const DWORD explorerIndex = explorer ? count : MAXDWORD;
    if (explorer) handles[count++] = explorer;
    const DWORD readyIndex = count;
    handles[count++] = ready;
    const DWORD result = WaitForMultipleObjects(count, handles.data(), FALSE, readyTimeout);
    if (cancelIndex != MAXDWORD && result == WAIT_OBJECT_0 + cancelIndex) return ERROR_CANCELLED;
    if (explorerIndex != MAXDWORD && result == WAIT_OBJECT_0 + explorerIndex) return ERROR_PROCESS_ABORTED;
    if (result == WAIT_OBJECT_0 + readyIndex) return ERROR_SUCCESS;
    return result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
}

inline bool AreSuppressedTaskbarsControlled(const Snapshot& snapshot)
{
    if (!snapshot.enabled || !snapshot.suppressTaskbar) return false;
    bool hasTarget = false;
    for (LONG index = 0; index < snapshot.targetCount; ++index)
    {
        const auto& target = snapshot.targets[index];
        if (!target.suppressTaskbar) continue;
        hasTarget = true;
        const HWND window = reinterpret_cast<HWND>(target.taskbar);
        if (!IsWindow(window) || !GetPropW(window, native::kAttachedProperty)) return false;
        // A panel temporarily reveals a controlled taskbar without disconnecting.
        if (target.shellPanelVisible) continue;
        DWORD cloak = 0;
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloak, sizeof(cloak))) ||
            !(cloak & DWM_CLOAKED_APP)) return false;
    }
    return hasTarget;
}
}
