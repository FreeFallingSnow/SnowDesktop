#pragma once

#include "taskbar_native.h"
#include "taskbar_hook_lifecycle.h"
#include <dwmapi.h>
#include <array>
#include <span>
#include <string>

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
    const UINT connectMessage = RegisterWindowMessageW(kConnectHookMessageName);
    if (!connectMessage) return GetLastError();
    // A fresh acknowledgement is required even when another DLL from this
    // host already owns the bars (for example, rolling back to a retired DLL).
    static std::atomic<std::uintptr_t> sequence{0};
    const std::uintptr_t nonce = sequence.fetch_add(1) + 1;
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
        // Window-thread delivery is synchronous; clearing only our transient
        // acknowledgement cannot release the existing owner or native hooks.
        RemovePropW(window, kHookConnectionProperty);
        DWORD_PTR ignored = 0;
        if (!SendMessageTimeoutW(window, connectMessage, nonce, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored))
        {
            const DWORD error = GetLastError();
            return error ? error : ERROR_TIMEOUT;
        }
        const std::array<HWND, 1> attached{window};
        return AreTaskbarThreadsConnected(attached, processId, hostProcessId) &&
            reinterpret_cast<std::uintptr_t>(GetPropW(window, kHookConnectionProperty)) == nonce
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

// Retry only an unacknowledged candidate, never a cancelled/hung/dead Shell.
// The resolver supplies an immutable isolated copy; retired code stays retired.
template<typename ResolveIsolated>
DWORD ConnectTaskbarRuntime(HWND primary, std::span<const HWND> taskbars,
    DWORD processId, DWORD hostProcessId, std::wstring& selectedPath,
    HANDLE explorer, HANDLE cancel, ResolveIsolated&& resolveIsolated)
{
    const auto connect = [&](const std::wstring& path) -> DWORD {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return ERROR_CANCELLED;
        if (explorer && WaitForSingleObject(explorer, 0) == WAIT_OBJECT_0) return ERROR_PROCESS_ABORTED;
        const HMODULE module = LoadLibraryW(path.c_str());
        if (!module) return GetLastError();
        const auto procedure = reinterpret_cast<HOOKPROC>(GetProcAddress(module, "SnowDesktopTaskbarHookProc"));
        const DWORD error = procedure ? ConnectTaskbarThreads(primary, taskbars,
            processId, hostProcessId, module, procedure, explorer, cancel) : ERROR_INVALID_FUNCTION;
        FreeLibrary(module);
        return error;
    };
    const DWORD error = connect(selectedPath);
    if (error != ERROR_DLL_INIT_FAILED) return error;
    const std::wstring isolated = resolveIsolated();
    if (isolated.empty() || isolated == selectedPath) return error;
    // Keep the choice even after a failed retry: do not mint copies repeatedly.
    selectedPath = isolated;
    return connect(selectedPath);
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
