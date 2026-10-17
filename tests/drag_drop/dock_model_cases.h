#pragma once

#include "widget_displacement_cases.h"
#include "dock/dock_drop_model.h"
#include "data/dock_layout_codec.h"

namespace dock_model_cases
{
using namespace drop_placement_cases;

inline bool SameCell(const GridCell& left, const GridCell& right)
{
    return left.pageId == right.pageId && left.column == right.column && left.row == right.row;
}

struct World
{
    struct ItemState
    {
        std::wstring layoutKey, parsingName;
        GridCell gridCell;
        bool selected, isCut;
    };
    std::filesystem::path root;
    std::vector<DesktopItem> items;
    std::vector<DesktopWidget> widgets;
    std::vector<DockEntry> dock;
    bool capacity = true;
    size_t requestedCapacity = 0;

    explicit World(std::filesystem::path path, bool folders = false) : root(std::move(path))
    {
        std::filesystem::create_directories(root / L"source");
        for (size_t index = 0; index < 4; ++index)
        {
            const auto name = std::wstring(1, static_cast<wchar_t>(L'A' + index));
            const auto file = root / L"source" / (name + L".txt");
            if (folders) std::filesystem::create_directory(file);
            std::ofstream(folders ? file / L"entry.txt" : file) << Encode(name);
            DesktopItem item; item.name = name; item.parsingName = file.wstring(); item.layoutKey = Key(file.wstring());
            item.gridCell = {L"page", static_cast<int>(index), 1}; item.gridSpan = {1, 1};
            item.selected = index % 2 != 0; item.isCut = true;
            items.push_back(std::move(item));
        }
    }

    dock_drop_model::Services Services(bool folderSection = false)
    {
        dock_drop_model::Services services;
        services.normalize = Key;
        services.recycle = [](const auto& entry) { return entry.reference == kDesktopIconClsidRecycleBin; };
        services.folder = [folderSection](const auto& entry) {
            return folderSection || entry.type == DockEntryType::FolderMapping ||
                entry.type == DockEntryType::DesktopFiles || entry.type == DockEntryType::FileGroup;
        };
        services.capacity = [this](size_t count) { requestedCapacity = count; return capacity; };
        services.item = [this](const auto& key) -> DesktopItem* {
            for (auto& item : items) if (Key(item.layoutKey) == Key(key)) return &item;
            return nullptr;
        };
        return services;
    }

    std::vector<std::wstring> Order() const
    {
        std::vector<std::wstring> result;
        for (const auto& entry : dock) result.push_back(entry.reference);
        return result;
    }

    std::vector<ItemState> ItemStates() const
    {
        std::vector<ItemState> result;
        for (const auto& item : items)
            result.push_back({item.layoutKey, item.parsingName, item.gridCell, item.selected, item.isCut});
        return result;
    }

    std::string Snapshot() const
    {
        std::ostringstream stream;
        const auto escape = [](const auto& text) { return json::EscapeString(Encode(text)); };
        for (const auto& item : items)
        {
            layout_geometry_codec::WriteItem(stream, item, escape);
            stream << item.selected << item.isCut << Encode(item.parsingName);
        }
        for (const auto& widget : widgets) widget_layout_codec::Write(stream, widget, escape, Encode);
        for (const auto& entry : dock)
        {
            dock_layout_codec::Write(stream, entry, escape);
            stream << entry.selected << entry.userOrder.value_or(static_cast<size_t>(-1));
        }
        return stream.str();
    }

    void Files() const
    {
        for (const auto& item : items)
            Require(std::filesystem::exists(item.parsingName) &&
                Bytes(std::filesystem::is_directory(item.parsingName) ? std::filesystem::path(item.parsingName) / L"entry.txt" : std::filesystem::path(item.parsingName)) == Encode(item.name),
                "Dock entry operations leave original physical files and bytes unchanged");
        Require(std::distance(std::filesystem::directory_iterator(root / L"source"), std::filesystem::directory_iterator{}) == 4,
            "Dock pinning, mapping, reorder and removal do not materialize extra source files");
    }

