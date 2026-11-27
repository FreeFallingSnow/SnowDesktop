#pragma once

#include "common/types.h"
#include "data/layout_storage.h"
#include "layout/folder_sort_rules.h"
#include "layout/list_detail_rules.h"
#include <algorithm>
#include <ostream>

namespace snowdesktop::dock_layout_codec
{
template<class Decode, class Normalize>
inline DockEntry Restore(const layout_storage::DockRecord& saved,
    const std::vector<DesktopWidget>& widgets, Decode decode, Normalize normalize)
{
    DockEntry entry;
    if (saved.freeOrderSection && *saved.freeOrderSection >= static_cast<int>(DockEntrySection::Fixed) &&
        *saved.freeOrderSection <= static_cast<int>(DockEntrySection::Files))
        entry.freeOrderSection = static_cast<DockEntrySection>(*saved.freeOrderSection);
    if (saved.type == "collection")
        entry.type = DockEntryType::Collection;
    else if (saved.type == "folderMapping")
        entry.type = DockEntryType::FolderMapping;
    else if (saved.type == "separator")
        entry.type = DockEntryType::Separator;
    else if (saved.type == "quickSearch")
        entry.type = DockEntryType::QuickSearch;
    else
        entry.type = DockEntryType::DesktopItem;
    entry.reference = decode(saved.reference);
    if (IsDockLayoutEntry(entry.type)) return entry;
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
    entry.dateHeaders = saved.dateHeaders;
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
    if (IsDockLayoutEntry(entry.type))
    {
        file << "    { \"type\": \"" << (entry.type == DockEntryType::Separator ? "separator" : "quickSearch")
            << "\", \"ref\": \"" << escape(entry.reference) << "\"";
        if (entry.freeOrderSection) file << ", \"freeOrderSection\": " << static_cast<int>(*entry.freeOrderSection);
        file << " }";
        return;
    }
    file << "    { \"type\": \""
         << (IsLogicalDockEntryType(entry.type)
                ? "collection"
                : (entry.type == DockEntryType::FolderMapping
                    ? "folderMapping" : "item"))
         << "\", \"ref\": \"" << escape(entry.reference)
         << "\"";
    if (entry.freeOrderSection) file << ", \"freeOrderSection\": " << static_cast<int>(*entry.freeOrderSection);
    file << ", \"keepOnDesktop\": " << (entry.keepOnDesktop ? "true" : "false")
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
         << ", \"dateHeaders\": " << (entry.dateHeaders ? "true" : "false")
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
