#pragma once

#include "widget_displacement_cases.h"
#include "layout/page_mapping.h"

namespace page_mapping_cases
{
using namespace drop_placement_cases;

struct World : widget_displacement_cases::World
{
    std::unordered_map<std::wstring, LayoutRecord> records;
    int offset = 1;
    std::wstring last = L"target";
    std::uint64_t revision = 0;
    bool ready = true;
    int cancelled = 0, refreshed = 0, reserved = 0, icons = 0;

    World(std::filesystem::path path, size_t monitors, bool reverse) : widget_displacement_cases::World(std::move(path))
    {
        savedIds = monitors == 1 ? std::vector<std::wstring>{L"source", L"target", L"later"}
            : std::vector<std::wstring>{L"fixed", L"source", L"target", L"later"};
        columns = {{L"source", 7}, {L"target", 6}, {L"later", 9}, {L"fixed", 8}};
        rows = {{L"source", 7}, {L"target", 5}, {L"later", 4}, {L"fixed", 8}};
        if (monitors == 1) { columns.erase(L"fixed"); rows.erase(L"fixed"); }
        names = {{L"target", L"Destination"}, {L"later", L"Later"}};
        if (monitors > 1) names[L"fixed"] = L"Fixed";
        pages.resize(monitors); order.clear();
        for (size_t index = 0; index < monitors; ++index) order.push_back(index);
        if (reverse) std::reverse(order.begin(), order.end());
        for (size_t index = 0; index < monitors; ++index)
        {
            pages[index].monitorId = L"monitor-" + std::to_wstring(index);
            pages[index].id = index == order.back() ? L"target" : L"fixed";
            pages[index].columns = columns.at(pages[index].id); pages[index].rows = rows.at(pages[index].id);
        }
    }

    page_mapping::State State()
    {
        return {savedIds, columns, rows, names, records, widgets, items, pages, offset, last, revision};
    }
    page_mapping::Services Services()
    {
        page_mapping::Services services;
        services.ready = [this] { return ready; };
        services.hasContent = [this](const auto& id) {
            return page_navigation_rules::HasContent(id, items, widgets,
                [this](const auto& item) { return Owned(item); }, [this](const auto& widget) { return Grouped(widget); });
        };
        services.monitorOrder = [this] { return order; };
        services.firstPage = [this]() -> const GridPage* { return order.empty() ? nullptr : &pages[order.front()]; };
        services.cancelHover = [this] { ++cancelled; };
        services.refreshOwnership = [this] { ++refreshed; };
        services.spacing = [](auto&) {};
        services.notify = [this](int index) {
            Require(index >= 0 && static_cast<size_t>(index) < savedIds.size(), "page notification identifies an existing retained page");
        };
        services.reserveDock = [this] {
            ++reserved;
            for (const auto& page : pages)
            {
                Require(std::find(savedIds.begin(), savedIds.end(), page.id) != savedIds.end(), "every active monitor has a retained mapped page");
                Require(columns.at(page.id) == page.columns && rows.at(page.id) == page.rows,
                    "Dock reservation observes the committed page dimensions");
            }
        };
        services.refreshIcons = [this] { ++icons; };
        return services;
    }
    void Apply() { page_mapping::Apply(State(), Services()); }
    std::wstring Named(const std::wstring& name) const
    {
        for (const auto& [id, value] : names) if (value == name) return id;
        return {};
    }
    bool Move(GridCell target, GridSpan span)
    {
        widget_displacement::Services services;
        services.grouped = [this](const auto& widget) { return Grouped(widget); };
        services.owned = [this](const auto& item) { return Owned(item); };
        services.generatePageId = [this] { return page_mapping::GenerateId(savedIds); };
        services.committed = [this] { ++commits; Apply(); Save(); };
        return widget_displacement::Place({widgets, items, pages, order, savedIds, columns, rows}, 0, std::move(target), span, true, services);
    }

