#pragma once

#include "dock_model_cases.h"
#include "dock/dock_desktop_return.h"
#include <set>
#include <tuple>

namespace dock_return_cases
{
using namespace drop_placement_cases;

struct World : dock_model_cases::World
{
    std::vector<GridPage> pages;
    explicit World(std::filesystem::path path) : dock_model_cases::World(std::move(path))
    {
        GridPage first; first.id = L"page"; first.columns = 5; first.rows = 5;
        GridPage second; second.id = L"other-page"; second.columns = 6; second.rows = 4;
        pages = {first, second};
    }
    bool Grouped(const DesktopWidget& child) const
    {
        for (const auto& group : widgets)
            if (IsGroupWidgetType(group.type) && std::find(group.childWidgetIds.begin(), group.childWidgetIds.end(), child.id) != group.childWidgetIds.end()) return true;
        return false;
    }
    bool Owned(const DesktopItem& item) const
    {
        for (const auto& widget : widgets)
            if (widget.type != DesktopWidgetType::FolderMapping &&
                std::find(widget.itemKeys.begin(), widget.itemKeys.end(), item.layoutKey) != widget.itemKeys.end()) return true;
        return false;
    }
    dock_desktop_return::Services ReturnServices()
    {
        dock_desktop_return::Services services;
        services.normalize = Key;
        services.grouped = [this](const auto& widget) { return Grouped(widget); };
        services.owned = [this](const auto& item) { return Owned(item); };
        services.itemIndex = [this](const auto& key) {
            for (size_t index = 0; index < items.size(); ++index) if (Key(items[index].layoutKey) == Key(key)) return index;
            return items.size();
        };
        return services;
    }
    std::vector<dock_desktop_return::Landing> Plan(std::vector<size_t> indices, GridCell target)
    {
        return dock_desktop_return::Plan({dock, widgets, items, pages}, std::move(indices), std::move(target), ReturnServices());
    }
    bool Apply(const std::vector<dock_desktop_return::Landing>& landings)
    {
        const bool changed = dock_desktop_return::Apply(dock, widgets, items, landings, ReturnServices());
        if (changed) dock_drop_model::Normalize(dock, Services());
        return changed;
    }
    void Legal() const
    {
        std::set<std::tuple<std::wstring, int, int>> occupied;
        const auto add = [&](const GridCell& cell, GridSpan span) {
            const auto page = std::find_if(pages.begin(), pages.end(), [&](const auto& value) { return value.id == cell.pageId; });
            Require(page != pages.end() && span.columns > 0 && span.rows > 0,
                "Dock return keeps each visible object's full footprint in bounds and without overlap");
            for (int column = 0; column < span.columns; ++column)
                for (int row = 0; row < span.rows; ++row)
                    Require(cell.column + column >= 0 && cell.column + column < page->columns &&
                        cell.row + row >= 0 && cell.row + row < page->rows && occupied.emplace(cell.pageId, cell.column + column, cell.row + row).second,
                        "Dock return keeps each visible object's full footprint in bounds and without overlap");
        };
        for (const auto& widget : widgets)
            if (!Grouped(widget) && widget.gridCell.pageId != kDockPageId) add(widget.gridCell, widget.gridSpan);
        for (const auto& item : items)
            if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !Owned(item)) add(item.gridCell, item.gridSpan);
    }
};