    void SaveReload(const std::vector<std::wstring>& expected) const
    {
        const auto escape = [](const auto& text) { return json::EscapeString(Encode(text)); };
        std::ostringstream stream;
        stream << "{\"schemaVersion\":1,\"items\":[";
        for (size_t index = 0; index < items.size(); ++index)
        {
            if (index) stream << ',';
            layout_geometry_codec::WriteItem(stream, items[index], escape);
        }
        stream << "],\"widgets\":[";
        for (size_t index = 0; index < widgets.size(); ++index)
        {
            if (index) stream << ',';
            widget_layout_codec::Write(stream, widgets[index], escape, Encode);
        }
        stream << "],\"dockEntries\":[";
        const auto order = dock_folder_rules::EntriesInUserOrder(dock);
        for (size_t index = 0; index < order.size(); ++index)
        {
            if (index) stream << ',';
            dock_layout_codec::Write(stream, *order[index], escape);
        }
        stream << "]}";
        Require(layout_storage::SaveDocument(root / L"layout.json", stream.str()), "save the real Dock and desktop model atomically");
        layout_storage::Document document;
        Require(layout_storage::LoadDocument(root / L"layout.json", document).status == layout_storage::LoadStatus::LoadedPrimary,
            "reload the actual Dock model document");
        Require(document.dockEntries.size() == expected.size(), "Dock reload retains exactly the expected stable entry identities");
        for (size_t index = 0; index < expected.size(); ++index)
        {
            const auto restored = dock_layout_codec::Restore(document.dockEntries[index], widgets, Decode, Key);
            Require(restored.reference == expected[index], "explicit Dock order survives storage and reload");
            const auto current = std::find_if(dock.begin(), dock.end(), [&](const auto& entry) { return entry.reference == expected[index]; });
            Require(current != dock.end() && restored.type == current->type && restored.keepOnDesktop == current->keepOnDesktop,
                "Dock storage preserves the component type and mapping versus exclusive contract");
            Require(restored.folderSortMode == current->folderSortMode && restored.folderSortAscending == current->folderSortAscending &&
                restored.folderItemKeys == current->folderItemKeys && restored.listMode == current->listMode &&
                restored.fanPopup == current->fanPopup && restored.showSearchBox == current->showSearchBox &&
                restored.showFileCategories == current->showFileCategories && restored.categoryTabOrder == current->categoryTabOrder &&
                restored.detailShowModified == current->detailShowModified && restored.detailShowType == current->detailShowType &&
                restored.detailShowSize == current->detailShowSize && restored.detailModifiedPosition == current->detailModifiedPosition &&
                restored.detailTypePosition == current->detailTypePosition && restored.detailSizePosition == current->detailSizePosition,
                "moving a Dock entry retains all popup presentation, sorting and member-order settings after reload");
        }
        for (const auto& item : items)
        {
            const auto saved = std::find_if(document.items.begin(), document.items.end(),
                [&](const auto& value) { return Decode(value.key) == item.layoutKey; });
            Require(saved != document.items.end(), "Dock storage does not lose a desktop file identity");
            const auto restored = layout_geometry_codec::RestoreItem(*saved, Decode, [](const auto&) { return 0; });
            Require(SameCell(restored.cell, item.gridCell), "Dock operations preserve the committed desktop coordinates after reload");
        }
        for (const auto& widget : widgets)
        {
            const auto saved = std::find_if(document.widgets.begin(), document.widgets.end(),
                [&](const auto& value) { return Decode(value.id) == widget.id; });
            Require(saved != document.widgets.end(), "Dock storage does not lose a component identity");
            DesktopWidget restored;
            widget_layout_codec::RestorePlacement(*saved, restored, Decode);
            widget_layout_codec::RestoreMembers(*saved, restored, Decode, Key);
            Require(SameCell(restored.gridCell, widget.gridCell) && restored.itemKeys == widget.itemKeys &&
                restored.childWidgetIds == widget.childWidgetIds && restored.activeCategoryId == widget.activeCategoryId,
                "Dock pinning persists actual placement and logical ownership together");
        }
    }
};

inline void Run(const std::filesystem::path& root)
{
    size_t serial = 0;
    // U07 ordinary pin and Ctrl mapping: retain the physical files in both
    // cases, with distinct desktop ownership. Source decoding is a boundary.
    for (bool keep : {false, true})
    for (int ownerKind : {0, 1, 2})
    for (size_t count : {size_t{1}, size_t{2}})
    {
        World world(root / std::to_wstring(serial++));
        if (ownerKind)
        {
            DesktopWidget owner; owner.id = L"owner";
            owner.type = ownerKind == 1 ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories;
            owner.itemKeys = {world.items[0].layoutKey, world.items[1].layoutKey, world.items[2].layoutKey};
            world.widgets.push_back(owner);
        }
        const auto beforeItems = world.ItemStates();
        const auto beforeWidgets = world.widgets;
        world.dock = {{DockEntryType::DesktopItem, L"UNCHANGED", true}};
        std::vector<DockEntry> additions;
        std::vector<std::wstring> expected{L"UNCHANGED"};
        for (size_t index = 0; index < count; ++index)
        {
            additions.push_back({DockEntryType::DesktopItem, world.items[index].layoutKey, keep});
            expected.push_back(world.items[index].layoutKey);
        }
        Require(dock_drop_model::Pin({world.dock, world.widgets}, additions, 1, world.Services()), "supported desktop pin or mapping commits");
        Require(world.Order() == expected && world.requestedCapacity == count,
            "Dock inserts each selected identity at the supplied boundary without changing existing entries");
        for (size_t index = 0; index < world.items.size(); ++index)
        {
            Require(world.items[index].layoutKey == beforeItems[index].layoutKey && world.items[index].parsingName == beforeItems[index].parsingName &&
                world.items[index].selected == beforeItems[index].selected && world.items[index].isCut == beforeItems[index].isCut,
                "Dock pinning preserves file identity and unrelated transient state");
            Require(keep || index >= count ? SameCell(world.items[index].gridCell, beforeItems[index].gridCell)
                    : world.items[index].gridCell.pageId == kDockPageId,
                "ordinary Dock pin moves only selected desktop entries; Ctrl mapping keeps their original positions");
        }
        if (ownerKind)
        {
            auto expectedMembers = beforeWidgets[0].itemKeys;
            if (!keep) expectedMembers.erase(expectedMembers.begin(), expectedMembers.begin() + static_cast<std::ptrdiff_t>(count));
            Require(world.widgets[0].itemKeys == expectedMembers && world.widgets[0].id == L"owner",
                "ordinary pin revokes selected logical membership while Ctrl mapping retains the original owner");
            Require(keep || world.widgets[0].missingItems.empty(), "ordinary pin revokes deferred ownership as well as live ownership");
        }
        for (size_t index = 1; index < world.dock.size(); ++index)
            Require(world.dock[index].keepOnDesktop == keep, "each actual Dock entry has the requested mapping or exclusive policy");
        world.Files(); world.SaveReload(expected);
    }

    const std::array<DesktopWidgetType, 5> types{DesktopWidgetType::Collection, DesktopWidgetType::FileCategories,
        DesktopWidgetType::FolderMapping, DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup};
    for (auto type : types)
    for (bool icon : {false, true})
    {
        World world(root / std::to_wstring(serial++));
        DesktopWidget child; child.id = L"component"; child.type = type; child.title = L"custom title";
        child.gridCell = {L"page", 2, 2}; child.gridSpan = {1, 1}; child.singleCellIconMode = icon;
        const bool groupType = IsGroupWidgetType(type);
        if (groupType) child.childWidgetIds = {L"nested-one", L"nested-two"};
        else child.itemKeys = {world.items[0].layoutKey, world.items[1].layoutKey};
        child.sourceFolderPath = (world.root / L"source").wstring(); child.privacyMode = true;
        child.listMode = true; child.selected = true; if (groupType) child.activeCategoryId = L"nested-two";
        world.widgets.push_back(child);
        const bool grouped = type == DesktopWidgetType::Collection || type == DesktopWidgetType::FileCategories || type == DesktopWidgetType::FolderMapping;
        if (grouped)
        {
            DesktopWidget parent; parent.id = L"parent";
            parent.type = type == DesktopWidgetType::Collection ? DesktopWidgetType::CollectionGroup : DesktopWidgetType::FileGroup;
            parent.childWidgetIds = {L"before", child.id, L"after"}; parent.activeCategoryId = child.id;
            world.widgets.push_back(parent);
            for (const auto& id : {L"before", L"after"})
            {
                DesktopWidget sibling; sibling.id = id; sibling.type = type;
                world.widgets.push_back(sibling);
            }
        }
        if (groupType)
        {
            for (size_t index = 0; index < 2; ++index)
            {
                DesktopWidget nested; nested.id = child.childWidgetIds[index];
                nested.type = type == DesktopWidgetType::CollectionGroup ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories;
                nested.itemKeys = {world.items[index].layoutKey}; world.widgets.push_back(nested);
            }
        }
        const auto dockType = DockEntryTypeForWidget(type);
        Require(dock_drop_model::Pin({world.dock, world.widgets}, {{dockType, child.id, false}}, 0, world.Services()),
            "supported component pin commits its existing identity");
        const auto& pinned = world.widgets[0];
        Require(world.Order() == std::vector<std::wstring>{child.id} && world.dock[0].type == dockType && !world.dock[0].keepOnDesktop &&
            pinned.gridCell.pageId == kDockPageId && pinned.id == child.id && pinned.type == child.type && pinned.title == child.title &&
            pinned.gridSpan.columns == 1 && pinned.gridSpan.rows == 1 && pinned.singleCellIconMode == icon &&
            pinned.itemKeys == child.itemKeys && pinned.childWidgetIds == child.childWidgetIds && pinned.activeCategoryId == child.activeCategoryId &&
            pinned.sourceFolderPath == child.sourceFolderPath && pinned.privacyMode && pinned.listMode && pinned.selected,
            "component Dock pin preserves the original component, contents, configuration and footprint");
        if (grouped) Require(world.widgets[1].childWidgetIds == std::vector<std::wstring>{L"before", L"after"},
            "pinning a grouped child removes its old group membership and preserves the remaining sibling order");
        world.Files(); world.SaveReload({child.id});
    }

    for (bool folders : {false, true})
    {
        World world(root / std::to_wstring(serial++), folders);
        for (const auto& item : world.items)
        {
            DockEntry entry{DockEntryType::DesktopItem, item.layoutKey, true};
            entry.folderSortMode = folder_sort_rules::kManual; entry.folderSortAscending = false;
            entry.folderItemKeys = {L"child-B", L"child-A"}; entry.listMode = true; entry.fanPopup = true;
            entry.showSearchBox = true; entry.showFileCategories = true; entry.categoryTabOrder = {L"documents", L"all"};
            entry.detailShowModified = true; entry.detailShowType = true; entry.detailShowSize = true;
            world.dock.push_back(std::move(entry));
        }
        const auto a = world.items[0].layoutKey, b = world.items[1].layoutKey, c = world.items[2].layoutKey, d = world.items[3].layoutKey;
        const auto before = world.ItemStates();
        Require(dock_drop_model::Reorder(world.dock, {1}, 3, world.Services(folders)) == dock_drop_model::ReorderResult::Changed &&
            world.Order() == std::vector<std::wstring>{a, c, b, d}, "Dock B before D adjusts the original insertion boundary after removing B");
        world.SaveReload({a, c, b, d});
        Require(dock_drop_model::Reorder(world.dock, {2}, 1, world.Services(folders)) == dock_drop_model::ReorderResult::Changed &&
            world.Order() == std::vector<std::wstring>{a, b, c, d}, "Dock single-entry reorder can return to its original boundary");
        Require(dock_drop_model::Reorder(world.dock, {3, 1}, 0, world.Services(folders)) == dock_drop_model::ReorderResult::Changed &&
            world.Order() == std::vector<std::wstring>{b, d, a, c}, "Dock B,D before A produces B,D,A,C and preserves unmoved relative order");
        world.SaveReload({b, d, a, c});
        Require(dock_drop_model::Reorder(world.dock, {0, 1}, 4, world.Services(folders)) == dock_drop_model::ReorderResult::Changed &&
            world.Order() == std::vector<std::wstring>{a, c, b, d}, "Dock multi-selection at the trailing boundary preserves the moved block order");
        world.SaveReload({a, c, b, d});
        Require(dock_drop_model::RemoveMappings(world.dock, {3, 2, 3}, world.Services(folders)) &&
            world.Order() == std::vector<std::wstring>{a, c}, "dragging mapped entries out removes only their Dock references");
        for (size_t index = 0; index < before.size(); ++index)
            Require(SameCell(world.items[index].gridCell, before[index].gridCell), "Dock reorder and mapping removal preserve every desktop entry position");
        world.Files(); world.SaveReload({a, c});
    }

    {
        World world(root / std::to_wstring(serial++));
        DesktopWidget owner; owner.id = L"temporarily-missing"; owner.type = DesktopWidgetType::Collection;
        owner.itemKeys = {world.items[2].layoutKey};
        owner.missingItems = {{world.items[0].layoutKey, 0, 42}, {world.items[1].layoutKey, 1, 42}};
        world.widgets.push_back(owner);
        Require(dock_drop_model::Pin({world.dock, world.widgets},
            {{DockEntryType::DesktopItem, world.items[0].layoutKey, false}, {DockEntryType::DesktopItem, world.items[1].layoutKey, false}}, 0, world.Services()) &&
            world.widgets[0].missingItems.empty() && world.widgets[0].itemKeys == std::vector<std::wstring>{world.items[2].layoutKey},
            "explicit Dock pin revokes a stale owner's deferred membership without consuming unrelated members");
        world.Files(); world.SaveReload({world.items[0].layoutKey, world.items[1].layoutKey});
    }
    {
        World world(root / std::to_wstring(serial++));
        world.dock = {{DockEntryType::DesktopItem, world.items[0].layoutKey, false}};
        const auto before = world.Snapshot(); world.capacity = false;
        Require(!dock_drop_model::Pin({world.dock, world.widgets}, {{DockEntryType::DesktopItem, world.items[1].layoutKey, false}}, 0, world.Services()) &&
            world.Snapshot() == before, "capacity rejection leaves Dock entries, logical ownership and desktop positions unchanged");
        Require(!dock_drop_model::AddMaterialized({world.dock, world.widgets}, {world.items[1].parsingName}, 0, false, world.Services()) &&
            world.Snapshot() == before, "materialized Dock capacity rejection has no partial model commit");
        Require(!dock_drop_model::RemoveMappings(world.dock, {0, 99}, world.Services()) && world.Snapshot() == before,
            "mapping-only drag-out removal cannot consume an exclusive Dock entry");
        world.Files();
    }
    std::cout << "Dock pin, mapping, same-section reorder, removal and actual layout storage cases passed\n";
}
}
