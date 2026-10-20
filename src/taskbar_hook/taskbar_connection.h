#pragma once
#include "../platform/hook_entry_protocol.h"

#include "taskbar_native.h"
#include "taskbar_hook_lifecycle.h"
#include <dwmapi.h>
#include <array>
#include <span>
#include <string>
#include <algorithm>
#include <vector>

namespace snowdesktop::taskbar_hook
{
inline void RemoveStaleTaskbarWindows(std::vector<HWND>& taskbars, DWORD explorer) noexcept
{
    std::erase_if(taskbars, [explorer](HWND window) {
        DWORD process = 0;
        return !GetWindowThreadProcessId(window, &process) || !explorer || process != explorer;
    });
}

enum class ConnectionStage { None, Deployment, LoadModule, Export, RegisterHook, DeliverMessage, Acknowledge };
inline const wchar_t* ConnectionStageName(ConnectionStage stage) noexcept
{
    switch (stage)
    {
    case ConnectionStage::Deployment: return L"deployment";
    case ConnectionStage::LoadModule: return L"load-module";
    case ConnectionStage::Export: return L"resolve-export";
    case ConnectionStage::RegisterHook: return L"register-hook";
    case ConnectionStage::DeliverMessage: return L"deliver-message";
    case ConnectionStage::Acknowledge: return L"acknowledge";
    default: return L"none";
    }
}
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

// Rebind a compatible resident owner on its window threads. No DLL load or
// SetWindowsHookEx registration is needed; retired owners cannot acknowledge.
inline DWORD ResumeTaskbarThreads(std::span<const HWND> taskbars, DWORD processId,
    DWORD hostProcessId, HANDLE cancel)
{
    if (taskbars.empty() || !processId || !hostProcessId) return ERROR_INVALID_PARAMETER;
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return ERROR_CANCELLED;
    const UINT message = RegisterWindowMessageW(kResumeHookMessageName);
    if (!message) return GetLastError();
    static std::atomic<std::uintptr_t> sequence{0};
    const std::uintptr_t nonce = sequence.fetch_add(1) + 1;
    HANDLE instance = nullptr;
    for (HWND window : taskbars)
    {
        DWORD owner = 0;
        if (!GetWindowThreadProcessId(window, &owner) || owner != processId)
            return ERROR_INVALID_WINDOW_HANDLE;
        const HANDLE module = GetPropW(window, kHookOwnerProperty);
        if (!module || (instance && instance != module) ||
            reinterpret_cast<std::uintptr_t>(GetPropW(window, kHookResumeProtocolProperty)) != kSharedStateVersion)
            return ERROR_NOT_SUPPORTED;
        instance = module;
    }
    for (HWND window : taskbars)
    {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return ERROR_CANCELLED;
        RemovePropW(window, kHookConnectionProperty);
        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(window, message, hostProcessId, nonce,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result))
            return GetLastError() ? GetLastError() : ERROR_TIMEOUT;
        if (result != nonce || GetPropW(window, kHookOwnerProperty) != instance ||
            reinterpret_cast<std::uintptr_t>(GetPropW(window, kHookConnectionProperty)) != nonce)
            return ERROR_DLL_INIT_FAILED;
    }
    return AreTaskbarThreadsConnected(taskbars, processId, hostProcessId)
        ? ERROR_SUCCESS : ERROR_DLL_INIT_FAILED;
}