inline void Run(const std::filesystem::path& root)
{
    size_t serial = 0;
    // B04/B05 and the bound "move here" hint: the selected existing identity
    // returns to the supplied empty cell; no backing file is moved or copied.
    for (const auto& page : {L"page", L"other-page"})
    for (size_t count : {size_t{1}, size_t{2}})
    {
        World world(root / std::to_wstring(serial++));
        const auto beforeItems = world.ItemStates();
        std::vector<DockEntry> additions;
        for (size_t index = 0; index < count; ++index) additions.push_back({DockEntryType::DesktopItem, world.items[index].layoutKey, false});
        Require(dock_drop_model::Pin({world.dock, world.widgets}, additions, 0, world.Services()), "seed actual exclusive file pins");
        world.dock.push_back({DockEntryType::DesktopItem, L"UNCHANGED", true});
        const GridCell target{page, 3, 3};
        const auto pinned = world.Snapshot();
        const auto plan = world.Plan(count == 1 ? std::vector<size_t>{0} : std::vector<size_t>{1, 0}, target);
        Require(world.Snapshot() == pinned, "Dock return preview does not change file positions or Dock ownership");
        Require(plan.size() == count && plan[0].accepted && dock_model_cases::SameCell(plan[0].cell, target),
            "exclusive file return previews the specified empty screen and cell");
        Require(world.Apply(plan) && world.Order() == std::vector<std::wstring>{L"UNCHANGED"},
            "successful Dock return removes only the restored selected references");
        for (const auto& landing : plan)
            Require(landing.accepted && dock_model_cases::SameCell(world.items[landing.entryIndex].gridCell, landing.cell),
                "each returned file identity occupies its own accepted preview cell");
        for (size_t index = count; index < world.items.size(); ++index)
            Require(dock_model_cases::SameCell(world.items[index].gridCell, beforeItems[index].gridCell),
                "Dock return preserves every unrelated desktop entry position");
        world.Legal(); world.Files(); world.SaveReload({L"UNCHANGED"});
    }

    const std::array<DesktopWidgetType, 5> types{DesktopWidgetType::Collection, DesktopWidgetType::FileCategories,
        DesktopWidgetType::FolderMapping, DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup};
    for (auto type : types)
    for (bool icon : {false, true})
    for (const auto& page : {L"page", L"other-page"})
    {
        World world(root / std::to_wstring(serial++));
        DesktopWidget component; component.id = L"moving"; component.type = type;
        component.gridCell = {L"page", 3, 3}; component.gridSpan = icon ? GridSpan{1, 1} : GridSpan{2, 2};
        component.title = L"original title"; component.privacyMode = true; component.listMode = true; component.singleCellIconMode = icon;
        component.sourceFolderPath = (world.root / L"source").wstring();
        if (!IsGroupWidgetType(type)) component.itemKeys = {world.items[0].layoutKey, world.items[1].layoutKey};
        else { component.childWidgetIds = {L"nested-one", L"nested-two"}; component.activeCategoryId = L"nested-two"; }
        world.widgets.push_back(component);
        if (IsGroupWidgetType(type))
            for (size_t index = 0; index < 2; ++index)
            {
                DesktopWidget nested; nested.id = component.childWidgetIds[index];
                nested.type = type == DesktopWidgetType::CollectionGroup ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories;
                nested.itemKeys = {world.items[index].layoutKey}; world.widgets.push_back(nested);
            }
        Require(dock_drop_model::Pin({world.dock, world.widgets}, {{DockEntryTypeForWidget(type), component.id, false}}, 0, world.Services()),
            "seed actual component pin without replacing its contents");
        const GridCell target{page, 3, 2};
        const auto plan = world.Plan({0}, target);
        Require(plan.size() == 1 && plan[0].accepted && dock_model_cases::SameCell(plan[0].cell, target),
            "component return previews the specified legal cell with the existing footprint");
        Require(world.Apply(plan) && world.dock.empty() && dock_model_cases::SameCell(world.widgets[0].gridCell, target),
            "component return commits the preview cell and removes its old Dock reference");
        const auto& returned = world.widgets[0];
        Require(returned.id == component.id && returned.type == type && returned.title == component.title &&
            returned.gridSpan.columns == component.gridSpan.columns && returned.gridSpan.rows == component.gridSpan.rows &&
            returned.itemKeys == component.itemKeys && returned.childWidgetIds == component.childWidgetIds &&
            returned.activeCategoryId == component.activeCategoryId && returned.sourceFolderPath == component.sourceFolderPath &&
            returned.singleCellIconMode == icon && returned.privacyMode && returned.listMode,
            "returning a Dock component preserves identity, membership, source directory, footprint and presentation");
        world.Legal(); world.Files(); world.SaveReload({});
    }

    // The Dock icon itself is ordinary, but the restored desktop object still
    // owns its large-icon footprint. Independent cell enumeration grades it.
    {
        World world(root / std::to_wstring(serial++));
        world.items[0].gridCell = {L"page", 0, 2}; world.items[0].gridSpan = {3, 3};
        world.items[0].largeIcon = LargeIconConfig{}; world.items[0].largeIcon->columns = 3; world.items[0].largeIcon->rows = 3;
        world.items[0].largeIcon->autoColor = false; world.items[0].largeIcon->manualColor = 0x123456;
        const auto appearance = *world.items[0].largeIcon;
        world.items[1].gridCell = {L"page", 4, 4};
        world.Legal();
        Require(dock_drop_model::Pin({world.dock, world.widgets}, {{DockEntryType::DesktopItem, world.items[0].layoutKey, false}}, 0, world.Services()),
            "seed a real exclusive pin of the large desktop identity");
        const auto before = world.Snapshot();
        const auto plan = world.Plan({0}, {L"page", 2, 2});
        const bool accepted = world.Apply(plan);
        if (!accepted) Require(world.Snapshot() == before, "rejected large Dock return leaves appearance, geometry and references unchanged");
        Require(world.items[0].gridSpan.columns == 3 && world.items[0].gridSpan.rows == 3 &&
            world.items[0].largeIcon && *world.items[0].largeIcon == appearance,
            "Dock return preserves the large desktop object's original size intent and appearance");
        world.Legal(); world.Files(); world.SaveReload(accepted ? std::vector<std::wstring>{} : std::vector<std::wstring>{world.items[0].layoutKey});
    }

    for (const auto& target : {GridCell{L"missing-page", 0, 0}, GridCell{L"page", 2, 1}})
    {
        World world(root / std::to_wstring(serial++));
        Require(dock_drop_model::Pin({world.dock, world.widgets}, {{DockEntryType::DesktopItem, world.items[0].layoutKey, false}}, 0, world.Services()),
            "seed pin for unavailable return target");
        const auto before = world.Snapshot();
        const auto plan = world.Plan({0}, target);
        if (!world.Apply(plan)) Require(world.Snapshot() == before,
            "unavailable Dock return leaves all original references, positions and ownership unchanged");
        world.Legal(); world.Files();
    }
    {
        World world(root / std::to_wstring(serial++));
        world.dock = {{DockEntryType::DesktopItem, world.items[0].layoutKey, false}};
        world.items[0].gridCell = {kDockPageId, 0, 0};
        const auto plan = world.Plan({0}, {L"page", 3, 3});
        world.dock[0].reference = world.items[1].layoutKey;
        const auto before = world.Snapshot();
        Require(!world.Apply(plan) && world.Snapshot() == before,
            "a captured Dock return cannot move or consume a replacement source identity");
        world.Files();
    }
    std::cout << "Dock desktop return, preview, identity, geometry and actual storage cases passed\n";
}
}
