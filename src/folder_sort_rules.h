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

inline int NormalizeMode(int mode)
{
    return mode >= kName && mode <= kModified
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
            // Folder stacks always keep directories before files. The
            // requested direction only applies inside each group.
            if (a.isDirectory != b.isDirectory)
                return a.isDirectory;

            int comparison = 0;
            if (mode == kType)
            {
                const std::wstring_view extensionA =
                    ExtensionOf(a.name);
                const std::wstring_view extensionB =
                    ExtensionOf(b.name);
                comparison = CompareInsensitive(
                    extensionA, extensionB);
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

            if (comparison == 0)
                comparison = CompareInsensitive(
                    a.name, b.name);
            if (comparison == 0)
                comparison = CompareInsensitive(
                    a.fullPath, b.fullPath);
            return ascending
                ? comparison < 0
                : comparison > 0;
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
