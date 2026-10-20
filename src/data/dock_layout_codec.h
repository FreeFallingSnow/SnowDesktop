#pragma once

#include "common/types.h"
#include "data/layout_storage.h"
#include <algorithm>
#include <ostream>

namespace snowdesktop::dock_layout_codec
{
template<class Decode, class Normalize>
inline DockEntry Restore(const layout_storage::DockRecord& saved,
    const std::vector<DesktopWidget>& widgets, Decode decode, Normalize normalize)
{
    DockEntry entry;
    if (saved.type == "collection")
        entry.type = DockEntryType::Collection;
    else if (saved.type == "folderMapping")
        entry.type = DockEntryType::FolderMapping;
    else
        entry.type = DockEntryType::DesktopItem;
    entry.reference = decode(saved.reference);
    // Preserve the existing on-disk "collection" reference token. The
    // referenced widget supplies its precise runtime type, including groups.
    if (entry.type == DockEntryType::Collection)
    {
        const auto widget = std::find_if(widgets.begin(), widgets.end(),
            [&](const auto& value) { return value.id == entry.reference; });
        if (widget != widgets.end() && DockEntryTypeForWidget(widget->type) != DockEntryType::DesktopItem)
            entry.type = DockEntryTypeForWidget(widget->type);
    }
    if (entry.type == DockEntryType::DesktopItem)
        entry.reference = normalize(entry.reference);
    entry.keepOnDesktop = saved.keepOnDesktop;
    entry.folderSortMode =
        snowdesktop::folder_sort_rules::NormalizeMode(
            saved.folderSortMode);
    entry.folderSortAscending = saved.folderSortAscending;
    entry.folderItemKeys.reserve(saved.folderItems.size());
    for (const auto& key : saved.folderItems)
        entry.folderItemKeys.push_back(decode(key));
    entry.listMode = saved.listMode;
    entry.listColumns = std::clamp(saved.listColumns, 1, 4);
    entry.fanPopup = saved.fanPopup;
    entry.showSearchBox = saved.showSearchBox;
    entry.showFileCategories = saved.showFileCategories;
    for (const auto& id : saved.categoryTabOrder) entry.categoryTabOrder.push_back(decode(id));
    entry.detailShowModified = saved.detailShowModified;
    entry.detailShowType = saved.detailShowType;
    entry.detailShowSize = saved.detailShowSize;
    const auto positions = snowdesktop::list_detail_rules::
        NormalizePositions(
            entry.detailShowModified,
            entry.detailShowType,
            entry.detailShowSize,
            {
                saved.detailModifiedPosition.value_or(
                    snowdesktop::list_detail_rules::
                        kDefaultModifiedPosition),
                saved.detailTypePosition.value_or(
                    snowdesktop::list_detail_rules::
                        kDefaultTypePosition),
                saved.detailSizePosition.value_or(
                    snowdesktop::list_detail_rules::
                        kDefaultSizePosition),
            });
    entry.detailModifiedPosition = positions.modified;
    entry.detailTypePosition = positions.type;
    entry.detailSizePosition = positions.size;
    entry.listColumns = snowdesktop::list_detail_rules::ListColumns(entry);
    return entry;
}

template<class Escape>
inline void Write(std::ostream& file, const DockEntry& entry, Escape escape)
{
    file << "    { \"type\": \""
         << (IsLogicalDockEntryType(entry.type)
                ? "collection"
                : (entry.type == DockEntryType::FolderMapping
                    ? "folderMapping" : "item"))
         << "\", \"ref\": \"" << escape(entry.reference)
         << "\", \"keepOnDesktop\": " << (entry.keepOnDesktop ? "true" : "false")
         << ", \"folderSortMode\": "
         << snowdesktop::folder_sort_rules::
                NormalizeMode(
                    entry.folderSortMode)
         << ", \"folderSortAscending\": "
         << (entry.folderSortAscending
                ? "true" : "false")
         << ", \"listMode\": "
         << (entry.listMode ? "true" : "false")
         << ", \"listColumns\": " << snowdesktop::list_detail_rules::ListColumns(entry)
         << ", \"fanPopup\": " << (entry.fanPopup ? "true" : "false")
         << ", \"showSearchBox\": " << (entry.showSearchBox ? "true" : "false")
         << ", \"showFileCategories\": " << (entry.showFileCategories ? "true" : "false")
         << ", \"detailShowModified\": "
         << (entry.detailShowModified ? "true" : "false")
         << ", \"detailShowType\": "
         << (entry.detailShowType ? "true" : "false")
         << ", \"detailShowSize\": "
         << (entry.detailShowSize ? "true" : "false")
         << ", \"detailModifiedPosition\": "
         << entry.detailModifiedPosition
         << ", \"detailTypePosition\": "
         << entry.detailTypePosition
         << ", \"detailSizePosition\": "
         << entry.detailSizePosition
         << ", \"categoryTabOrder\": [";
    for (size_t j = 0; j < entry.categoryTabOrder.size(); ++j)
    {
        if (j) file << ", ";
        file << "\"" << escape(entry.categoryTabOrder[j]) << "\"";
    }
    file << "], \"folderItems\": [";
    for (size_t j = 0;
        j < entry.folderItemKeys.size(); ++j)
    {
        file << "\""
             << escape(
                    entry.folderItemKeys[j])
             << "\"";
        if (j + 1 !=
            entry.folderItemKeys.size())
            file << ", ";
    }
    file << "] }";
}
}
