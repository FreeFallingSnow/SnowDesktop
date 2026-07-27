#pragma once

#include <string_view>
#include <windows.h>

namespace snowdesktop::shell_item_visibility
{

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
