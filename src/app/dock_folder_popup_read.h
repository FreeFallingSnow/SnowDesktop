#pragma once

#include "shell_refresh_snapshot.h"

namespace snowdesktop::dock_folder_popup_read
{
// Availability comes from the directory read, not the independently queued
// Shell target classification. A pending classification must not gate the read.
template<class QueueRead, class ApplyEntries>
bool Refresh(const std::wstring& path,
    const shell_refresh::FolderSnapshot* snapshot,
    bool& available, bool& loading, QueueRead queueRead, ApplyEntries applyEntries)
{
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
