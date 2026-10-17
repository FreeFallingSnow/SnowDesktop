#pragma once

#include "common/types.h"
#include "data/layout_storage.h"
#include "layout/folder_sort_rules.h"
#include <ostream>

namespace snowdesktop::widget_layout_codec
{
template<class Decode>
inline void RestorePlacement(const layout_storage::WidgetRecord& saved,
    DesktopWidget& widget, Decode decode)
{
    widget.gridCell = {decode(saved.page), saved.column, saved.row};
    widget.gridSpan = {std::max(1, saved.width), std::max(1, saved.height)};
}

template<class Decode, class NormalizeKey>
inline void RestoreMembers(const layout_storage::WidgetRecord& saved,
    DesktopWidget& widget, Decode decode, NormalizeKey normalizeKey)
{
    widget.activeCategoryId = decode(saved.activeCategory);
    widget.itemKeys.clear();
    widget.itemKeys.reserve(saved.items.size());
    for (const auto& key : saved.items) widget.itemKeys.push_back(decode(key));
    if (widget.type != DesktopWidgetType::FolderMapping)
        for (const auto& missing : saved.missingItems)
            widget.missingItems.push_back({normalizeKey(decode(missing.key)),
                static_cast<size_t>(missing.index), missing.missingSince});
    widget.childWidgetIds.clear();
    widget.childWidgetIds.reserve(saved.childWidgets.size());
    for (const auto& child : saved.childWidgets) widget.childWidgetIds.push_back(decode(child));
}

inline void RestoreSort(const layout_storage::WidgetRecord& saved, DesktopWidget& widget)
{
    widget.folderSortMode = snowdesktop::folder_sort_rules::NormalizeMode(
        saved.folderSortMode);
    widget.folderSortAscending = saved.folderSortAscending;
    widget.contentSortColumn =
        snowdesktop::list_detail_rules::FromString(
            saved.contentSortColumn);
    widget.contentSortAscending = saved.contentSortAscending;
    if (widget.contentSortColumn ==
            snowdesktop::list_detail_rules::Column::None &&
        widget.type == DesktopWidgetType::FolderMapping &&
        widget.folderSortMode >=
            snowdesktop::folder_sort_rules::kName)
    {
        widget.contentSortColumn = snowdesktop::list_detail_rules::
            FromLegacyFolderSortMode(widget.folderSortMode);
        widget.contentSortAscending = widget.folderSortAscending;
    }
}

template<class NormalizeKey>
inline DesktopWidgetType TypeFromName(const std::wstring& type, NormalizeKey normalizeKey)
{
    std::wstring n = normalizeKey(type);
    if (n == L"FILECATEGORIES" || n == L"FILE_CATEGORIES") return DesktopWidgetType::FileCategories;
    if (n == L"FOLDERMAPPING" || n == L"FOLDER_MAPPING") return DesktopWidgetType::FolderMapping;
    if (n == L"COLLECTIONGROUP" || n == L"COLLECTION_GROUP") return DesktopWidgetType::CollectionGroup;
    if (n == L"FILEGROUP" || n == L"FILE_GROUP") return DesktopWidgetType::FileGroup;
    if (n == L"LUA" || n == L"LUASCRIPT" || n == L"LUA_SCRIPT") return DesktopWidgetType::LuaScript;
    if (n == L"GUIDE") return DesktopWidgetType::Guide;
    if (n == L"COLLECTION") return DesktopWidgetType::Collection;
    return DesktopWidgetType::Collection;
}

inline std::wstring TypeName(DesktopWidgetType type)
{
    switch (type)
    {
    case DesktopWidgetType::CollectionGroup: return L"collectionGroup";
    case DesktopWidgetType::FileGroup: return L"fileGroup";
    case DesktopWidgetType::FileCategories: return L"fileCategories";
    case DesktopWidgetType::FolderMapping: return L"folderMapping";
    case DesktopWidgetType::LuaScript: return L"lua";
    case DesktopWidgetType::Guide: return L"guide";
    case DesktopWidgetType::Collection:
    default: return L"collection";
    }
}

// The host's widget writer, including view state, stable identity, members,
// folder ordering and group children. UTF conversion stays at the caller.
template<class Escape, class Encode>
inline void Write(std::ostream& file, const DesktopWidget& w, Escape escape, Encode encode)
{
    const bool hasCustomTitle = !w.customTitle.empty();
    file << "    { \"id\": \"" << escape(w.id)
         << "\", \"type\": \"" << escape(TypeName(w.type))
         << "\", \"title\": \"" << escape(w.title)
         << "\", \"titleMode\": \"" << (hasCustomTitle ? "custom" : "auto")
         << "\", \"customTitle\": \"" << escape(w.customTitle)
         << "\", \"demoIconCategory\": \""
         << escape(w.demoIconCategory)
         << "\", \"dockIconAssetId\": \"" << w.dockIconAssetId
         << "\", \"sourceFolderPath\": \"" << escape(w.sourceFolderPath)
         << "\", \"sourceDesktopItemKey\": \"" << escape(w.sourceDesktopItemKey)
         << "\", \"packageId\": \"" << escape(w.packageId)
         << "\", \"packageSourceProvider\": \""
         << escape(w.packageSourceProvider)
         << "\", \"packageSourceExternalItemId\": \""
         << escape(w.packageSourceExternalItemId)
         << "\", \"packageSourceUrl\": \""
         << escape(w.packageSourceUrl)
         << "\", \"activeCategory\": \"" << escape(w.activeCategoryId)
         << "\", \"page\": \"" << escape(w.gridCell.pageId)
         << "\", \"x\": " << w.gridCell.column
         << ", \"y\": " << w.gridCell.row
         << ", \"w\": " << std::max(1, w.gridSpan.columns)
         << ", \"h\": " << std::max(1, w.gridSpan.rows)
         << ", \"autoCollect\": " << (w.autoCollect ? "true" : "false")
         << ", \"dissolveWhenSingle\": " << (w.dissolveWhenSingle ? "true" : "false")
         << ", \"listMode\": " << (w.listMode ? "true" : "false")
         << ", \"fanPopup\": " << (w.fanPopup ? "true" : "false")
         << ", \"showDetails\": "
         << (snowdesktop::list_detail_rules::HasMetadataColumns(
                w.detailShowModified,
                w.detailShowType,
                w.detailShowSize)
                ? "true" : "false")
         << ", \"detailShowModified\": "
         << (w.detailShowModified ? "true" : "false")
         << ", \"detailShowType\": "
         << (w.detailShowType ? "true" : "false")
         << ", \"detailShowSize\": "
         << (w.detailShowSize ? "true" : "false")
         << ", \"detailModifiedPosition\": "
         << w.detailModifiedPosition
         << ", \"detailTypePosition\": "
         << w.detailTypePosition
         << ", \"detailSizePosition\": "
         << w.detailSizePosition
         << ", \"contentSortColumn\": \""
         << snowdesktop::list_detail_rules::ToString(
                w.contentSortColumn)
         << "\", \"contentSortAscending\": "
         << (w.contentSortAscending ? "true" : "false")
         << ", \"dateHeaders\": " << (w.dateHeaders ? "true" : "false")
         << ", \"showFileCategories\": " << (w.showFileCategories ? "true" : "false")
         << ", \"showSearchBox\": " << (w.showSearchBox ? "true" : "false")
         << ", \"showOnHoverOnly\": " << (w.showOnHoverOnly ? "true" : "false")
         << ", \"privacyMode\": " << (w.privacyMode ? "true" : "false")
         << ", \"singleCellIconMode\": " << (w.singleCellIconMode ? "true" : "false")
         << ", \"scrollContainerMode\": " << (w.scrollContainerMode ? "true" : "false")
         << ", \"titleBarCollapsed\": " << (w.titleBarCollapsed ? "true" : "false")
         << ", \"titleBarExpandOnHover\": " << (w.titleBarExpandOnHover ? "true" : "false")
         << ", \"largeFolderTitleless\": "
         << (w.largeFolderTitleless ? "true" : "false")
         << ", \"keepWhenDesktopHidden\": "
         << (w.keepWhenDesktopHidden ? "true" : "false")
         << ", \"showTitle\": " << (w.showTitle ? "true" : "false")
         << ", \"bottomBarHover\": " << (w.bottomBarHover ? "true" : "false")
         << ", \"userRenamed\": " << (hasCustomTitle ? "true" : "false")
         << ", \"scrollOffset\": " << std::max(0, w.scrollOffset)
         << ", \"tabScrollOffset\": " << std::max(0, w.tabScrollOffset)
         << ", \"folderSortMode\": "
         << snowdesktop::folder_sort_rules::
                NormalizeMode(w.folderSortMode)
         << ", \"folderSortAscending\": "
         << (w.folderSortAscending ? "true" : "false")
         << ", \"autoCollectCategoryIds\": [";
    for (size_t j = 0; j < w.autoCollectCategoryIds.size(); ++j)
    {
        if (j) file << ",";
        file << "\"" << escape(w.autoCollectCategoryIds[j]) << "\"";
    }
    file << "], \"categoryTabOrder\": [";
    for (size_t j = 0; j < w.categoryTabOrder.size(); ++j)
    {
        if (j) file << ",";
        file << "\"" << escape(w.categoryTabOrder[j]) << "\"";
    }
    file << "], \"items\": [";
    for (size_t j = 0; j < w.itemKeys.size(); ++j)
    {
        file << "\"" << escape(w.itemKeys[j]) << "\"";
        if (j + 1 != w.itemKeys.size()) file << ", ";
    }
    file << "]";
    if (!w.missingItems.empty() && w.type != DesktopWidgetType::FolderMapping)
    {
        std::vector<snowdesktop::layout_storage::MissingItemRecord> missing;
        for (const auto& entry : w.missingItems)
            missing.push_back({encode(entry.key), static_cast<int>(entry.index), entry.missingSince});
        file << ", \"missingItems\": " << snowdesktop::layout_storage::SerializeMissingItems(missing);
    }
    file << ", \"childWidgets\": [";
    for (size_t j = 0; j < w.childWidgetIds.size(); ++j)
    {
        file << "\"" << escape(w.childWidgetIds[j]) << "\"";
        if (j + 1 != w.childWidgetIds.size()) file << ", ";
    }
    file << "] }";
}
}
