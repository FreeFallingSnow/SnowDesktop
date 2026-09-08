#pragma once

#include "../types.h"
#include <string_view>
#include <unordered_map>

namespace snowdesktop::shell_refresh
{
// Access time is deliberately excluded: reading metadata can change it.
struct FileStamp
{
    DWORD attributes = 0;
    FILETIME created{}, modified{};
    DWORD sizeHigh = 0, sizeLow = 0;

    template<class Attributes>
    static FileStamp From(const Attributes& value)
    {
        return {value.dwFileAttributes, value.ftCreationTime,
            value.ftLastWriteTime, value.nFileSizeHigh, value.nFileSizeLow};
    }
    bool operator==(const FileStamp& other) const
    {
        return attributes == other.attributes &&
            CompareFileTime(&created, &other.created) == 0 &&
            CompareFileTime(&modified, &other.modified) == 0 &&
            sizeHigh == other.sizeHigh && sizeLow == other.sizeLow;
    }
};

struct ShellMetadata
{
    std::wstring path;
    FileStamp stamp;
    SHFILEINFOW info{}; // No HICON: queries never request SHGFI_ICON.
    Pidl absoluteId;

    ShellMetadata() = default;
    ShellMetadata(const ShellMetadata& other)
        : path(other.path), stamp(other.stamp), info(other.info),
          absoluteId(other.absoluteId.get() ? ILCloneFull(other.absoluteId.get()) : nullptr) {}
    ShellMetadata& operator=(const ShellMetadata& other)
    {
        if (this != &other)
            *this = ShellMetadata(other);
        return *this;
    }
    ShellMetadata(ShellMetadata&&) noexcept = default;
    ShellMetadata& operator=(ShellMetadata&&) noexcept = default;

    bool Matches(std::wstring_view currentPath, const FileStamp& currentStamp) const
    {
        // Preserve case-only renames even if timestamps and the folded key match.
        return path == currentPath && stamp == currentStamp;
    }
};

using MetadataMap = std::unordered_map<std::wstring, ShellMetadata>;

struct MetadataCache
{
    MetadataMap desktop;
    std::unordered_map<std::wstring, MetadataMap> folders;
    size_t hits = 0, queries = 0;

    // Keys are normalized with the same ToUpperInvariant used by the model.
    void Invalidate(const std::wstring& key, bool descendants = false)
    {
        if (key.empty()) return;
        desktop.erase(key);
        const std::wstring prefix = key.ends_with(L"\\") ? key : key + L"\\";
        for (auto folder = folders.begin(); folder != folders.end();)
        {
            if (descendants && (folder->first == key || folder->first.starts_with(prefix)))
                folder = folders.erase(folder);
            else
            {
                folder->second.erase(key);
                ++folder;
            }
        }
    }
};
}
