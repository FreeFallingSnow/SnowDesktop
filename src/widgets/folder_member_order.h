#pragma once

#include "common/types.h"
#include "layout/folder_sort_rules.h"
#include <iterator>
#include <unordered_map>

namespace snowdesktop::folder_member_order
{
// This transaction changes the view order; physical files and their identity
// stay in the mapped folder. Date grouping disables manual member reorder.
inline bool Reorder(DesktopWidget& widget, const std::vector<size_t>& indices, size_t insertBefore)
{
    if (widget.dateHeaders) return false;
    auto selected = indices;
    std::erase_if(selected, [&](size_t index) { return index >= widget.folderEntries.size(); });
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    if (selected.empty()) return false;
    insertBefore = std::min(insertBefore, widget.folderEntries.size());
    const auto removedBefore = static_cast<size_t>(std::count_if(selected.begin(), selected.end(),
        [&](size_t index) { return index < insertBefore; }));
    std::vector<FolderEntry> moving;
    moving.reserve(selected.size());
    // FolderEntry owns icon bitmaps; copying the whole vector would clone
    // every bitmap. Preserve those resources while changing only order.
    for (auto index = selected.rbegin(); index != selected.rend(); ++index)
    {
        moving.push_back(std::move(widget.folderEntries[*index]));
        widget.folderEntries.erase(widget.folderEntries.begin() + static_cast<ptrdiff_t>(*index));
    }
    std::reverse(moving.begin(), moving.end());
    widget.folderEntries.insert(widget.folderEntries.begin() + static_cast<ptrdiff_t>(insertBefore - removedBefore),
        std::make_move_iterator(moving.begin()), std::make_move_iterator(moving.end()));
    folder_sort_rules::RewriteOrderKeys(widget.folderEntries, widget.itemKeys);
    widget.folderSortMode = folder_sort_rules::kManual;
    widget.contentSortColumn = list_detail_rules::Column::None;
    return true;
}

// Production refresh projection, used after directory enumeration and after
// restoring persisted member order. Metadata remains the actual input.
template<class NormalizeKey>
inline void Refresh(DesktopWidget& widget, NormalizeKey normalizeKey)
{
    std::sort(widget.folderEntries.begin(), widget.folderEntries.end(),
        [](const FolderEntry& a, const FolderEntry& b) {
            if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
            return _wcsicmp(a.DisplayName().c_str(), b.DisplayName().c_str()) < 0;
        });
    if (snowdesktop::folder_sort_rules::
            NormalizeMode(widget.folderSortMode) !=
        snowdesktop::folder_sort_rules::kManual)
    {
        snowdesktop::folder_sort_rules::StableSort(
            widget.folderEntries,
            widget.folderSortMode,
            widget.folderSortAscending);
    }
    else if (!widget.itemKeys.empty())
    {
        std::unordered_map<std::wstring, size_t> order;
        for (size_t i = 0; i < widget.itemKeys.size(); ++i)
            order[normalizeKey(widget.itemKeys[i])] = i;
        std::stable_sort(widget.folderEntries.begin(), widget.folderEntries.end(),
            [&](const FolderEntry& a, const FolderEntry& b) {
                auto ia = order.find(normalizeKey(a.fullPath));
                auto ib = order.find(normalizeKey(b.fullPath));
                bool ha = ia != order.end();
                bool hb = ib != order.end();
                if (ha != hb) return ha;
                if (ha && hb) return ia->second < ib->second;
                return false;
            });
    }
    snowdesktop::folder_sort_rules::RewriteOrderKeys(
        widget.folderEntries, widget.itemKeys);
}
}
