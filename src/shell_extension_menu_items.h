#pragma once
#include <windows.h>
#include <iterator>
#include <string_view>

namespace snowdesktop::shell_extensions
{
// Lazy Shell cascades can contain a dummy command with an ID but no text.
// Inspect the native popup before assigning tokens, otherwise the dummy looks
// executable and becomes an extra ellipsis row in the custom menu.
inline bool RequiresNativePopup(HMENU menu)
{
    bool hasItem = false;
    for (int index = 0; index < GetMenuItemCount(menu); ++index)
    {
        wchar_t label[2048]{};
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_STRING | MIIM_FTYPE;
        item.dwTypeData = label;
        item.cch = static_cast<UINT>(std::size(label));
        if (!GetMenuItemInfoW(menu, index, TRUE, &item))
            return true;
        if (item.fType & MFT_SEPARATOR)
            continue;
        hasItem = true;
        const std::wstring_view text(label);
        if ((item.fType & MFT_OWNERDRAW) || text.empty() || text == L"…" || text == L"...")
            return true;
    }
    return !hasItem;
}
} // namespace snowdesktop::shell_extensions
