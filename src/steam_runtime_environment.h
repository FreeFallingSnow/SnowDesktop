#pragma once

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop
{
namespace detail
{
inline bool EnvironmentEntryHasName(std::wstring_view entry,
    std::wstring_view name)
{
    return entry.size() > name.size() && entry[name.size()] == L'=' &&
        CompareStringOrdinal(entry.data(), static_cast<int>(name.size()),
            name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
}

inline bool EnvironmentEntryNameStartsWith(std::wstring_view entry,
    std::wstring_view prefix)
{
    const std::size_t delimiter = entry.find(L'=');
    return delimiter != std::wstring_view::npos &&
        delimiter >= prefix.size() &&
        CompareStringOrdinal(entry.data(), static_cast<int>(prefix.size()),
            prefix.data(), static_cast<int>(prefix.size()), TRUE) ==
            CSTR_EQUAL;
}

inline bool EnvironmentEntryLess(const std::wstring& left,
    const std::wstring& right)
{
    const int comparison = CompareStringOrdinal(left.data(),
        static_cast<int>(left.size()), right.data(),
        static_cast<int>(right.size()), TRUE);
    if (comparison == CSTR_EQUAL) return left < right;
    return comparison == CSTR_LESS_THAN;
}

inline std::vector<std::wstring> ReadCurrentEnvironmentEntries()
{
    LPWCH source = GetEnvironmentStringsW();
    if (!source) return {};

    std::vector<std::wstring> entries;
    for (const wchar_t* current = source; *current != L'\0';
         current += std::wcslen(current) + 1)
        entries.emplace_back(current);
    FreeEnvironmentStringsW(source);
    return entries;
}

inline std::vector<wchar_t> BuildUnicodeEnvironmentBlock(
    std::vector<std::wstring> entries)
{
    std::sort(entries.begin(), entries.end(), EnvironmentEntryLess);

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

inline std::vector<wchar_t> BuildSnowDesktopDetachedRuntimeEnvironment()
{
    std::vector<std::wstring> entries =
        detail::ReadCurrentEnvironmentEntries();
    if (entries.empty()) return {};
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [](const std::wstring& entry)
        {
            return detail::EnvironmentEntryNameStartsWith(entry, L"Steam");
        }), entries.end());
    return detail::BuildUnicodeEnvironmentBlock(std::move(entries));
}
}
