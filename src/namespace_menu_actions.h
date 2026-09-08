#pragma once
#include <windows.h>
#include <shlobj.h>
#include <optional>
#include <string>
#include <string_view>

namespace snowdesktop::namespace_menu_actions
{
struct Command
{
    UINT offset = 0;
    std::wstring label;
    bool enabled = false;
};

inline std::optional<Command> Find(IContextMenu* context, HMENU menu, std::wstring_view verb)
{
    if (!context || !menu) return {};
    for (int i = 0; i < GetMenuItemCount(menu); ++i)
    {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_STATE | MIIM_FTYPE;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item) || (item.fType & MFT_SEPARATOR)) continue;
        if (item.hSubMenu)
        {
            if (auto found = Find(context, item.hSubMenu, verb)) return found;
            continue;
        }
        if (item.wID < 1 || item.wID > 0x7fff) continue;
        wchar_t name[256]{};
        if (FAILED(context->GetCommandString(item.wID - 1, GCS_VERBW, nullptr,
            reinterpret_cast<LPSTR>(name), static_cast<UINT>(std::size(name)))))
        {
            char ansi[256]{};
            if (FAILED(context->GetCommandString(item.wID - 1, GCS_VERBA, nullptr, ansi, static_cast<UINT>(std::size(ansi))))) continue;
            MultiByteToWideChar(CP_ACP, 0, ansi, -1, name, static_cast<int>(std::size(name)));
        }
        if (CompareStringOrdinal(name, -1, verb.data(), static_cast<int>(verb.size()), TRUE) != CSTR_EQUAL) continue;
        const int count = GetMenuStringW(menu, static_cast<UINT>(i), nullptr, 0, MF_BYPOSITION);
        std::wstring label(static_cast<size_t>(count) + 1, L'\0');
        const int copied = GetMenuStringW(menu, static_cast<UINT>(i), label.data(), static_cast<int>(label.size()), MF_BYPOSITION);
        label.resize(static_cast<size_t>(copied));
        return Command{item.wID - 1, std::move(label), (item.fState & (MFS_DISABLED | MFS_GRAYED)) == 0};
    }
    return {};
}
}
