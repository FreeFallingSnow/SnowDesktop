#pragma once

#include "dock_return_cases.h"
#include "drag_drop/desktop_drop_dispatch.h"
#include "drag_drop/right_drag_actions.h"

namespace dock_file_action_cases
{
using namespace drop_placement_cases;

inline DropPreviewList Preview(const DragSourceList& sources, int modifiers)
{
    // The host's admitted target and pointer-to-cell geometry are inputs.
    // Action conversion and all dispatch/physical commit code remain real.
    DropPreviewList preview; preview.targetKind = DropTargetKind::Desktop;
    preview.action = DropActionFromMods(modifiers, DropAction::Move);
    preview.fileBacked = preview.action == DropAction::Copy || preview.action == DropAction::Link;
    preview.anchorCell = {L"other-page", 2, 1};
    for (const auto& source : sources.entries)
    {
        DropLanding landing; landing.kind = DropLandingKind::DesktopCell;
        landing.sourceIndex = source.sourceIndex; landing.cell = preview.anchorCell;
        landing.cell.row += static_cast<int>(source.sourceIndex);
        preview.landings.push_back(std::move(landing));
    }
    return preview;
}

inline void OriginalFiles(const std::vector<std::pair<std::filesystem::path, std::string>>& files)
{
    for (const auto& [path, bytes] : files)
        Require(std::filesystem::exists(path) && Bytes(path) == bytes,
            "Dock desktop actions preserve every original physical file and byte");
}

inline void Run(const std::filesystem::path& root)
{
    struct Gesture { bool menu; int value; DropAction expected; bool cancelled; };
    // Independent expectations are the visible verbs, not conversions of the
    // production modifiers. Q01 combinations and Q02 components are separate.
    const Gesture gestures[]{
        {false, 0, DropAction::Move, false}, {false, MK_SHIFT, DropAction::Move, false},
        {false, MK_CONTROL, DropAction::Copy, false}, {false, MK_ALT, DropAction::Link, false},
        {true, 1, DropAction::Move, false}, {true, 2, DropAction::Copy, false},
        {true, 3, DropAction::Link, false}, {true, 4, DropAction::Move, true},
        {true, 0, DropAction::Move, true},
    };
    struct Pair { UINT command; DWORD modifiers; const char* label; widget_pair_drop::Action action; };
    size_t serial = 0;
    for (bool synchronous : {false, true})
    for (bool sameDirectory : {false, true})
    for (size_t count : {size_t{1}, size_t{2}})
    for (const auto& gesture : gestures)
    {
        dock_return_cases::World world(root / std::to_wstring(serial++));
        if (count == 2)
        {
            // A real file and directory, both backed by existing desktop IDs.
            std::filesystem::remove(world.items[1].parsingName);
            std::filesystem::create_directory(world.items[1].parsingName);
            std::ofstream(std::filesystem::path(world.items[1].parsingName) / L"entry.txt") << "B";
        }
        const auto originalTree = widget_displacement_cases::Files(world.root / L"source");
        std::vector<std::pair<std::filesystem::path, std::string>> originalFiles;
        for (const auto& [path, bytes] : originalTree)
            if (bytes != "<directory>") originalFiles.emplace_back(world.root / L"source" / path, bytes);
        Batch batch; batch.reverseOutputs = true;
        batch.destination = sameDirectory ? world.root / L"source" : world.root / L"target";
        std::filesystem::create_directories(batch.destination);
        const auto beforeDestination = widget_displacement_cases::Files(batch.destination);
        std::vector<DockEntry> additions;
        for (size_t index = 0; index < count; ++index)
            additions.push_back({DockEntryType::DesktopItem, world.items[index].layoutKey, false});
        Require(dock_drop_model::Pin({world.dock, world.widgets}, additions, 0, world.Services()), "seed actual exclusive Dock file entries");
        world.dock.push_back({DockEntryType::DesktopItem, L"UNCHANGED", true});
        for (const auto& item : world.items) batch.existingDesktopKeys.insert(item.layoutKey);
        const auto before = world.Snapshot();
        const auto beforeItems = world.ItemStates();
        const auto beforeDock = world.Order();
        DragSourceList sources; sources.hasDesktopIcons = true;
        sources.hasOriginSurface = true; sources.originSurface = slot_contract::SlotSurfaceKind::Dock;
        for (size_t index = 0; index < count; ++index)
        {
            const auto& item = world.items[index];
            DragSourceEntry entry; entry.kind = DropSourceKind::DesktopIcon;
            entry.sourceIndex = index; entry.desktopIndex = index; entry.desktopKey = item.layoutKey;
            entry.filePath = item.parsingName; entry.displayName = item.name;
            entry.dockReference = item.layoutKey; entry.fromDock = true;
            entry.originalCell = item.gridCell; entry.originalSpan = {1, 1};
            sources.entries.push_back(std::move(entry));
        }
        int modifiers = gesture.value;
        bool dispatch = true;
        if (gesture.menu)
        {
            right_drag_actions::Context context; context.allowedEffects = 7;
            context.itemTarget = true; context.sourceDock = true;
            context.sourceDesktopIcons = true; context.sourceHasPaths = true;
            context.preview = [&](DWORD mods) { return Preview(sources, static_cast<int>(mods)); };
            const auto choices = right_drag_actions::Build(context, std::vector<Pair>{});
            const std::array<const char*, 3> labels{"widget.base.move", "widget.base.copy_label", "widget.base.create_shortcut"};
            Require(choices.size() == 3, "ordinary exclusive Dock files offer move, copy and shortcut on an admitted desktop target");
            for (size_t index = 0; index < labels.size(); ++index)
            {
                const auto choice = right_drag_actions::Select(choices, static_cast<UINT>(index + 1));
                Require(choice && std::string(choice->label) == labels[index], "Dock file menu offers each bound action with its promised label");
            }
            const auto* choice = right_drag_actions::Select(choices, static_cast<UINT>(gesture.value));
            dispatch = choice != nullptr;
            if (choice) modifiers = static_cast<int>(right_drag_actions::SelectedKeys(*choice) & (MK_CONTROL | MK_SHIFT | MK_ALT));
        }
        int returns = 0, generic = 0;
        Harness harness;
        if (dispatch)
        {
            const auto handled = desktop_drop_dispatch::Dock(true, sources, modifiers, [&] {
                ++returns;
                std::vector<size_t> indices;
                for (size_t index = 0; index < count; ++index) indices.push_back(index);
                return world.Apply(world.Plan(std::move(indices), {L"other-page", 2, 1}));
            }, [&](const auto& selected, int mods) {
                ++generic;
                auto services = harness.PipelineServices(batch, harness.FileExecutor(batch));
                return drop_pipeline::Execute(selected, Preview(selected, mods), world.widgets, world.dock, services,
                    [&](bool success) { ++batch.completed; batch.success = success; }, synchronous, batch.result);
            });
            Require(handled.has_value() && *handled, "supported Dock desktop action is handled by the real dispatch boundary");
            if (generic && !synchronous) PumpUntil([&] { return batch.completed == 1; });
        }
        if (gesture.cancelled)
        {
            Require(!dispatch && returns == 0 && generic == 0 && batch.executions == 0 &&
                batch.completed == 0 && batch.result->outputs.empty() && world.Snapshot() == before &&
                widget_displacement_cases::Files(world.root / L"source") == originalTree &&
                widget_displacement_cases::Files(batch.destination) == beforeDestination,
                "cancelled or dismissed Dock file menu does not mutate files, entry ownership or desktop positions");
            continue;
        }
        OriginalFiles(originalFiles);
        if (gesture.expected == DropAction::Move)
        {
            Require(returns == 1 && generic == 0 && batch.executions == 0 && batch.result->outputs.empty() &&
                world.Order() == std::vector<std::wstring>{L"UNCHANGED"} &&
                dock_model_cases::SameCell(world.items[0].gridCell, {L"other-page", 2, 1}),
                "Dock Move returns existing identities to the selected desktop cell without a physical file move");
        }
        else
        {
            Require(returns == 0 && generic == 1 && batch.executions == 1 && batch.completed == 1 && batch.success &&
                batch.result->outputs.size() == count && world.Snapshot() == before && world.Order() == beforeDock,
                "explicit Dock file copy or shortcut executes one physical operation and keeps original Dock ownership");
            for (const auto& source : sources.entries)
            {
                const auto output = std::find_if(batch.result->outputs.begin(), batch.result->outputs.end(),
                    [&](const auto& value) { return value.source == source.filePath; });
                Require(output != batch.result->outputs.end() && !SamePath(output->destination, source.filePath) && std::filesystem::exists(output->destination),
                    "each selected Dock source has a distinct real output identity");
                if (gesture.expected == DropAction::Link)
                    Require(std::filesystem::equivalent(item_location::ResolveRevealPath(output->destination), source.filePath),
                        "Dock shortcut action produces a usable link to the retained original object");
                else
                {
                    const auto content = source.sourceIndex == 0 ? std::filesystem::path(output->destination) : std::filesystem::path(output->destination) / L"entry.txt";
                    Require(Bytes(content) == (source.sourceIndex == 0 ? "A" : "B"), "Dock Copy duplicates the selected file or directory bytes");
                }
            }
            Require(batch.placements->size() == 1, "Dock file output publishes exactly one captured placement transaction");
            for (const auto& output : batch.result->outputs)
            {
                DesktopItem item; item.parsingName = output.destination; item.layoutKey = Key(output.destination);
                item.name = std::filesystem::path(output.destination).filename().wstring(); world.items.push_back(std::move(item));
            }
            std::unordered_set<std::wstring> claimed;
            grid_occupancy::Slots used;
            for (size_t index = 0; index < beforeItems.size(); ++index)
                if (world.items[index].gridCell.pageId != kDockPageId)
                    grid_occupancy::Mark(used, world.items[index].gridCell, world.items[index].gridSpan);
            auto page = [&](const auto& id) -> const GridPage* {
                for (const auto& value : world.pages) if (value.id == id) return &value;
                return nullptr;
            };
            auto slot = [&](const auto& cell) {
                const auto* geometry = page(cell.pageId);
                Require(geometry != nullptr, "file placement uses a real captured page identity");
                return cell.column * geometry->rows + cell.row;
            };
            Require(pending_drop::ApplyEntries(batch.placements->front(), world.widgets, world.items, claimed, Key,
                [](const auto&, const auto&) -> bool { throw std::runtime_error("Dock file outputs must use actual identities"); },
                [](const auto&, const auto&) { return true; },
                [&](auto& item, const auto& landing) {
                    return pending_drop::PlaceDesktop(item, landing, used, page,
                        [&](auto span, const auto& marked, auto& cell, const auto& preferred, int start) {
                            return grid_occupancy::FindFree(world.pages, {0, 1}, span, marked, cell, preferred, start);
                        }, slot);
                }, [](const auto&) { throw std::runtime_error("desktop outputs cannot claim a component"); }),
                "real Dock file outputs enter the actual desktop placement transaction");
            for (const auto& source : sources.entries)
            {
                const auto output = std::find_if(batch.result->outputs.begin(), batch.result->outputs.end(), [&](const auto& value) { return value.source == source.filePath; });
                const auto item = std::find_if(world.items.begin(), world.items.end(), [&](const auto& value) { return SamePath(value.parsingName, output->destination); });
                Require(item != world.items.end() && dock_model_cases::SameCell(item->gridCell,
                    {L"other-page", 2, 1 + static_cast<int>(source.sourceIndex)}),
                    "each copied Dock file or shortcut occupies its own captured page and exact cell despite reverse receipts");
            }
        }
        for (size_t index = count; index < beforeItems.size(); ++index)
            Require(dock_model_cases::SameCell(world.items[index].gridCell, beforeItems[index].gridCell), "Dock file actions preserve unrelated desktop positions");
        world.Legal(); world.SaveReload(gesture.expected == DropAction::Move ? std::vector<std::wstring>{L"UNCHANGED"} : beforeDock);
    }
    std::cout << "Dock file modifiers, right-menu verbs, real file outputs, placement and storage cases passed\n";
}
}
