#pragma once

#include <string_view>
#include <string>
#include <windows.h>

namespace snowdesktop::shell_item_visibility
{

// After a successful user change, read visibility from the registry again.
// A cached hide override must not defeat a later Show action or OS changes.
template <typename Cache, typename Writer>
bool CommitDesktopIconVisibility(const std::wstring& clsid, bool visible,
    Cache& overrides, Writer write)
{
    if (!write(clsid, visible)) return false;
    overrides.erase(clsid);
    return true;
}

inline std::wstring_view FileNameOf(
    std::wstring_view pathOrName)
{
    while (!pathOrName.empty() &&
        (pathOrName.back() == L'\\' ||
         pathOrName.back() == L'/'))
        pathOrName.remove_suffix(1);
    const size_t separator =
        pathOrName.find_last_of(L"\\/");
    return separator == std::wstring_view::npos
        ? pathOrName
        : pathOrName.substr(separator + 1);
}

inline bool IsAlwaysHidden(
    std::wstring_view pathOrName)
{
    const std::wstring_view name =
        FileNameOf(pathOrName);
    constexpr std::wstring_view desktopIni =
        L"desktop.ini";
    return name.size() == desktopIni.size() &&
        CompareStringOrdinal(
            name.data(),
            static_cast<int>(name.size()),
            desktopIni.data(),
            static_cast<int>(
                desktopIni.size()),
            TRUE) == CSTR_EQUAL;
}

} // namespace snowdesktop::shell_item_visibility
