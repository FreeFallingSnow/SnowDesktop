#pragma once
#include "../types.h"
#include <string>

namespace snowdesktop::shell_icon_request
{
inline std::wstring Stamp(const std::optional<FILETIME>& modified,
    const std::optional<std::uint64_t>& bytes)
{
    return L"\nV:" + (modified ? std::to_wstring(
        (static_cast<std::uint64_t>(modified->dwHighDateTime) << 32) |
            modified->dwLowDateTime) : L"unknown") + L":" +
        (bytes ? std::to_wstring(*bytes) : L"unknown");
}
inline std::wstring Stamp(const DesktopItem& item) { return Stamp(item.modifiedTime, item.fileSize); }
inline std::wstring Stamp(const FolderEntry& item) { return Stamp(item.lastWriteTime, item.fileSize); }
template<class Item>
bool Matches(const std::wstring& request, const Item& item) { return request.ends_with(Stamp(item)); }
}
