#pragma once

#include "steam_app_identity.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop
{
namespace detail
{
inline bool SteamEnvironmentEntryHasName(std::wstring_view entry,
    std::wstring_view name)
{
    return entry.size() > name.size() && entry[name.size()] == L'=' &&
        CompareStringOrdinal(entry.data(), static_cast<int>(name.size()),
            name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
}

inline bool SteamEnvironmentEntryLess(const std::wstring& left,
    const std::wstring& right)
{
    const int comparison = CompareStringOrdinal(left.data(),
        static_cast<int>(left.size()), right.data(),
        static_cast<int>(right.size()), TRUE);
    if (comparison == CSTR_EQUAL) return left < right;
    return comparison == CSTR_LESS_THAN;
}
}

inline std::vector<wchar_t> BuildSnowDesktopSteamChildEnvironment()
{
    LPWCH source = GetEnvironmentStringsW();
    if (!source) return {};

    std::vector<std::wstring> entries;
    for (const wchar_t* current = source; *current != L'\0';
         current += std::wcslen(current) + 1)
    {
        std::wstring entry(current);
        if (!detail::SteamEnvironmentEntryHasName(entry, L"SteamAppId") &&
            !detail::SteamEnvironmentEntryHasName(entry, L"SteamGameId"))
            entries.push_back(std::move(entry));
    }
    FreeEnvironmentStringsW(source);

    const std::wstring appId = std::to_wstring(kSnowDesktopSteamAppId);
    entries.push_back(L"SteamAppId=" + appId);
    entries.push_back(L"SteamGameId=" + appId);
    std::sort(entries.begin(), entries.end(),
        detail::SteamEnvironmentEntryLess);

    std::size_t characterCount = 1;
    for (const auto& entry : entries)
        characterCount += entry.size() + 1;
    std::vector<wchar_t> block;
    block.reserve(characterCount);
    for (const auto& entry : entries)
    {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}
}
