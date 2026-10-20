#pragma once

#include "shortcut_category_target.h"
#include "layout/folder_sort_rules.h"

namespace snowdesktop::item_sort_metadata
{
// A user can sort before the asynchronous shortcut icon phase completes.
// Read the saved target once, outside the comparator, without resolving it.
template<class Item>
inline void Prepare(Item& item, const std::wstring& path)
{
    if (!item.shortcutTarget.classified &&
        shortcut_application_rules::HasExtension(path, L".lnk"))
        item.shortcutTarget = category_collection_rules::ReadShortcutTarget(path);
}

template<class Entries>
inline void PrepareFolder(Entries& entries, int mode)
{
    if (mode != folder_sort_rules::kType) return;
    for (auto& entry : entries)
        if (!entry.isDirectory) Prepare(entry, entry.fullPath);
}
}
