#pragma once

#include "desktop/folder_mapping_visibility_rules.h"
#include "desktop/desktop_item_reference_migration.h"

template<class Check>
void CheckFolderMappingVisibility(Check check)
{
    namespace visibility = snowdesktop::folder_mapping_visibility;
    DesktopItem folder;
    folder.name = L"Projects";
    folder.layoutKey = L"C:\\Users\\Snow\\Desktop\\Projects";
    folder.parsingName = folder.layoutKey;
    folder.gridCell = {L"page", 2, 3};
    folder.largeIcon.emplace();
    std::vector<DesktopWidget> widgets(1);
    widgets[0].id = L"mapping-a";
    widgets[0].type = DesktopWidgetType::FolderMapping;
    widgets[0].sourceFolderPath = L"c:/users/snow/Desktop/Projects/";

    check(visibility::HasMappingForItem(folder, widgets),
        "mapped desktop source is hidden across case and separator variants");
    check(!visibility::HasExplicitOwner(folder, widgets, {}),
        "mapping does not transfer ownership or clear saved source icon settings");
    check(folder.largeIcon.has_value() && folder.gridCell.column == 2 && folder.gridCell.row == 3,
        "visibility checks preserve source icon and return position");
    widgets[0].sourceFolderPath = L"\\\\?\\C:\\Users\\Snow\\Desktop\\Projects\\.\\";
    check(visibility::HasMappingForItem(folder, widgets),
        "extended and lexical path variants identify the same source folder");
    folder.parsingName = L"\\\\server\\share\\Desktop\\Projects";
    widgets[0].sourceFolderPath = L"\\\\?\\UNC\\SERVER\\share\\Desktop\\Projects\\";
    check(visibility::HasMappingForItem(folder, widgets),
        "extended UNC paths identify the same network-desktop source folder");
    folder.parsingName = folder.layoutKey;
    widgets[0].sourceFolderPath = folder.parsingName;

    folder.isShortcut = true;
    check(!visibility::HasMappingForItem(folder, widgets),
        "a shortcut to a mapped folder remains an independent desktop item");
    folder.isShortcut = false;
    folder.desktopIconClsid = L"{namespace}";
    check(!visibility::HasMappingForItem(folder, widgets),
        "system namespace icons remain visible even when their directory is mapped");
    folder.desktopIconClsid.clear();
    widgets[0].sourceFolderPath += L"\\child";
    check(!visibility::HasMappingForItem(folder, widgets),
        "mapping a child does not hide its desktop ancestor");
    widgets[0].sourceFolderPath = L"C:\\Users\\Snow\\Desktop";
    check(!visibility::HasMappingForItem(folder, widgets),
        "mapping the desktop root does not hide every desktop folder");
    widgets[0].sourceFolderPath.clear();
    check(!visibility::HasMappingForItem(folder, widgets), "an empty mapping path hides no source");
    widgets[0].sourceFolderPath = folder.parsingName;
    widgets[0].type = DesktopWidgetType::Collection;
    check(!visibility::HasMappingForItem(folder, widgets), "only folder mappings hide their sources");
    widgets[0].type = DesktopWidgetType::FolderMapping;
    widgets[0].gridCell.pageId = kDockPageId;
    check(visibility::HasMappingForItem(folder, widgets), "Dock-hosted mappings keep the source hidden");
    widgets[0].gridCell.pageId = L"other-page";
    DesktopWidget group;
    group.id = L"file-group";
    group.type = DesktopWidgetType::FileGroup;
    group.childWidgetIds = {widgets[0].id};
    widgets.push_back(group);
    check(visibility::HasMappingForItem(folder, widgets), "grouped or off-page mappings keep the source hidden");
    widgets.pop_back();
    widgets.push_back(widgets[0]);
    widgets.back().id = L"mapping-b";
    check(visibility::HasMappingForItem(folder, widgets, L"mapping-a"),
        "removing one duplicate mapping does not release the source");
    widgets.erase(widgets.begin());
    check(visibility::HasMappingForItem(folder, widgets), "remaining duplicate keeps the source hidden");
    check(!visibility::HasMappingForItem(folder, widgets, L"mapping-b"),
        "removing the final mapping releases the source");
    widgets.clear();
    check(!visibility::HasMappingForItem(folder, widgets), "the source returns when no mapping remains");

    widgets.emplace_back();
    widgets[0].itemKeys.push_back(folder.layoutKey);
    check(visibility::HasExplicitOwner(folder, widgets, {}),
        "existing collection ownership still takes precedence when a mapping is removed");
    widgets.clear();
    DockEntry dock;
    dock.type = DockEntryType::DesktopItem;
    dock.reference = folder.layoutKey;
    dock.keepOnDesktop = false;
    check(visibility::HasExplicitOwner(folder, widgets, {dock}),
        "an exclusive Dock source stays owned when its mapping is removed");
    dock.keepOnDesktop = true;
    check(!visibility::HasExplicitOwner(folder, widgets, {dock}),
        "a retained Dock shortcut does not own the free-desktop source");

    check(visibility::CanConvert(1, true, false, true, false),
        "a single real free-desktop folder offers conversion");
    check(!visibility::CanConvert(0, true, false, true, false) &&
        !visibility::CanConvert(2, true, false, true, false),
        "empty and multiple selections do not offer conversion");
    check(!visibility::CanConvert(1, false, false, true, false) &&
        !visibility::CanConvert(1, true, true, true, false) &&
        !visibility::CanConvert(1, true, false, false, false) &&
        !visibility::CanConvert(1, true, false, true, true),
        "file targets, namespace icons, unsupported surfaces and duplicate mappings reject conversion");

    std::vector<DesktopItem> items;
    folder.selected = true;
    folder.bounds = {10, 20, 50, 60};
    items.push_back(std::move(folder));
    widgets.emplace_back();
    widgets[0].id = L"source-map";
    widgets[0].type = DesktopWidgetType::FolderMapping;
    widgets[0].sourceFolderPath = items[0].parsingName;
    std::unordered_set<std::wstring> collected;
    auto normalize = [](std::wstring key) {
        CharUpperBuffW(key.data(), static_cast<DWORD>(key.size()));
        return key;
    };
    visibility::HideMappedSources(items, widgets, collected, normalize);
    check(collected.contains(normalize(items[0].layoutKey)) && !items[0].selected &&
        IsRectEmpty(&items[0].bounds),
        "production projection update removes hidden source bounds and stale selection");
    check(items[0].largeIcon.has_value() && items[0].gridCell.column == 2 && items[0].gridCell.row == 3,
        "hiding the projection preserves the source model and return position");
    collected.clear();
    collected.insert(normalize(items[0].layoutKey));
    items[0].selected = true;
    visibility::HideMappedSources(items, widgets, collected, normalize);
    check(items[0].selected, "mapping does not clear a collection or exclusive Dock member selection");
    widgets.clear();
    collected.clear();
    visibility::HideMappedSources(items, widgets, collected, normalize);
    check(collected.empty(), "rebuilding projection after final mapping removal restores the free source");

    items[0].isShortcut = true;
    items[0].parsingName = L"C:\\Users\\Snow\\Desktop\\Projects.lnk";
    items[0].layoutKey = items[0].parsingName;
    widgets.emplace_back();
    widgets[0].id = L"shortcut-map";
    widgets[0].type = DesktopWidgetType::FolderMapping;
    widgets[0].sourceFolderPath = L"D:\\Projects";
    widgets[0].sourceDesktopItemKey = items[0].layoutKey;
    widgets[0].itemKeys = {L"D:\\Projects\\notes.txt"};
    visibility::HideMappedSources(items, widgets, collected, normalize);
    check(collected.contains(normalize(items[0].layoutKey)) && !items[0].selected &&
        items[0].largeIcon.has_value(),
        "explicit shortcut conversion replaces only its source icon and retains its model");
    check(!visibility::HasExplicitOwner(items[0], widgets, {}),
        "mapped-content order is separate from shortcut conversion ownership");
    widgets[0].itemKeys.clear();
    check(visibility::HasMappingForItem(items[0], widgets),
        "empty folders and changing content order do not lose the converted shortcut identity");
    items[0].layoutKey += L"-other";
    check(!visibility::HasMappingForItem(items[0], widgets),
        "other shortcuts to the same target remain independent");
    items[0].layoutKey = widgets[0].sourceDesktopItemKey;
    std::vector<DockEntry> migratedDock;
    const auto migration = snowdesktop::desktop_item_reference_migration::MigrateReferences(
        widgets, migratedDock, items[0].layoutKey, L"renamed-link");
    check(migration.widgetReferences == 1 && widgets[0].sourceDesktopItemKey == L"renamed-link",
        "source identity migration keeps a converted shortcut hidden after rename");

    DockEntry pinned;
    pinned.reference = L"renamed-link";
    pinned.keepOnDesktop = true;
    pinned.listMode = true;
    pinned.fanPopup = true;
    pinned.showSearchBox = true;
    pinned.showFileCategories = true;
    pinned.folderSortMode = snowdesktop::folder_sort_rules::kManual;
    pinned.folderSortAscending = false;
    pinned.folderItemKeys = {L"D:\\Projects\\b.txt", L"D:\\Projects\\a.txt"};
    pinned.categoryTabOrder = {L"folders", L"all"};
    pinned.detailShowModified = true;
    pinned.detailShowSize = true;
    const auto order = pinned.folderItemKeys;
    check(visibility::ReplaceDockEntry(pinned, widgets[0]) &&
        pinned.type == DockEntryType::FolderMapping && pinned.reference == widgets[0].id &&
        !pinned.keepOnDesktop && widgets[0].gridCell.pageId == kDockPageId,
        "Dock conversion replaces the pinned entry without inserting a new desktop widget");
    check(widgets[0].listMode && widgets[0].fanPopup && widgets[0].showSearchBox &&
        widgets[0].showFileCategories && widgets[0].folderSortMode == snowdesktop::folder_sort_rules::kManual &&
        !widgets[0].folderSortAscending && widgets[0].itemKeys == order &&
        widgets[0].categoryTabOrder == pinned.categoryTabOrder && widgets[0].detailShowModified && widgets[0].detailShowSize,
        "Dock conversion carries existing popup display and sorting preferences to the mapping");
    check(!visibility::ReplaceDockEntry(pinned, widgets[0]),
        "a stale already-converted Dock entry cannot be converted again");
}
