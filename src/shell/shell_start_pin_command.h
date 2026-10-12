#pragma once

#include "shell_start_pin_protocol.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <optional>

namespace snowdesktop::shell_start_pin
{
inline std::optional<Action> CommandAction(IContextMenu* menu, UINT offset)
{
    if (!menu) return {};
    wchar_t verb[128]{};
    if (FAILED(menu->GetCommandString(offset, GCS_VERBW, nullptr,
            reinterpret_cast<LPSTR>(verb), static_cast<UINT>(std::size(verb)))))
    {
        char ansi[128]{};
        if (FAILED(menu->GetCommandString(offset, GCS_VERBA, nullptr,
                ansi, static_cast<UINT>(std::size(ansi)))))
            return {};
        ansi[std::size(ansi) - 1] = '\0';
        if (!MultiByteToWideChar(CP_ACP, 0, ansi, -1, verb,
                static_cast<int>(std::size(verb))))
            return {};
    }
    verb[std::size(verb) - 1] = L'\0';
    if (_wcsicmp(verb, L"PinToStartScreen") == 0) return Action::Pin;
    if (_wcsicmp(verb, L"UnpinFromStartScreen") == 0) return Action::Unpin;
    return {};
}

inline std::optional<UINT> FindCommand(IContextMenu* context, HMENU menu,
    Action action)
{
    for (int i = 0; menu && i < GetMenuItemCount(menu); ++i)
    {
        const UINT id = GetMenuItemID(menu, i);
        if (!id || id == static_cast<UINT>(-1)) continue;
        const auto candidate = CommandAction(context, id - 1);
        if (candidate && *candidate == action &&
            (GetMenuState(menu, id, MF_BYCOMMAND) &
                (MF_DISABLED | MF_GRAYED)) == 0)
            return id - 1;
    }
    return {};
}

inline HRESULT InvokeInExplorer(const Request& request)
{
    using Microsoft::WRL::ComPtr;
    PIDLIST_ABSOLUTE rawPidl = nullptr;
    HRESULT result = SHParseDisplayName(request.path, nullptr,
        &rawPidl, 0, nullptr);
    if (FAILED(result)) return result;
    struct PidlScope
    {
        PIDLIST_ABSOLUTE value;
        ~PidlScope() { ILFree(value); }
    } pidl{rawPidl};
    ComPtr<IShellFolder> folder;
    PCUITEMID_CHILD child = nullptr;
    result = SHBindToParent(pidl.value, IID_PPV_ARGS(&folder), &child);
    if (FAILED(result)) return result;
    ComPtr<IDataObject> selection;
    result = folder->GetUIObjectOf(request.owner, 1, &child,
        __uuidof(IDataObject), nullptr,
        reinterpret_cast<void**>(selection.GetAddressOf()));
    if (FAILED(result)) return result;

    // Instantiate only Microsoft's Start handler. Rebuilding the aggregate
    // here would load unrelated third-party extensions into Explorer.
    constexpr CLSID handler{0x470c0ebd, 0x5d73, 0x4d58,
        {0x9c, 0xed, 0xe9, 0x1e, 0x22, 0xe2, 0x32, 0x82}};
    ComPtr<IContextMenu> context;
    result = CoCreateInstance(handler, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&context));
    if (FAILED(result)) return result;
    ComPtr<IShellExtInit> initializer;
    result = context.As(&initializer);
    if (FAILED(result)) return result;
    PidlScope parent{ILCloneFull(pidl.value)};
    if (!parent.value) return E_OUTOFMEMORY;
    ILRemoveLastID(parent.value);
    result = initializer->Initialize(parent.value, selection.Get(), nullptr);
    if (FAILED(result)) return result;
    struct MenuScope
    {
        HMENU value = CreatePopupMenu();
        ~MenuScope() { if (value) DestroyMenu(value); }
    } menu;
    if (!menu.value) return E_OUTOFMEMORY;
    result = context->QueryContextMenu(menu.value, 0, 1, 0x7fff, CMF_NORMAL);
    if (FAILED(result)) return result;
    const auto offset = FindCommand(context.Get(), menu.value, request.action);
    // A pin becoming an unpin while the original menu is open must not
    // accidentally undo the user's already completed pin (or vice versa).
    if (!offset) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE |
        CMIC_MASK_FLAG_LOG_USAGE | CMIC_MASK_NOASYNC;
    invoke.hwnd = request.owner;
    invoke.lpVerb = MAKEINTRESOURCEA(*offset);
    invoke.lpVerbW = MAKEINTRESOURCEW(*offset);
    invoke.nShow = SW_SHOWNORMAL;
    invoke.ptInvoke = request.point;
    return context->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
}

inline void Receive(DWORD processId, std::uint64_t token)
{
    const auto name = MappingName(processId, token);
    const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (!mapping) return;
    auto* state = static_cast<Request*>(MapViewOfFile(mapping,
        FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Request)));
    if (!state) { CloseHandle(mapping); return; }
    const Request request = *state;
    DWORD ownerProcess = 0;
    GetWindowThreadProcessId(request.owner, &ownerProcess);
    if (ValidRequest(request, processId, token) && ownerProcess == processId &&
        InterlockedCompareExchange(&state->status, Running, Pending) == Pending)
    {
        HRESULT result = E_UNEXPECTED;
        try { result = InvokeInExplorer(request); }
        catch (...) { result = E_UNEXPECTED; }
        state->result = result;
        MemoryBarrier();
        InterlockedExchange(&state->status, Completed);
    }
    UnmapViewOfFile(state);
    CloseHandle(mapping);
}
} // namespace snowdesktop::shell_start_pin
