#pragma once

#include "../types.h"
#include "shell_metadata_cache.h"
#include "shell_folder_notifications.h"
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <functional>

namespace snowdesktop::shell_refresh
{
// Only value data and independently allocated PIDLs cross the worker boundary.
// These snapshots never contain a UI COM object or a rendered bitmap.
struct FolderSnapshot
{
    std::wstring path;
    bool complete = false;
    DWORD error = ERROR_SUCCESS;
    std::vector<FolderEntry> entries;
    std::unordered_map<std::wstring, Pidl> absoluteIds;
};

struct Request
{
    bool foldersOnly = false;
    std::unordered_map<std::wstring, bool> iconVisibility;
    std::vector<std::wstring> folders;
    std::function<void(const DesktopItem&)> publishDesktopItem;
};

struct Snapshot
{
    bool foldersOnly = false;
    bool desktopComplete = false;
    bool desktopIncremental = false;
    std::vector<DesktopItem> desktopItems;
    std::unordered_map<std::wstring, FolderSnapshot> folders;
    ULONGLONG readMs = 0;
    ULONGLONG desktopReadMs = 0, folderReadMs = 0;
    ULONGLONG modelMs = 0, layoutMs = 0, saveMs = 0, rebuildMs = 0, notifyMs = 0;
    MetadataCache metadata;
};

// The filesystem/Shell calls are the replaceable boundary. Production and
// regression tests share scope selection, deduplication and snapshot assembly.
template<class DesktopReader, class FolderReader>
bool ReadSources(const Request& request, Snapshot& snapshot,
    DesktopReader readDesktop, FolderReader readFolder)
{
    const ULONGLONG started = GetTickCount64();
    snapshot.foldersOnly = request.foldersOnly;
    snapshot.metadata.hits = snapshot.metadata.queries = 0;
    snapshot.desktopComplete = request.foldersOnly || readDesktop(request, snapshot);
    snapshot.desktopReadMs = GetTickCount64() - started;
    const ULONGLONG foldersStarted = GetTickCount64();
    for (const auto& path : request.folders)
    {
        const auto [entry, inserted] = snapshot.folders.try_emplace(FolderKey(path));
        if (inserted)
            entry->second = readFolder(path, snapshot.metadata);
    }
    if (!request.foldersOnly)
        std::erase_if(snapshot.metadata.folders, [&](const auto& entry) {
            return !snapshot.folders.contains(entry.first);
        });
    snapshot.folderReadMs = GetTickCount64() - foldersStarted;
    snapshot.readMs = GetTickCount64() - started;
    return snapshot.desktopComplete;
}

inline void SelectFolders(Request& request, const FolderRefreshScope& scope)
{
    request.foldersOnly = scope.FoldersOnly();
    if (!request.foldersOnly) return;
    std::erase_if(request.folders, [&](const auto& path) { return !scope.Includes(path); });
    request.iconVisibility.clear();
}

bool ReadDesktop(const std::unordered_map<std::wstring, bool>& visibility,
    bool showHidden, std::vector<DesktopItem>& items, MetadataCache* cache = nullptr,
    const std::function<void(const DesktopItem&)>& publish = {});
FolderSnapshot ReadFolder(const std::wstring& path, bool showHidden, MetadataCache* cache = nullptr);
bool Read(const Request& request, Snapshot& snapshot);
bool ReadLocalDesktop(const Request& request, Snapshot& snapshot, bool common);

// Only enumeration value data crosses this boundary. Never copy UI bitmaps,
// selection or layout state back from a worker.
inline DesktopItem CloneReadItem(const DesktopItem& source)
{
    DesktopItem item;
    item.name = source.name;
    item.parsingName = source.parsingName;
    item.layoutKey = source.layoutKey;
    item.desktopIconClsid = source.desktopIconClsid;
    item.typeName = source.typeName;
    item.modifiedTime = source.modifiedTime;
    item.fileSize = source.fileSize;
    item.sysIconIndex = source.sysIconIndex;
    item.absolutePidl.reset(ILCloneFull(source.absolutePidl.get()));
    item.childPidl.reset(ILCloneFull(source.childPidl.get()));
    return item;
}

inline void AppendUnobservedItems(std::vector<DesktopItem>& items,
    std::vector<DesktopItem>& previous)
{
    std::unordered_set<std::wstring> present;
    for (const auto& item : items) present.insert(item.layoutKey);
    for (auto& item : previous)
        if (present.insert(item.layoutKey).second)
            items.push_back(std::move(item));
}

// Coalesce notifications and reject a read superseded by later filesystem or
// model changes. At most one read can be queued/running at a time.
class Revision
{
public:
    void Invalidate() { ++revision_; }
    std::optional<std::uint64_t> Begin()
    {
        if (running_)
            return std::nullopt;
        running_ = true;
        active_ = revision_;
        return active_;
    }
    bool Finish(std::uint64_t revision)
    {
        if (!running_ || revision != active_)
            return false;
        running_ = false;
        return revision == revision_;
    }
    bool Running() const { return running_; }
    std::uint64_t Current() const { return revision_; }
    bool IsCurrent(std::uint64_t revision) const { return revision == revision_; }
private:
    std::uint64_t revision_ = 0;
    std::uint64_t active_ = 0;
    bool running_ = false;
};

inline bool SameTime(const std::optional<FILETIME>& a,
    const std::optional<FILETIME>& b)
{
    return a.has_value() == b.has_value() &&
        (!a || CompareFileTime(&*a, &*b) == 0);
}

template<typename Item>
inline void TransferIcon(Item& destination, Item& source)
{
    destination.iconBitmap = std::exchange(source.iconBitmap, nullptr);
    destination.iconBitmapSize = source.iconBitmapSize;
    destination.shortcutArrow = source.shortcutArrow;
    destination.isShortcut = source.isShortcut;
    destination.isApplicationShortcut = source.isApplicationShortcut;
    destination.iconIsMediaThumbnail = source.iconIsMediaThumbnail;
    destination.iconState = source.iconState;
}

inline void PreserveRuntime(DesktopItem& item, DesktopItem& previous)
{
    item.selected = previous.selected;
    item.isCut = previous.isCut;
    item.gridCell = previous.gridCell;
    item.gridSpan = previous.gridSpan;
    item.largeIcon = previous.largeIcon;
    item.slot = previous.slot;
    item.bounds = previous.bounds;
    if (item.sysIconIndex < 0 || previous.sysIconIndex < 0 ||
        item.sysIconIndex == previous.sysIconIndex)
    {
        // Membership-only snapshots carry no Shell index. They must not
        // discard an icon already delivered by the independent loader.
        if (item.sysIconIndex < 0) item.sysIconIndex = previous.sysIconIndex;
        if (item.typeName.empty()) item.typeName = previous.typeName;
        TransferIcon(item, previous);
        if (item.fileSize != previous.fileSize ||
            !SameTime(item.modifiedTime, previous.modifiedTime))
            item.iconState = IconState::Loading;
    }
}

inline void ApplyLoadedLayout(DesktopItem& item, const LayoutRecord* record)
{
    item.gridCell = {};
    item.gridSpan = {1, 1};
    item.largeIcon.reset();
    item.slot = -1;
    if (!record || !record->hasGrid) return;
    item.gridCell = record->cell;
    item.gridSpan = record->span;
    item.largeIcon = record->largeIcon;
}

inline void PreserveRuntime(FolderEntry& item, FolderEntry& previous)
{
    item.selected = previous.selected;
    item.isCut = previous.isCut;
    if (item.sysIconIndex < 0 || item.sysIconIndex == previous.sysIconIndex)
    {
        item.sysIconIndex = previous.sysIconIndex;
        if (item.typeName.empty()) item.typeName = previous.typeName;
        TransferIcon(item, previous);
        if (item.fileSize != previous.fileSize ||
            CompareFileTime(&item.lastWriteTime, &previous.lastWriteTime) != 0)
            item.iconState = IconState::Loading;
    }
}
} // namespace snowdesktop::shell_refresh
