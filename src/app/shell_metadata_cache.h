#pragma once

#include "../types.h"
#include "../shortcut_application_rules.h"
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace snowdesktop::shell_refresh
{
// Membership-only startup labels cannot wait for Shell metadata. Hide shortcut
// suffixes immediately, while keeping parsing names and layout identities intact.
inline std::wstring LocalDesktopDisplayName(std::wstring_view path, bool directory)
{
    const auto separator = path.find_last_of(L"\\/");
    auto name = separator == std::wstring_view::npos ? path : path.substr(separator + 1);
    if (!directory && name.size() > 4 &&
        (shortcut_application_rules::HasExtension(name, L".lnk") ||
            shortcut_application_rules::HasExtension(name, L".url")))
        name.remove_suffix(4);
    return std::wstring(name);
}

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

// Publishing startup membership must not enter the system image list, even
// with SHGFI_USEFILEATTRIBUTES: that list can be busy in another Shell query.
// The callback is the Shell boundary; cache selection is shared with tests.
template<class Query>
SHFILEINFOW ReadDesktopMetadata(bool deferred, const std::wstring& path,
    const std::wstring& key, const FileStamp& stamp, bool hasAttributes,
    MetadataCache* cache, std::unordered_set<std::wstring>& seen, Query query)
{
    SHFILEINFOW info{};
    info.iIcon = -1;
    if (deferred) return info;
    if (cache && hasAttributes)
    {
        const auto found = cache->desktop.find(key);
        if (found != cache->desktop.end() && found->second.Matches(path, stamp))
        {
            ++cache->hits;
            seen.insert(key);
            return found->second.info;
        }
    }
    if (cache) ++cache->queries;
    if (query(info) && cache && hasAttributes)
    {
        auto& metadata = cache->desktop[key];
        metadata.path = path;
        metadata.stamp = stamp;
        metadata.info = info;
        seen.insert(key);
    }
    return info;
}
}
