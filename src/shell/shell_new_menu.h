#pragma once

#include "shell_extension_menu_items.h"

#include <functional>
#include <string>
#include <shlobj.h>
#include <wrl/client.h>

namespace snowdesktop
{
// Own the original Shell menu and COM handler until selection has been invoked.
// The background cascade uses a private range, separate from host commands and
// the extension presentation's 0x71000000 range. Shell verbs remain offsets.
class ShellNewMenu
{
public:
    static constexpr UINT FirstCommand = 0x10000;
    static constexpr UINT LastCommand = FirstCommand + 0x7ffe;

    ~ShellNewMenu() { if (root_) DestroyMenu(root_); }
    ShellNewMenu() = default;
    ShellNewMenu(const ShellNewMenu&) = delete;
    ShellNewMenu& operator=(const ShellNewMenu&) = delete;

    HRESULT Initialize(const std::wstring& directory,
        const std::function<HRESULT(IContextMenu*)>& attachSite = {})
    {
        if (root_ || context_) return E_UNEXPECTED;
        HRESULT hr = CoCreateInstance(CLSID_NewMenu, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(context_.GetAddressOf()));
        if (FAILED(hr)) return hr;
        Microsoft::WRL::ComPtr<IShellExtInit> initialize;
        hr = context_.As(&initialize);
        if (FAILED(hr)) return hr;
        PIDLIST_ABSOLUTE id = nullptr;
        hr = SHParseDisplayName(directory.c_str(), nullptr, &id, 0, nullptr);
        if (FAILED(hr)) return hr;
        hr = initialize->Initialize(id, nullptr, nullptr);
        ILFree(id);
        if (FAILED(hr)) return hr;
        if (attachSite && FAILED(hr = attachSite(context_.Get()))) return hr;
        root_ = CreatePopupMenu();
        if (!root_) return E_OUTOFMEMORY;
        hr = context_->QueryContextMenu(root_, 0, FirstCommand, LastCommand, CMF_NORMAL);
        if (FAILED(hr)) return hr;
        popup_ = GetSubMenu(root_, 0);
        if (!popup_) return E_FAIL;
        Microsoft::WRL::ComPtr<IContextMenu2> messages;
        if (SUCCEEDED(context_.As(&messages)))
            messages->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(popup_), 0);
        return S_OK;
    }

    HMENU Menu() const { return popup_; }
    IContextMenu* Context() const { return context_.Get(); }
    bool CanDraw() const { return CanDrawPopup(popup_); }
    bool Contains(UINT command) const
    {
        if (!popup_ || command < FirstCommand || command > LastCommand) return false;
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_STATE | MIIM_SUBMENU;
        return GetMenuItemInfoW(popup_, command, FALSE, &item) && !item.hSubMenu &&
            !(item.fState & (MFS_DISABLED | MFS_GRAYED));
    }
    UINT FolderCommand() const
    {
        for (int index = 0; index < GetMenuItemCount(popup_); ++index)
        {
            const UINT command = GetMenuItemID(popup_, index);
            char verb[128]{};
            if (Contains(command) && SUCCEEDED(context_->GetCommandString(
                command - FirstCommand, GCS_VERBA, nullptr, verb, sizeof(verb))) &&
                lstrcmpiA(verb, "NewFolder") == 0) return command;
        }
        return 0;
    }

private:
    static bool CanDrawPopup(HMENU menu, unsigned depth = 0)
    {
        if (depth > 8 || shell_extensions::RequiresNativePopup(menu)) return false;
        for (int index = 0; index < GetMenuItemCount(menu); ++index)
            if (const HMENU child = GetSubMenu(menu, index); child && !CanDrawPopup(child, depth + 1))
                return false;
        return true;
    }
    Microsoft::WRL::ComPtr<IContextMenu> context_;
    HMENU root_ = nullptr;
    HMENU popup_ = nullptr;
};
} // namespace snowdesktop
