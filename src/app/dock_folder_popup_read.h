#pragma once

#include "shell_refresh_snapshot.h"
#include "../popup_animation_rules.h"

namespace snowdesktop::dock_folder_popup_read
{
inline bool HasFanContent(bool folderMapping, std::size_t count)
{
    // Folder loading/error/empty states have their own fan status label.
    return folderMapping || count != 0;
}

// The loading hint is not the entries' opening animation. When the first real
// listing changes the presentation, reveal it even if the hint finished long ago.
// Closing or hidden popups must never be reopened by a late directory result.
inline bool RevealFirstEntries(popup_animation_rules::State& animation,
    bool presentationChanged, std::size_t previousCount, std::size_t count, bool available,
    bool animationsEnabled, std::uint64_t now)
{
    if (!presentationChanged || previousCount != 0 || count == 0 || !available ||
        !animationsEnabled || !animation.IsInteractive())
        return false;
    animation.ResetHidden();
    animation.Open(now);
    return true;
}

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