    // Reconstruct placement/member state from the real stored document, then
    // remap the explicitly requested destination with the same monitor input.
    // Full DesktopApp startup, enumeration and pixel layout remain boundaries.
    void Replay() const
    {
        layout_storage::Document document;
        Require(layout_storage::LoadDocument(root / L"layout.json", document).status == layout_storage::LoadStatus::LoadedPrimary,
            "reopen the actual post-move page document");
        std::vector<std::wstring> ids;
        std::unordered_map<std::wstring, int> savedColumns, savedRows;
        std::unordered_map<std::wstring, std::wstring> savedNames;
        for (const auto& record : document.pages)
        {
            const auto id = Decode(record.id); ids.push_back(id);
            Require(record.columns && record.rows, "saved pages retain their own grid dimensions");
            savedColumns[id] = *record.columns; savedRows[id] = *record.rows;
            if (record.name) savedNames[id] = Decode(*record.name);
        }
        std::vector<DesktopWidget> restoredWidgets;
        for (const auto& saved : document.widgets)
        {
            DesktopWidget widget; widget.id = Decode(saved.id); widget.type = widget_layout_codec::TypeFromName(Decode(saved.type), Key);
            widget.sourceFolderPath = Decode(saved.sourceFolderPath);
            widget.singleCellIconMode = saved.singleCellIconMode;
            widget_layout_codec::RestorePlacement(saved, widget, Decode);
            widget_layout_codec::RestoreMembers(saved, widget, Decode, Key);
            restoredWidgets.push_back(std::move(widget));
        }
        std::vector<DesktopItem> restoredItems;
        std::unordered_map<std::wstring, LayoutRecord> restoredRecords;
        for (const auto& saved : document.items)
        {
            auto record = layout_geometry_codec::RestoreItem(saved, Decode, [](const auto&) { return 0; });
            restoredRecords.emplace(Decode(saved.key), record);
            DesktopItem item; item.layoutKey = Decode(saved.key); item.parsingName = item.layoutKey;
            item.name = std::filesystem::path(item.parsingName).filename().wstring(); item.gridCell = record.cell; item.gridSpan = record.span;
            restoredItems.push_back(std::move(item));
        }
        const auto destination = Named(L"Destination");
        const auto index = std::find(ids.begin(), ids.end(), destination);
        Require(index != ids.end(), "the moved component's destination remains a persisted page identity");
        int requested = static_cast<int>(index - ids.begin()) - static_cast<int>(pages.size() - 1);
        auto restoredPages = pages;
        std::wstring restoredLast;
        std::uint64_t restoredRevision = 0;
        page_mapping::State state{ids, savedColumns, savedRows, savedNames, restoredRecords, restoredWidgets, restoredItems,
            restoredPages, requested, restoredLast, restoredRevision};
        page_mapping::Services services;
        services.ready = [] { return true; };
        services.hasContent = [&](const auto& id) {
            return page_navigation_rules::HasContent(id, restoredItems, restoredWidgets,
                [&](const auto& item) {
                    for (const auto& widget : restoredWidgets)
                        if (widget.type != DesktopWidgetType::FolderMapping && std::find(widget.itemKeys.begin(), widget.itemKeys.end(), item.layoutKey) != widget.itemKeys.end()) return true;
                    return false;
                }, [&](const auto& child) {
                    for (const auto& widget : restoredWidgets)
                        if (std::find(widget.childWidgetIds.begin(), widget.childWidgetIds.end(), child.id) != widget.childWidgetIds.end()) return true;
                    return false;
                });
        };
        services.monitorOrder = [&] { return order; };
        services.firstPage = [&]() -> const GridPage* { return &restoredPages[order.front()]; };
        services.cancelHover = services.refreshOwnership = services.reserveDock = services.refreshIcons = [] {};
        services.spacing = [](auto&) {}; services.notify = [](int) {};
        page_mapping::Apply(state, services);
        const auto moving = std::find_if(restoredWidgets.begin(), restoredWidgets.end(), [](const auto& widget) { return widget.id == L"moving"; });
        Require(moving != restoredWidgets.end() && moving->gridCell.pageId == restoredPages[order.back()].id &&
            moving->gridCell.column == 2 && moving->gridCell.row == 1 && savedNames.at(moving->gridCell.pageId) == L"Destination" &&
            restoredPages[order.back()].columns == 6 && restoredPages[order.back()].rows == 5,
            "reloaded component identity, exact cell and requested monitor mapping retain the destination grid");
        Require(moving->type == widgets[0].type && moving->sourceFolderPath == widgets[0].sourceFolderPath &&
            moving->singleCellIconMode == widgets[0].singleCellIconMode && moving->itemKeys == widgets[0].itemKeys && moving->childWidgetIds == widgets[0].childWidgetIds,
            "page cleanup and replay preserve the moved component's type, presentation, source and memberships");
    }
};

inline void Seed(World& world, DesktopWidgetType type, bool icon, bool named, int remaining)
{
    if (named) world.names[L"source"] = L"Source";
    DesktopWidget moving; moving.id = L"moving"; moving.type = type; moving.title = L"Original";
    moving.gridCell = {L"source", 0, 0}; moving.gridSpan = icon ? GridSpan{1, 1} : GridSpan{2, 2};
    moving.singleCellIconMode = icon; moving.listMode = true; moving.privacyMode = true;
    if (type == DesktopWidgetType::FolderMapping) moving.sourceFolderPath = (world.root / L"sources" / L"mapped").wstring();
    world.widgets.push_back(moving);
    if (type == DesktopWidgetType::Collection || type == DesktopWidgetType::FileCategories || IsGroupWidgetType(type))
    {
        world.Item(L"member.txt", {L"source", 0, 0});
        if (IsGroupWidgetType(type))
        {
            DesktopWidget child; child.id = L"child";
            child.type = type == DesktopWidgetType::CollectionGroup ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories;
            child.gridCell = {L"source", 0, 0}; child.gridSpan = {1, 1}; child.itemKeys = {world.items.back().layoutKey};
            world.widgets[0].childWidgetIds = {child.id}; world.widgets[0].activeCategoryId = child.id;
            world.widgets.push_back(std::move(child));
        }
        else world.widgets[0].itemKeys = {world.items.back().layoutKey};
    }
    world.Item(L"stationary.txt", {L"target", 5, 4});
    if (remaining == 1) world.Item(L"remaining.txt", {L"source", 4, 4});
    if (remaining == 2)
    {
        DesktopWidget other; other.id = L"remaining"; other.type = DesktopWidgetType::Collection;
        other.title = L"Remaining"; other.gridCell = {L"source", 4, 4}; other.gridSpan = {2, 2}; world.widgets.push_back(std::move(other));
    }
    for (const auto& item : world.items) world.records[item.layoutKey] = {item.gridCell, item.gridSpan, item.largeIcon, true, item.slot};
}

inline void Run(const std::filesystem::path& root)
{
    size_t serial = 0;
    for (size_t monitors : {size_t{1}, size_t{2}})
    for (bool reverse : {false, true})
    for (auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories, DesktopWidgetType::FolderMapping,
        DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (bool icon : {false, true})
    for (bool named : {false, true})
    for (int remaining : {0, 1, 2})
    {
        if (monitors == 1 && reverse) continue;
        World world(root / std::to_wstring(serial++), monitors, reverse);
        Seed(world, type, icon, named, remaining);
        world.Legal(); const auto originalFiles = widget_displacement_cases::Files(world.root / L"sources");
        const auto metadata = world.Metadata(); const auto before = world.widgets;
        Require(world.Move({L"target", 2, 1}, world.widgets[0].gridSpan), "cross-page component commit accepts the supplied free destination");
        const auto destination = world.Named(L"Destination");
        Require(!destination.empty() && world.widgets[0].gridCell.pageId == destination && world.widgets[0].gridCell.column == 2 && world.widgets[0].gridCell.row == 1 &&
            world.pages[world.order.back()].id == destination && world.pages[world.order.back()].columns == 6 && world.pages[world.order.back()].rows == 5,
            "removing empty predecessors keeps the requested destination mapped with its own dimensions");
        Require(!named || !world.Named(L"Source").empty(), "a named source page survives after its last component leaves");
        Require(!world.Named(L"Later").empty(), "named empty pages survive cleanup and remain accessible after the move");
        Require(world.savedIds.size() == monitors + 1 + static_cast<size_t>(named || remaining != 0),
            "only unnamed empty surplus pages are reclaimed; live or named sources and every monitor are retained");
        Require(world.Metadata() == metadata && world.widgets.size() == before.size(), "page mapping preserves file metadata and all original component identities");
        for (size_t index = 0; index < before.size(); ++index)
            Require(world.widgets[index].id == before[index].id && world.widgets[index].type == before[index].type &&
                world.widgets[index].itemKeys == before[index].itemKeys && world.widgets[index].childWidgetIds == before[index].childWidgetIds &&
                world.widgets[index].sourceFolderPath == before[index].sourceFolderPath &&
                world.widgets[index].gridSpan.columns == before[index].gridSpan.columns && world.widgets[index].gridSpan.rows == before[index].gridSpan.rows &&
                (index == 0 || (world.widgets[index].gridCell.column == before[index].gridCell.column && world.widgets[index].gridCell.row == before[index].gridCell.row)),
                "normalizing page identifiers never replaces components, membership, source data or unrelated coordinates");
        for (const auto& item : world.items)
            if (!world.Owned(item)) Require(world.records.at(item.layoutKey).cell.pageId == item.gridCell.pageId,
                "live file positions and remembered layout records use the same normalized page identities");
        Require(widget_displacement_cases::Files(world.root / L"sources") == originalFiles, "cross-page cleanup never changes underlying file names or bytes");
        world.Legal(); world.Reload(); world.Replay();
        if (named && remaining == 0)
        {
            const auto count = world.savedIds.size();
            world.names.erase(world.Named(L"Source")); world.Apply(); world.Save();
            Require(world.savedIds.size() + 1 == count && world.Named(L"Source").empty(),
                "clearing the empty source name restores automatic surplus-page cleanup");
            world.Legal(); world.Reload(); world.Replay();
        }
    }

    for (size_t monitors : {size_t{1}, size_t{2}})
    {
        World world(root / std::to_wstring(serial++), monitors, monitors == 2);
        world.savedIds.clear(); world.names.clear(); world.columns.clear(); world.rows.clear(); world.last.clear();
        const auto physical = world.pages;
        world.Apply();
        Require(world.savedIds.size() >= monitors && std::unordered_set<std::wstring>(world.savedIds.begin(), world.savedIds.end()).size() == world.savedIds.size(),
            "initial page padding provides a distinct saved page for each active monitor");
        for (size_t index = 0; index < monitors; ++index)
            Require(!world.pages[index].id.empty() && world.pages[index].columns == physical[index].columns && world.pages[index].rows == physical[index].rows,
                "padded pages inherit the grid of their configured monitor rather than another physical index");
    }
    {
        World world(root / std::to_wstring(serial++), 1, false);
        world.ready = false;
        world.Item(L"loaded.txt", {L"source", 3, 3}); const auto key = world.items[0].layoutKey;
        world.records[key] = {world.items[0].gridCell, world.items[0].gridSpan, {}, true, 0}; world.items.clear();
        world.Apply();
        Require(world.savedIds.size() == 3 && world.columns.size() == 3 && world.rows.size() == 3 &&
            std::find(world.savedIds.begin(), world.savedIds.end(), world.records.at(key).cell.pageId) != world.savedIds.end(),
            "unknown initial enumeration cannot erase pages or dimensions required by a loaded file record");
        DesktopItem restored; restored.name = L"loaded.txt"; restored.layoutKey = key;
        restored.gridCell = world.records.at(key).cell; restored.gridSpan = {1, 1}; world.items.push_back(std::move(restored));
        world.ready = true; world.Apply();
        Require(world.savedIds.size() == 3, "a subsequently enumerated visible file keeps its restored source page");
    }
    {
        World world(root / std::to_wstring(serial++), 1, false);
        world.ready = false; world.pages[0].columns = 12; world.pages[0].rows = 10;
        world.Apply();
        Require(world.pages[0].columns == 6 && world.pages[0].rows == 5,
            "mapping an initial monitor applies the saved destination grid before layout reservation");
    }
    std::cout << "cross-page component commit, cleanup, padding, identity remap, monitor mapping and storage cases passed\n";
}
}
