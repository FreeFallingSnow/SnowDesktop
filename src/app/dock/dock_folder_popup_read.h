#pragma once

#include "app/shell/shell_refresh_snapshot.h"

#include <cstddef>

namespace snowdesktop::dock_folder_popup_read
{
// A direct mapping already names its directory. A changed shortcut must wait
// for a target from the current source version, even if its old path still works.
inline bool TargetPending(bool mapping, bool fresh, bool sameSourceVersion)
{
    return !mapping && !fresh && !sameSourceVersion;
}

inline bool BindTarget(const std::wstring& resolvedPath, bool pending,
    std::wstring& path, bool& available, bool& loading)
{
    const std::wstring next = pending ? std::wstring{} : resolvedPath;
    const bool changed = path != next;
    if (changed || pending) available = false;
    if (changed || pending) loading = pending || !next.empty();
    path = next;
    return changed;
}

inline bool HasFanContent(bool folderMapping, std::size_t count)
{
    // Folder loading/error/empty states have their own fan status label.
    return folderMapping || count != 0;
}

// Availability comes from the directory read, not the independently queued
// Shell target classification. A pending classification must not gate the read.
template<class QueueRead, class ApplyEntries>
bool Refresh(const std::wstring& path,
    const shell_refresh::FolderSnapshot* snapshot,
    bool& available, bool& loading, QueueRead queueRead, ApplyEntries applyEntries,
    bool targetPending = false)
{
    if (targetPending)
    {
        available = false;
        loading = true;
        return !snapshot;
    }
    if (snapshot && shell_refresh::FolderKey(snapshot->path) != shell_refresh::FolderKey(path))
        return false;
    if (!snapshot)
    {
        loading = !path.empty();
        if (loading) queueRead(path);
        else available = false;
        return true;
    }
    loading = false;
    available = snapshot->complete &&
        (snapshot->error == ERROR_SUCCESS || snapshot->error == ERROR_FILE_NOT_FOUND);
    // A missing directory has a complete, empty listing, but is unavailable.
    // Other read failures retain the last listing without enabling operations.
    if (snapshot->complete) applyEntries(*snapshot);
    return true;
}
}
