#pragma once

#include "common/types.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace snowdesktop::folder_mapping_visibility
{
// Identity comparison only: never resolve shortcuts, touch the filesystem or
// turn a mapped ancestor into ownership of all its desktop descendants.
inline std::wstring SourcePathKey(std::wstring path)
{
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.size() >= 8 && _wcsnicmp(path.c_str(), L"\\\\?\\UNC\\", 8) == 0)
        path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\"))
        path.erase(0, 4);
    path = std::filesystem::path(path).lexically_normal().wstring();
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

inline bool EqualInsensitive(const std::wstring& left, const std::wstring& right)
{
    return !left.empty() && !right.empty() &&
        CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

inline bool IsSourceFolder(const DesktopItem& item, const std::wstring& source)
{
    return !item.name.empty() && !item.isShortcut && item.desktopIconClsid.empty() &&
        !item.parsingName.empty() && !source.empty() &&
        EqualInsensitive(SourcePathKey(item.parsingName), SourcePathKey(source));
}

inline bool HasMappingForItem(const DesktopItem& item,
    const std::vector<DesktopWidget>& widgets, const std::wstring& excludedId = {})
{
    return std::any_of(widgets.begin(), widgets.end(), [&](const DesktopWidget& widget) {
        return widget.type == DesktopWidgetType::FolderMapping &&
            (excludedId.empty() || widget.id != excludedId) &&
            (IsSourceFolder(item, widget.sourceFolderPath) ||
                (!item.name.empty() && item.desktopIconClsid.empty() &&
                    EqualInsensitive(item.layoutKey, widget.sourceDesktopItemKey)));
    });
}

// Mapping a directory hides its free-desktop projection without moving the
// source item into a container. Preserve its return position and icon settings.
inline bool HasExplicitOwner(const DesktopItem& item,
    const std::vector<DesktopWidget>& widgets, const std::vector<DockEntry>& dock)
{
    for (const auto& widget : widgets)
    {
        if (widget.type == DesktopWidgetType::FolderMapping) continue;
        for (const auto& key : widget.itemKeys)
            if (EqualInsensitive(item.layoutKey, key)) return true;
    }
    for (const auto& entry : dock)
        if (entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop &&
            EqualInsensitive(item.layoutKey, entry.reference)) return true;
    return false;
}

template<class NormalizeKey>
void HideMappedSources(std::vector<DesktopItem>& items,
    const std::vector<DesktopWidget>& widgets,
    std::unordered_set<std::wstring>& collectedKeys, NormalizeKey normalizeKey)
{
    for (auto& item : items)
    {
        if (item.layoutKey.empty() || !HasMappingForItem(item, widgets)) continue;
        const auto key = normalizeKey(item.layoutKey);
        // Existing collection/Dock members retain their own selection. Only
        // retire the free-desktop projection and its stale selection/bounds.
        if (!collectedKeys.contains(key))
        {
            item.selected = false;
            item.bounds = {};
        }
        collectedKeys.insert(key);
    }
}

constexpr bool CanConvert(std::size_t selectedCount, bool folderTarget,
    bool isNamespace, bool supportedSurface, bool alreadyMapped) noexcept
{
    return selectedCount == 1 && folderTarget && !isNamespace &&
        supportedSurface && !alreadyMapped;
}

inline bool ReplaceDockEntry(DockEntry& entry, DesktopWidget& widget)
{
    if (entry.type != DockEntryType::DesktopItem ||
        widget.type != DesktopWidgetType::FolderMapping || widget.id.empty() ||
        !EqualInsensitive(entry.reference, widget.sourceDesktopItemKey)) return false;
    widget.gridCell = {kDockPageId, 0, 0};
    widget.folderSortMode = entry.folderSortMode;
    widget.folderSortAscending = entry.folderSortAscending;
    widget.itemKeys = entry.folderItemKeys;
    widget.listMode = entry.listMode;
    widget.fanPopup = entry.fanPopup;
    widget.showSearchBox = entry.showSearchBox;
    widget.showFileCategories = entry.showFileCategories;
    widget.categoryTabOrder = entry.categoryTabOrder;
    widget.detailShowModified = entry.detailShowModified;
    widget.detailShowType = entry.detailShowType;
    widget.detailShowSize = entry.detailShowSize;
    widget.detailModifiedPosition = entry.detailModifiedPosition;
    widget.detailTypePosition = entry.detailTypePosition;
    widget.detailSizePosition = entry.detailSizePosition;
    entry.type = DockEntryType::FolderMapping;
    entry.reference = widget.id;
    entry.keepOnDesktop = false;
    return true;
}
} // namespace snowdesktop::folder_mapping_visibility
