#pragma once

#include "../types.h"
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace snowdesktop::shell_refresh
{
// Only value data and independently allocated PIDLs cross the worker boundary.
// These snapshots never contain a UI COM object or a rendered bitmap.
struct FolderSnapshot
{
    std::wstring path;
    bool complete = false;
    std::vector<FolderEntry> entries;
    std::unordered_map<std::wstring, Pidl> absoluteIds;
};

struct Request
{
    std::unordered_map<std::wstring, bool> iconVisibility;
    std::vector<std::wstring> folders;
    std::vector<std::wstring> dockPaths;
};

struct Snapshot
{
    bool desktopComplete = false;
    std::vector<DesktopItem> desktopItems;
    std::unordered_map<std::wstring, FolderSnapshot> folders;
    std::unordered_set<std::wstring> missingDockPaths;
    ULONGLONG readMs = 0;
};

bool ReadDesktop(const std::unordered_map<std::wstring, bool>& visibility,
    bool showHidden, std::vector<DesktopItem>& items);
FolderSnapshot ReadFolder(const std::wstring& path, bool showHidden);
bool Read(const Request& request, Snapshot& snapshot);

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
    item.slot = previous.slot;
    item.bounds = previous.bounds;
    if (item.sysIconIndex == previous.sysIconIndex)
    {
        TransferIcon(item, previous);
        if (item.fileSize != previous.fileSize ||
            !SameTime(item.modifiedTime, previous.modifiedTime))
            item.iconState = IconState::Loading;
    }
}

inline void PreserveRuntime(FolderEntry& item, FolderEntry& previous)
{
    item.selected = previous.selected;
    item.isCut = previous.isCut;
    if (item.sysIconIndex == previous.sysIconIndex)
    {
        TransferIcon(item, previous);
        if (item.fileSize != previous.fileSize ||
            CompareFileTime(&item.lastWriteTime, &previous.lastWriteTime) != 0)
            item.iconState = IconState::Loading;
    }
}
} // namespace snowdesktop::shell_refresh
