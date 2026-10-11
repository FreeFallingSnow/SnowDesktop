#pragma once

#include "taskbar_native.h"
#include "taskbar_hook_lifecycle.h"
#include <dwmapi.h>
#include <array>
#include <span>

namespace snowdesktop::taskbar_hook
{
inline bool AreTaskbarThreadsConnected(std::span<const HWND> taskbars,
    DWORD processId, DWORD hostProcessId) noexcept
{
    if (taskbars.empty() || !processId || !hostProcessId) return false;
    HANDLE instance = nullptr;
    for (const HWND window : taskbars)
    {
        DWORD owner = 0;
        if (!GetWindowThreadProcessId(window, &owner) || owner != processId) return false;
        const HANDLE module = GetPropW(window, kHookOwnerProperty);
        if (!module || (instance && module != instance) ||
            reinterpret_cast<std::uintptr_t>(GetPropW(window, kHookHostProcessProperty)) != hostProcessId)
            return false;
        instance = module;
    }
    return true;
}

// WH_CALLWNDPROC installs the native owner synchronously on each taskbar
// thread. XAML may fail or never become ready; it does not own this connection.
inline DWORD ConnectTaskbarThreads(HWND primary, std::span<const HWND> taskbars,
    DWORD processId, DWORD hostProcessId, HMODULE module, HOOKPROC procedure,
    HANDLE explorer, HANDLE cancel)
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
        const std::array<HWND, 1> attached{window};
        return AreTaskbarThreadsConnected(attached, processId, hostProcessId)
            ? ERROR_SUCCESS : ERROR_DLL_INIT_FAILED;
    };
    struct ScopedHook
    {
        HHOOK value = nullptr;
        ~ScopedHook() { if (value) UnhookWindowsHookEx(value); }
    } primaryHook;
    if (const DWORD error = attach(primary, primaryHook.value)) return error;
    // The caller also supplies cached handles for temporarily reparented bars.
    for (HWND window : taskbars)
    {
        if (window == primary) continue;
        ScopedHook hook;
        if (const DWORD error = attach(window, hook.value)) return error;
    }
    if (const DWORD error = interrupted()) return error;
    return AreTaskbarThreadsConnected(taskbars, processId, hostProcessId)
        ? ERROR_SUCCESS : ERROR_DLL_INIT_FAILED;
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
