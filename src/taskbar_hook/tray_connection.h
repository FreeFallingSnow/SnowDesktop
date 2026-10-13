#pragma once

#include "tray_protocol.h"
#include <string>

namespace snowdesktop::tray
{
struct HookConnectionApi
{
    decltype(&SetWindowsHookExW) install = &SetWindowsHookExW;
    decltype(&UnhookWindowsHookEx) uninstall = &UnhookWindowsHookEx;
};

// The acknowledgement must belong to this new mapping/epoch. A published
// resident property alone is never sufficient to declare collection attached.
template<typename ResolvePath>
DWORD AttachCollector(HWND window, SharedState& state, HMODULE& module,
    const wchar_t*& stage, ResolvePath&& path, HookConnectionApi api = {})
{
    const auto epoch = static_cast<WPARAM>(Read(state.epoch));
    DWORD targetProcess = 0;
    const DWORD targetThread = GetWindowThreadProcessId(window, &targetProcess);
    if (!targetThread || !targetProcess) return ERROR_INVALID_WINDOW_HANDLE;
    if (reinterpret_cast<UINT_PTR>(GetPropW(window, kResidentProperty)) == kVersion)
    {
        stage = L"resumeCollector";
        DWORD_PTR acknowledgement = 0;
        const UINT message = RegisterWindowMessageW(kResumeMessage);
        if (message && SendMessageTimeoutW(window, message, state.owner,
                static_cast<LPARAM>(epoch), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &acknowledgement) &&
            acknowledgement == epoch && state.explorer == targetProcess)
            return ERROR_SUCCESS;
        // A stale/older broker can be replaced through the ordinary cold
        // registration route, which also validates the incoming IPC mapping.
    }
    stage = L"loadHook";
    module = LoadLibraryExW(path().c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) return GetLastError();
    const auto procedure = reinterpret_cast<HOOKPROC>(GetProcAddress(module, "SnowDesktopTrayHookProc"));
    if (!procedure) return ERROR_PROC_NOT_FOUND;
    stage = L"installHook";
    const HHOOK hook = api.install(WH_CALLWNDPROC, procedure, module, targetThread);
    if (!hook) return GetLastError();
    DWORD_PTR ignored = 0;
    stage = L"attachMessage";
    const UINT message = RegisterWindowMessageW(kAttachMessage);
    const bool delivered = message && SendMessageTimeoutW(window, message, state.owner, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored) != 0;
    const DWORD error = delivered ? ERROR_SUCCESS : GetLastError();
    api.uninstall(hook);
    return error ? error : (delivered ? ERROR_SUCCESS : ERROR_TIMEOUT);
}
}