// WH_CALLWNDPROC installs the native owner synchronously on each taskbar
// thread. XAML may fail or never become ready; it does not own this connection.
inline DWORD ConnectTaskbarThreads(HWND primary, std::span<const HWND> taskbars,
    DWORD processId, DWORD hostProcessId, HMODULE module, HOOKPROC procedure,
    HANDLE explorer, HANDLE cancel, ConnectionStage* stage = nullptr)
{
    if (stage) *stage = ConnectionStage::None;
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
        const auto entryAbi = reinterpret_cast<const DWORD*>(GetProcAddress(module, "SnowDesktopHookEntryAbi"));
        const HANDLE previousEntryOwner = GetPropW(window, hook_entry::kOwnerProperty);
        const bool entry = entryAbi && *entryAbi == hook_entry::kAbi;
        if (entryAbi && *entryAbi == hook_entry::kAbi &&
            !SetPropW(window, hook_entry::kOwnerProperty, ULongToHandle(hostProcessId))) return GetLastError();
        if (stage) *stage = ConnectionStage::RegisterHook;
        // Current-process callbacks are already mapped and owned by the
        // caller/resident DLL. User32 requires no module identity for them;
        // recording a DLL path here also consumes a session-wide Hook slot.
        hook = SetWindowsHookExW(WH_CALLWNDPROC, procedure,
            processId == GetCurrentProcessId() ? nullptr : module, thread);
        if (!hook)
        {
            const DWORD error = GetLastError();
            if (entry && GetPropW(window, hook_entry::kOwnerProperty) == ULongToHandle(hostProcessId))
            {
                if (previousEntryOwner) SetPropW(window, hook_entry::kOwnerProperty, previousEntryOwner);
                else RemovePropW(window, hook_entry::kOwnerProperty);
            }
            return error;
        }
        // Window-thread delivery is synchronous; clearing only our transient
        // acknowledgement cannot release the existing owner or native hooks.
        RemovePropW(window, kHookConnectionProperty);
        DWORD_PTR ignored = 0;
        if (stage) *stage = ConnectionStage::DeliverMessage;
        if (!SendMessageTimeoutW(window, connectMessage, nonce, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored))
        {
            const DWORD error = GetLastError();
            return error ? error : ERROR_TIMEOUT;
        }
        const std::array<HWND, 1> attached{window};
        if (stage) *stage = ConnectionStage::Acknowledge;
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
    if (!AreTaskbarThreadsConnected(taskbars, processId, hostProcessId)) return ERROR_DLL_INIT_FAILED;
    if (stage) *stage = ConnectionStage::None;
    return ERROR_SUCCESS;
}

// Retry only an unacknowledged candidate, never a cancelled/hung/dead Shell.
// The resolver supplies an immutable isolated copy; retired code stays retired.
template<typename ResolveIsolated>
DWORD ConnectTaskbarRuntime(HWND primary, std::span<const HWND> taskbars,
    DWORD processId, DWORD hostProcessId, std::wstring& selectedPath,
    HANDLE explorer, HANDLE cancel, ResolveIsolated&& resolveIsolated,
    ConnectionStage* stage = nullptr)
{
    const auto connect = [&](const std::wstring& path) -> DWORD {
        if (stage) *stage = ConnectionStage::None;
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return ERROR_CANCELLED;
        if (explorer && WaitForSingleObject(explorer, 0) == WAIT_OBJECT_0) return ERROR_PROCESS_ABORTED;
        if (stage) *stage = ConnectionStage::LoadModule;
        const HMODULE module = LoadLibraryW(path.c_str());
        if (!module) return GetLastError();
        if (stage) *stage = ConnectionStage::Export;
        const auto procedure = reinterpret_cast<HOOKPROC>(GetProcAddress(module, "SnowDesktopTaskbarHookProc"));
        const DWORD error = procedure ? ConnectTaskbarThreads(primary, taskbars,
            processId, hostProcessId, module, procedure, explorer, cancel, stage) : ERROR_INVALID_FUNCTION;
        FreeLibrary(module);
        return error;
    };
    const DWORD error = connect(selectedPath);
    if (error != ERROR_DLL_INIT_FAILED) return error;
    SetLastError(ERROR_SUCCESS);
    const std::wstring isolated = resolveIsolated();
    if (isolated.empty())
    {
        const DWORD deploymentError = GetLastError();
        if (deploymentError)
        {
            if (stage) *stage = ConnectionStage::Deployment;
            return deploymentError;
        }
        return error;
    }
    // The stable entry path can remain unchanged while its implementation
    // catalog selects a fresh image. This still performs only one retry.
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
