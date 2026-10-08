#pragma once

#include "shell_start_pin_command.h"
#include <optional>
#include <vector>

namespace snowdesktop::shell_start_pin
{
inline HRESULT Invoke(const std::wstring& hookPath, Action action,
    const std::wstring& path, HWND owner, POINT point)
{
    Request request;
    request.processId = GetCurrentProcessId();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    request.token = static_cast<std::uint64_t>(counter.QuadPart);
    request.action = action;
    request.owner = owner;
    request.point = point;
    if (path.size() >= std::size(request.path) ||
        path.find(L'\0') != std::wstring::npos)
        return E_INVALIDARG;
    wcscpy_s(request.path, path.c_str());
    DWORD ownerProcess = 0;
    GetWindowThreadProcessId(owner, &ownerProcess);
    if (!ValidRequest(request, request.processId, request.token) ||
        ownerProcess != request.processId)
        return E_INVALIDARG;
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const DWORD thread = taskbar ? GetWindowThreadProcessId(taskbar, nullptr) : 0;
    if (!thread || hookPath.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    const HMODULE module = LoadLibraryW(hookPath.c_str());
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    struct ModuleScope { HMODULE value; ~ModuleScope() { FreeLibrary(value); } } moduleScope{module};
    const auto procedure = reinterpret_cast<HOOKPROC>(
        GetProcAddress(module, "SnowDesktopStartPinHookProc"));
    if (!procedure) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    const auto name = MappingName(request.processId, request.token);
    const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(Request), name.c_str());
    if (!mapping) return HRESULT_FROM_WIN32(GetLastError());
    const DWORD mappingError = GetLastError();
    struct MappingScope { HANDLE value; ~MappingScope() { CloseHandle(value); } } mappingScope{mapping};
    if (mappingError == ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    auto* state = static_cast<Request*>(MapViewOfFile(mapping,
        FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Request)));
    if (!state) return HRESULT_FROM_WIN32(GetLastError());
    struct ViewScope { void* value; ~ViewScope() { UnmapViewOfFile(value); } } viewScope{state};
    *state = request;
    MemoryBarrier();
    const HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, procedure, module, thread);
    if (!hook) return HRESULT_FROM_WIN32(GetLastError());
    struct HookScope { HHOOK value; ~HookScope() { UnhookWindowsHookEx(value); } } hookScope{hook};
    const UINT message = RegisterWindowMessageW(kMessage);
    if (!message) return HRESULT_FROM_WIN32(GetLastError());
    DWORD_PTR ignored = 0;
    // Keep the request single-shot. A timeout must never fall back to a
    // second pinning call or retry a command which may already have run.
    if (!SendMessageTimeoutW(taskbar, message, request.processId,
            static_cast<LPARAM>(request.token), SMTO_ABORTIFHUNG | SMTO_BLOCK,
            10000, &ignored))
    {
        const DWORD error = GetLastError();
        InterlockedCompareExchange(&state->status, Cancelled, Pending);
        return HRESULT_FROM_WIN32(error ? error : ERROR_TIMEOUT);
    }
    MemoryBarrier();
    return state->status == Completed ? state->result : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

template<typename Execute>
inline std::optional<HRESULT> Route(IContextMenu* context, UINT offset,
    const std::vector<std::wstring>& paths, Execute&& execute)
{
    const auto action = CommandAction(context, offset);
    if (!action) return {};
    if (paths.size() != 1 || paths.front().empty()) return E_INVALIDARG;
    return execute(*action, paths.front());
}
} // namespace snowdesktop::shell_start_pin
