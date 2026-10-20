#pragma once

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace snowdesktop::folder_sort_rules
{

constexpr int kManual = -1;
constexpr int kName = 0;
constexpr int kType = 1;
constexpr int kModified = 2;
constexpr int kSize = 3;

inline int NormalizeMode(int mode)
{
    return mode >= kName && mode <= kSize
        ? mode
        : kManual;
}

inline std::uint64_t FileTimeValue(const FILETIME& value)
{
    return
        (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
        static_cast<std::uint64_t>(value.dwLowDateTime);
}

inline std::wstring_view ExtensionOf(std::wstring_view name)
{
    const size_t slash = name.find_last_of(L"\\/");
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos ||
        (slash != std::wstring_view::npos && dot < slash) ||
        dot == slash + 1)
        return {};
    return name.substr(dot);
}

inline int CompareInsensitive(
    std::wstring_view a,
    std::wstring_view b)
{
    if (a.empty() || b.empty())
    {
        if (a.empty() && b.empty()) return 0;
        return a.empty() ? -1 : 1;
    }
    const int result = CompareStringOrdinal(
        a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()),
        TRUE);
    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

enum class TypeGroup
{
    System,
    Directory,
    DirectoryShortcut,
    Other,
};

template<class Item>
inline TypeGroup TypeGroupFor(const Item& item, bool directory)
{
    if constexpr (requires { item.desktopIconClsid; })
        if (!item.desktopIconClsid.empty()) return TypeGroup::System;
    if (directory) return TypeGroup::Directory;
    if constexpr (requires { item.shortcutTarget.directory; })
        if (item.shortcutTarget.directory) return TypeGroup::DirectoryShortcut;
    return TypeGroup::Other;
}

// Direction is applied by the caller to the entire comparison, including
// category priority and name ties. Equal keys retain their original order.
inline int CompareType(TypeGroup groupA, std::wstring_view typeA, std::wstring_view nameA,
    TypeGroup groupB, std::wstring_view typeB, std::wstring_view nameB)
{
    if (groupA != groupB) return groupA < groupB ? -1 : 1;
    if (groupA == TypeGroup::Other)
    {
        if (typeA.empty() != typeB.empty()) return typeA.empty() ? 1 : -1;
        const int comparison = CompareInsensitive(typeA, typeB);
        if (comparison != 0) return comparison;
    }
    return CompareInsensitive(nameA, nameB);
}

template <typename Entry>
inline void StableSort(
    std::vector<Entry>& entries,
    int mode,
    bool ascending)
{
    mode = NormalizeMode(mode);
    if (mode == kManual) return;

    std::stable_sort(
        entries.begin(), entries.end(),
        [mode, ascending](
            const Entry& a,
            const Entry& b) {
            // Name views keep directories grouped before files.
            // Modified-time views compare every entry on the same timeline.
            if (mode == kName &&
                a.isDirectory != b.isDirectory)
                return a.isDirectory;

            int comparison = 0;
            if (mode == kType)
            {
                const auto name = [](const Entry& entry) -> std::wstring_view {
                    if constexpr (requires { entry.DisplayName(); }) return entry.DisplayName();
                    else return entry.name;
                };
                const auto type = [](const Entry& entry) -> std::wstring_view {
                    if constexpr (requires { entry.typeName; }) return entry.typeName;
                    else return ExtensionOf(entry.name);
                };
                comparison = CompareType(TypeGroupFor(a, a.isDirectory), type(a), name(a),
                    TypeGroupFor(b, b.isDirectory), type(b), name(b));
                if (comparison == 0) comparison = CompareInsensitive(a.fullPath, b.fullPath);
                return ascending ? comparison < 0 : comparison > 0;
            }
            else if (mode == kModified)
            {
                const std::uint64_t timeA =
                    FileTimeValue(a.lastWriteTime);
                const std::uint64_t timeB =
                    FileTimeValue(b.lastWriteTime);
                comparison =
                    timeA < timeB ? -1 :
                    timeA > timeB ? 1 : 0;
            }
            else if (mode == kSize)
            {
                if constexpr (requires { a.fileSize; b.fileSize; })
                {
                    const bool hasA = a.fileSize.has_value();
                    const bool hasB = b.fileSize.has_value();
                    if (hasA != hasB)
                        return hasA;
                    if (hasA)
                    {
                        comparison =
                            *a.fileSize < *b.fileSize ? -1 :
                            *a.fileSize > *b.fileSize ? 1 : 0;
                    }
                }
            }

            if (comparison != 0)
                return ascending
                    ? comparison < 0
                    : comparison > 0;
            if constexpr (requires { a.DisplayName(); b.DisplayName(); })
                comparison = CompareInsensitive(a.DisplayName(), b.DisplayName());
            else
                comparison = CompareInsensitive(a.name, b.name);
            if (comparison == 0)
                comparison = CompareInsensitive(
                    a.fullPath, b.fullPath);
            return mode == kName && !ascending
                ? comparison > 0
                : comparison < 0;
        });
}

template <typename Entry>
inline void RewriteOrderKeys(
    const std::vector<Entry>& entries,
    std::vector<std::wstring>& keys)
{
    keys.clear();
    keys.reserve(entries.size());
    for (const auto& entry : entries)
        keys.push_back(entry.fullPath);
}

} // namespace snowdesktop::folder_sort_rules
