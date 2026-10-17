#pragma once

#include "internal_desktop_cases.h"
#include "layout/selected_desktop_move.h"

namespace selected_desktop_cases
{
using namespace drop_placement_cases;
using World = internal_desktop_cases::World;

inline DropPreviewList Preview(const World& world, const DragSourceList& sources, GridCell target)
{
    const selected_desktop_move::State state{world.items, world.widgets, world.pages};
    const selected_desktop_move::Services services{
        [&](const auto& item) { return world.Owned(item); }, [&](const auto& widget) { return world.Grouped(widget); }};
    const auto planned = selected_desktop_move::Build(state, target, services, &sources);
    DropPreviewList preview; preview.targetKind = DropTargetKind::Desktop; preview.action = DropAction::Move; preview.anchorCell = target;
    preview.landings = selected_desktop_move::Landings(state, sources, planned);
    return preview;
}

inline DragSourceList WidgetSource(const World& world)
{
    DragSourceEntry entry; entry.kind = DropSourceKind::Widget; entry.sourceIndex = 37;
    entry.desktopWidgetIcon = true; entry.widgetId = world.widgets[0].id; entry.widgetType = world.widgets[0].type;
    entry.originalCell = world.widgets[0].gridCell; entry.originalSpan = world.widgets[0].gridSpan;
    DragSourceList source; source.hasWidgets = true; source.entries.push_back(std::move(entry)); return source;
}

inline void CheckCommit(World& world, const DragSourceList& sources, const DropPreviewList& preview)
{
    const auto files = widget_displacement_cases::Files(world.root / L"sources"); const auto metadata = world.Metadata();
    const auto beforeWidgets = world.widgets;
    Require(world.Execute(sources, preview), "an unchanged accepted desktop selection commits without physical file IO");
    const auto destination = world.Named(L"Destination");
    for (const auto& landing : preview.landings)
    {
        const auto source = std::find_if(sources.entries.begin(), sources.entries.end(), [&](const auto& entry) { return entry.sourceIndex == landing.sourceIndex; });
        Require(source != sources.entries.end(), "every selected desktop landing retains its captured source identity");
        if (source->desktopWidgetIcon)
        {
            const auto widget = std::find_if(world.widgets.begin(), world.widgets.end(), [&](const auto& entry) { return entry.id == source->widgetId; });
            Require(widget != world.widgets.end() && widget->gridCell.pageId == destination &&
                widget->gridCell.column == landing.cell.column && widget->gridCell.row == landing.cell.row,
                "selected component icons commit their own captured page and cell without replacing their identity");
        }
        else
        {
            const auto index = world.ItemIndex(source->desktopKey);
            Require(index < world.items.size() && world.items[index].gridCell.pageId == destination &&
                world.items[index].gridCell.column == landing.cell.column && world.items[index].gridCell.row == landing.cell.row,
                "selected desktop files commit their own captured page and cell after page remapping");
        }
    }
    Require(world.Metadata() == metadata && widget_displacement_cases::Files(world.root / L"sources") == files && world.widgets.size() == beforeWidgets.size(),
        "desktop translation preserves source bytes, file metadata, component identities and configuration");
    for (size_t index = 0; index < world.widgets.size(); ++index)
    {
        auto normalized = world.widgets[index]; normalized.gridCell = beforeWidgets[index].gridCell;
        Require(widget_displacement_cases::Widgets({normalized}) == widget_displacement_cases::Widgets({beforeWidgets[index]}),
            "desktop icon translation preserves component type, settings, members, child order and active label");
    }
    world.Legal(); world.Reload();
}

inline void Run(const std::filesystem::path& root, std::wstring_view section = L"all")
{
    Require(section == L"all" || section == L"cases" || section == L"selection" || section == L"occupied" || section == L"identity",
        "selected desktop diagnostics select a known nonempty section");
    size_t serial = 0;
    if (section == L"all" || section == L"cases")
    for (size_t monitors : {size_t{1}, size_t{2}})
    for (bool reverse : {false, true})
    for (bool named : {false, true})
    for (size_t count : {size_t{1}, size_t{2}})
    for (bool large : {false, true})
    {
        if (monitors == 1 && reverse) continue;
        World world(root / std::to_wstring(serial++), monitors, reverse);
        internal_desktop_cases::Seed(world, count, 0, large);
        if (named) world.names[L"source"] = L"Source";
        for (size_t index = 0; index < world.items.size(); ++index) world.items[index].selected = index < count;
        world.Apply(); world.Legal(); const auto countBefore = world.savedIds.size();
        const auto sources = internal_desktop_cases::Sources(world, count); const auto before = world.Snapshot();
        const auto preview = Preview(world, sources, {world.Named(L"Destination"), 1, 1});
        Require(world.Snapshot() == before && preview.landings.size() == count, "a free desktop file scene plans every captured source without changing the model");
        if (count == 1) Require(preview.landings[0].cell.column == 1 && preview.landings[0].cell.row == 1,
            "a captured single desktop source reaches its requested free cell without unrelated selection influence");
        CheckCommit(world, sources, preview);
        Require(world.savedIds.size() == countBefore - static_cast<size_t>(!named), "selected file translation preserves named source pages and reclaims empty unnamed surplus pages");
    }
    if (section == L"all" || section == L"cases")
    for (auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories, DesktopWidgetType::FolderMapping,
        DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (bool mixed : {false, true})
    {
        World world(root / std::to_wstring(serial++)); page_mapping_cases::Seed(world, type, true, false, 0);
        world.widgets[0].selected = true;
        for (auto& item : world.items) item.selected = false;
        if (mixed) { world.Item(L"mixed.txt", {L"source", 2, 0}); world.items.back().selected = true; }
        world.Apply(); world.Legal(); auto sources = WidgetSource(world);
        if (mixed)
        {
            sources.hasDesktopIcons = true;
            const auto& item = world.items.back(); DragSourceEntry entry; entry.kind = DropSourceKind::DesktopIcon;
            entry.sourceIndex = 71; entry.desktopIndex = world.items.size() - 1; entry.desktopKey = item.layoutKey; entry.filePath = item.parsingName;
            entry.originalCell = item.gridCell; entry.originalSpan = item.gridSpan; sources.entries.push_back(std::move(entry));
        }
        const auto before = world.Snapshot(); const auto preview = Preview(world, sources, {world.Named(L"Destination"), 1, 1});
        Require(world.Snapshot() == before, "component-icon desktop preview never changes the model or source ownership");
        // Q02 mixed admission remains undefined. Accepted plans must still keep
        // every represented object legal and fulfill their own preview cells.
        if (mixed && preview.Empty()) continue;
        Require(preview.landings.size() == sources.entries.size(), "an accepted component-icon scene retains every captured source identity");
        CheckCommit(world, sources, preview);
    }
    if (section == L"all" || section == L"selection")
    {
        World world(root / L"selection"); internal_desktop_cases::Seed(world, 1, 0, false);
        world.items[0].selected = true; world.items[1].selected = false; world.Apply();
        const auto source = internal_desktop_cases::Sources(world, 1);
        world.items[1].selected = true;
        world.Legal();
        const auto preview = Preview(world, source, {world.Named(L"Destination"), 1, 1});
        Require(preview.landings.size() == 1 && preview.landings[0].cell.column == 1 && preview.landings[0].cell.row == 1,
            "a captured single desktop source reaches its requested free cell without unrelated selection influence");
        CheckCommit(world, source, preview);
    }
    if (section == L"all" || section == L"occupied")
    {
        World world(root / L"occupied"); internal_desktop_cases::Seed(world, 1, 0, false);
        world.items[0].selected = true; world.items[1].selected = false;
        world.items[1].gridCell = {L"target", 1, 1}; world.records.at(world.items[1].layoutKey).cell = world.items[1].gridCell;
        world.Apply();
        const auto source = internal_desktop_cases::Sources(world, 1);
        world.items[1].selected = true;
        world.Legal(); const auto before = world.Snapshot();
        const auto preview = Preview(world, source, {world.Named(L"Destination"), 1, 1});
        Require(world.Snapshot() == before, "blocked selected desktop preview is nonmutating");
        if (!preview.Empty())
        {
            Require(world.Execute(source, preview), "an accepted selected desktop plan executes its real commit");
            const auto first = world.ItemIndex(source.entries[0].desktopKey);
            Require(world.items[first].gridCell.pageId != world.items[1].gridCell.pageId || world.items[first].gridCell.column != world.items[1].gridCell.column ||
                world.items[first].gridCell.row != world.items[1].gridCell.row,
                "captured-source exclusion never makes unrelated selected objects disappear from occupied cells");
            world.Legal(); world.Reload();
        }
    }
    if (section == L"all" || section == L"identity")
    {
        World world(root / L"identity"); internal_desktop_cases::Seed(world, 1, 0, false);
        world.items[0].selected = true; world.items[1].selected = false; world.Apply();
        const auto source = internal_desktop_cases::Sources(world, 1); std::swap(world.items[0], world.items[1]);
        const auto preview = Preview(world, source, {world.Named(L"Destination"), 1, 1});
        Require(preview.landings.size() == 1, "a captured desktop identity remains planned after source enumeration reorders");
        CheckCommit(world, source, preview);
    }
    std::cout << "selected desktop projection, occupancy, captured identity, actual commit and storage cases passed\n";
}
}
