#pragma once

#include "page_mapping_cases.h"
#include "drag_drop/internal_desktop_drop.h"
#include "drag_drop/desktop_landings.h"
#include "desktop/desktop_membership_recovery.h"

namespace internal_desktop_cases
{
using namespace drop_placement_cases;

struct World : page_mapping_cases::World
{
    std::vector<DockEntry> dock;
    int completed = 0, internals = 0;
    explicit World(std::filesystem::path path, size_t monitors = 1, bool reverse = false)
        : page_mapping_cases::World(std::move(path), monitors, reverse) {}

    size_t ItemIndex(const std::wstring& key) const
    {
        for (size_t index = 0; index < items.size(); ++index)
            if (Key(items[index].layoutKey) == Key(key)) return index;
        return items.size();
    }
    bool Execute(const DragSourceList& sources, const DropPreviewList& preview)
    {
        drop_pipeline::Services pipeline;
        pipeline.autoCollectSource = [&](const auto& list) { return drop_pipeline::AutoCollectSource(list, widgets); };
        pipeline.normalize = Key;
        pipeline.files = [](const auto&, const auto&, auto, bool, auto) -> bool {
            throw std::runtime_error("internal desktop move cannot submit physical file IO");
        };
        pipeline.internal = [&](const auto& list, const auto& plan) {
            ++internals;
            internal_desktop_drop::Services services;
            services.findItem = [&](const auto& key) { return ItemIndex(key); };
            services.findWidget = [&](const auto& id) {
                return static_cast<size_t>(std::find_if(widgets.begin(), widgets.end(),
                    [&](const auto& widget) { return widget.id == id; }) - widgets.begin());
            };
            services.removeKeys = [&](const auto& keys) {
                for (auto& widget : widgets) desktop_membership_recovery::Forget(widget, keys);
            };
            services.slot = [&](const auto& cell) { return widget_displacement::Slot(pages, cell); };
            services.mapPages = [&] { Apply(); };
            // Auto-collection is disabled in these inputs. Full LayoutItems,
            // native containers and rendering remain host service boundaries.
            services.prepareLayout = [] {};
            services.committed = [&] { ++commits; Save(); };
            return internal_desktop_drop::Execute(list, plan, items, widgets, {savedIds, columns, rows}, services);
        };
        bool notified = false;
        const bool result = drop_pipeline::Execute(sources, preview, widgets, dock, pipeline,
            [&](bool success) { ++completed; notified = success; }, true, {});
        Require(completed == 1 && internals == 1 && notified == result,
            "internal desktop dispatch completes exactly one model operation with its actual result");
        return result;
    }
};

inline DragSourceList Sources(const World& world, size_t count)
{
    DragSourceList result; result.hasDesktopIcons = true;
    for (size_t index = 0; index < count; ++index)
    {
        const auto& item = world.items[index];
        DragSourceEntry source; source.kind = DropSourceKind::DesktopIcon;
        source.sourceIndex = index == 0 ? 9 : 2; source.desktopIndex = index;
        source.desktopKey = item.layoutKey; source.filePath = item.parsingName;
        source.originalCell = item.gridCell; source.originalSpan = item.gridSpan;
        result.entries.push_back(std::move(source));
    }
    for (const auto& widget : world.widgets)
        if (std::find(widget.itemKeys.begin(), widget.itemKeys.end(), result.entries.front().desktopKey) != widget.itemKeys.end())
        {
            result.hasOriginWidget = true; result.originWidgetId = widget.id; result.originWidgetType = widget.type;
            for (auto& entry : result.entries) entry.widgetId = widget.id;
            break;
        }
    return result;
}

inline DropPreviewList Preview(const DragSourceList& sources)
{
    DropPreviewList preview; preview.targetKind = DropTargetKind::Desktop; preview.action = DropAction::Move;
    for (size_t index = 0; index < sources.entries.size(); ++index)
    {
        DropLanding landing; landing.kind = DropLandingKind::DesktopCell;
        landing.sourceIndex = sources.entries[index].sourceIndex;
        landing.cell = {L"target", index == 0 ? 1 : 4, 1}; landing.span = sources.entries[index].originalSpan;
        preview.landings.push_back(std::move(landing));
    }
    std::reverse(preview.landings.begin(), preview.landings.end());
    return preview;
}

inline void Seed(World& world, size_t count, int owner, bool large)
{
    for (size_t index = 0; index < count; ++index)
    {
        world.Item(index == 0 ? L"selected-A.txt" : L"selected-B.txt",
            {L"source", static_cast<int>(index * 3), 0}, large && index == 0 ? GridSpan{2, 2} : GridSpan{1, 1});
        if (large && index == 0)
        {
            world.items[0].largeIcon.emplace(); world.items[0].largeIcon->columns = 2; world.items[0].largeIcon->rows = 2;
        }
    }
    world.Item(L"stationary.txt", {L"target", 5, 4});
    if (owner != 0)
    {
        DesktopWidget widget; widget.id = L"owner";
        widget.type = owner == 2 || owner == 4 ? DesktopWidgetType::FileCategories : DesktopWidgetType::Collection;
        widget.gridCell = {L"later", 0, 0}; widget.gridSpan = {2, 2};
        for (size_t index = 0; index < count; ++index) widget.itemKeys.push_back(world.items[index].layoutKey);
        widget.missingItems.push_back({L"unrelated-missing", 1, 1});
        if (owner >= 3)
        {
            DesktopWidget group; group.id = L"group";
            group.type = owner == 3 ? DesktopWidgetType::CollectionGroup : DesktopWidgetType::FileGroup;
            group.gridCell = {L"later", 0, 0}; group.gridSpan = {2, 2};
            group.childWidgetIds = {widget.id}; group.activeCategoryId = widget.id;
            world.widgets.push_back(std::move(group));
        }
        world.widgets.push_back(std::move(widget));
    }
    for (const auto& item : world.items) world.records[item.layoutKey] = {item.gridCell, item.gridSpan, item.largeIcon, true, item.slot};
}

inline void RunLandings(const std::filesystem::path& root, bool previewOnly = false)
{
    size_t serial = 0;
    for (size_t monitors : {size_t{1}, size_t{2}})
    for (bool reverse : {false, true})
    for (size_t count : {size_t{1}, size_t{2}})
    for (int owner : {1, 2, 3, 4})
    for (bool smallGrid : {false, true})
    for (bool full : {false, true})
    {
        if ((monitors == 1 && reverse) || (smallGrid && !full)) continue;
        World world(root / (L"landings-" + std::to_wstring(serial++)), monitors, reverse);
        Seed(world, count, owner, false);
        // Keep a complete, valid enumerated model. The nonselected file stays
        // in its original logical owner while selected files leave it.
        world.widgets.back().itemKeys.push_back(world.items.back().layoutKey);
        world.names[L"source"] = L"Source";
        if (smallGrid) { world.columns[L"target"] = 1; world.rows[L"target"] = 1; }
        const auto fill = [&](const std::wstring& id) {
            DesktopWidget widget; widget.id = L"filler-" + id; widget.type = DesktopWidgetType::Collection;
            widget.gridCell = {id, 0, 0}; widget.gridSpan = {world.columns.at(id), world.rows.at(id)};
            widget.singleCellIconMode = widget.gridSpan.columns == 1 && widget.gridSpan.rows == 1;
            world.widgets.push_back(std::move(widget));
        };
        if (full) fill(L"target");
        if (monitors > 1) fill(L"fixed");
        // Start from the host's canonical committed page identities. This
        // prevents fixture-only opaque IDs from aliasing a newly planned ID.
        world.Apply(); world.Legal(); world.Save();
        const auto source = Sources(world, count);
        const auto target = world.Named(L"Destination");
        const auto* page = widget_displacement::FindPage(world.pages, target);
        Require(page != nullptr, "the requested landing page is displayed by the configured monitor mapping");
        desktop_landings::Services services;
        services.grouped = [&](const auto& widget) { return world.Grouped(widget); };
        services.owned = [&](const auto& item) { return world.Owned(item); };
        services.normalize = Key;
        services.findFree = [&](auto span, const auto& used, auto& cell, const auto& preferred, int slot) {
            return grid_occupancy::FindFree(world.pages, world.order, span, used, cell, preferred, slot);
        };
        services.generateId = [&](const auto& reserved) {
            auto ids = world.savedIds; ids.insert(ids.end(), reserved.begin(), reserved.end());
            return page_mapping::GenerateId(ids);
        };
        const GridCell request{target, smallGrid ? 0 : 1, smallGrid ? 0 : 1};
        const auto before = world.Snapshot(); const auto stored = Bytes(world.root / L"layout.json");
        const auto files = widget_displacement_cases::Files(world.root / L"sources");
        const auto metadata = world.Metadata();
        auto preview = Preview(source);
        preview.anchorCell = request;
        preview.landings = desktop_landings::Build(source, request, true, *page, world.widgets, world.items,
            world.columns, world.rows, services);
        Require(world.Snapshot() == before && Bytes(world.root / L"layout.json") == stored && world.commits == 0,
            "desktop landing preview never changes source ownership, page records or persisted layout");
        std::set<std::tuple<std::wstring, int, int>> previewCells;
        for (const auto& landing : preview.landings)
            for (int column = 0; column < landing.span.columns; ++column)
                for (int row = 0; row < landing.span.rows; ++row)
                    Require(landing.span.columns > 0 && landing.span.rows > 0 && landing.cell.column + column >= 0 &&
                        landing.cell.row + row >= 0 && previewCells.emplace(landing.cell.pageId, landing.cell.column + column, landing.cell.row + row).second,
                        "desktop preview publishes nonoverlapping legal landings for accepted sources");
        if (previewOnly) continue;
        // Q08 does not prescribe a full-page fallback. A declined plan may
        // preserve the model; every plan that does commit must remain legal.
        if (preview.Empty()) continue;
        Require(world.Execute(source, preview), "an unchanged accepted desktop landing plan commits its captured sources");
        for (const auto& landing : preview.landings)
        {
            const auto entry = std::find_if(source.entries.begin(), source.entries.end(),
                [&](const auto& value) { return value.sourceIndex == landing.sourceIndex; });
            Require(entry != source.entries.end(), "each published landing identifies an original selected file");
            const auto index = world.ItemIndex(entry->desktopKey);
            const auto& item = world.items.at(index);
            Require(!world.Owned(item) && std::find(world.savedIds.begin(), world.savedIds.end(), item.gridCell.pageId) != world.savedIds.end() &&
                world.columns.contains(item.gridCell.pageId) && world.rows.contains(item.gridCell.pageId),
                "accepted internal overflow landings remain accessible on retained pages after commit and reload");
            Require(item.gridCell.column == landing.cell.column && item.gridCell.row == landing.cell.row,
                "accepted desktop landings retain their captured cell coordinates");
            if (!full && count == 1)
                Require(item.gridCell.pageId == world.Named(L"Destination") && item.gridCell.column == 1 && item.gridCell.row == 1,
                    "a free single-file target fulfills the visible requested destination cell");
        }
        Require(world.Metadata() == metadata && widget_displacement_cases::Files(world.root / L"sources") == files,
            "planning and committing internal landings preserve original file identities, metadata and bytes");
        world.Legal(); world.Reload();
    }
}

inline void Run(const std::filesystem::path& root, std::wstring_view section = L"all")
{
    Require(section == L"all" || section == L"pages" || section == L"identity" || section == L"rejection" || section == L"landings" || section == L"landing-preview",
        "internal desktop diagnostic section must select a nonempty known scenario group");
    size_t serial = 0;
    if (section == L"all" || section == L"pages")
    for (size_t monitors : {size_t{1}, size_t{2}})
    for (bool reverse : {false, true})
    for (size_t count : {size_t{1}, size_t{2}})
    for (int owner : {0, 1, 2, 3, 4})
    for (bool named : {false, true})
    for (bool large : {false, true})
    {
        if ((monitors == 1 && reverse) || (owner != 0 && large)) continue;
        World world(root / std::to_wstring(serial++), monitors, reverse); Seed(world, count, owner, large);
        if (named) world.names[L"source"] = L"Source";
        const auto sources = Sources(world, count); const auto preview = Preview(sources);
        const auto metadata = world.Metadata(); const auto files = widget_displacement_cases::Files(world.root / L"sources");
        const auto widgets = world.widgets; world.Legal();
        Require(world.Execute(sources, preview), "internal desktop move accepts the supplied free destination cells");
        const auto destination = world.Named(L"Destination");
        Require(world.savedIds.size() == monitors + 1 + static_cast<size_t>(named) &&
            world.pages[world.order.back()].id == destination && world.pages[world.order.back()].columns == 6 && world.pages[world.order.back()].rows == 5,
            "internal file move preserves named pages, reclaims only empty surplus pages and retains destination mapping");
        for (size_t index = 0; index < count; ++index)
        {
            const auto item = world.ItemIndex(sources.entries[index].desktopKey);
            Require(item < world.items.size() && world.items[item].gridCell.pageId == destination &&
                world.items[item].gridCell.column == (index == 0 ? 1 : 4) && world.items[item].gridCell.row == 1 && !world.Owned(world.items[item]),
                "each original file identity reaches its own captured cell and leaves only its old logical ownership");
        }
        Require(world.Metadata() == metadata && widget_displacement_cases::Files(world.root / L"sources") == files,
            "internal file move preserves paths, bytes, metadata and desktop large-icon configuration");
        Require(world.widgets.size() == widgets.size(), "releasing files never replaces their component or group identities");
        for (size_t index = 0; index < widgets.size(); ++index)
            Require(world.widgets[index].id == widgets[index].id && world.widgets[index].type == widgets[index].type &&
                world.widgets[index].childWidgetIds == widgets[index].childWidgetIds && world.widgets[index].activeCategoryId == widgets[index].activeCategoryId &&
                world.widgets[index].missingItems.size() == widgets[index].missingItems.size(),
                "file release preserves group children, active label and unrelated missing members");
        world.Legal(); world.Reload();
    }
    if (section == L"all" || section == L"identity")
    {
        World world(root / L"identity"); Seed(world, 1, 0, false);
        const auto sources = Sources(world, 1); const auto preview = Preview(sources);
        std::swap(world.items[0], world.items[1]);
        Require(world.Execute(sources, preview), "source enumeration reorder retains an accepted stable source identity");
        const auto moved = world.ItemIndex(sources.entries[0].desktopKey);
        const auto other = world.ItemIndex(Key((world.root / L"sources" / L"stationary.txt").wstring()));
        Require(world.items[moved].gridCell.column == 1 && world.items[moved].gridCell.row == 1 &&
            world.items[moved].gridCell.pageId == world.Named(L"Destination") &&
            world.items[other].gridCell.column == 5 && world.items[other].gridCell.row == 4,
            "captured array indices cannot move a replacement file after source enumeration reorders");
        world.Legal(); world.Reload();
    }
    if (section == L"all" || section == L"rejection")
    {
        World world(root / L"rejection"); Seed(world, 1, 1, false);
        const auto sources = Sources(world, 1); auto preview = Preview(sources);
        preview.landings[0].sourceIndex = 700;
        world.Save(); const auto saved = Bytes(world.root / L"layout.json"); const auto before = world.Snapshot();
        Require(!world.Execute(sources, preview) && world.Snapshot() == before && world.commits == 0 && Bytes(world.root / L"layout.json") == saved,
            "an unmatched landing rejects without revoking source ownership or changing memory and saved layout");
    }
    if (section == L"all" || section == L"landings") RunLandings(root);
    if (section == L"landing-preview") RunLandings(root, true);
    if (section == L"all" || section == L"landings")
    for (int invalid : {0, 1, 2, 3})
    {
        World world(root / (L"invalid-planned-page-" + std::to_wstring(invalid))); Seed(world, 1, 1, false);
        world.Apply(); const auto source = Sources(world, 1); auto preview = Preview(source);
        const auto id = page_mapping::GenerateId(world.savedIds);
        auto& landing = preview.landings[0]; landing.cell = {id, 0, 0}; landing.plannedPageSize = GridSpan{2, 2};
        if (invalid == 0)
        {
            world.savedIds.push_back(id); world.columns[id] = 2; world.rows[id] = 2; world.names[id] = L"Replacement";
        }
        else if (invalid == 1) landing.plannedPageSize = GridSpan{0, 0};
        else if (invalid == 2) landing.cell.column = 2;
        else landing.plannedPageSize.reset();
        world.Save(); const auto before = world.Snapshot(); const auto stored = Bytes(world.root / L"layout.json");
        Require(!world.Execute(source, preview) && world.Snapshot() == before && world.commits == 0 &&
            Bytes(world.root / L"layout.json") == stored && world.Named(L"Replacement") == (invalid == 0 ? id : L""),
            "invalid or replaced planned pages reject before page registration, ownership removal and persisted mutation");
    }
    std::cout << "internal desktop file dispatch, identity, rejection, landing preview, page mapping and storage cases passed\n";
}
}
