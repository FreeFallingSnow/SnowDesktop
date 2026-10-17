#pragma once

#include "page_mapping_cases.h"
#include "drag_drop/desktop_landings.h"
#include "drag_drop/desktop_output_placement.h"
#include "layout/item_location.h"

namespace file_page_cases
{
using namespace drop_placement_cases;
using World = page_mapping_cases::World;

inline desktop_output_placement::Services Services(World& world)
{
    desktop_output_placement::Services services;
    services.reference = [&]() -> const GridPage* {
        return world.order.empty() || world.pages.empty() ? nullptr : &world.pages[world.order.back()];
    };
    services.findFree = [&](auto span, auto& used, auto& cell, const auto& preferred, int slot) {
        return grid_occupancy::FindFree(world.pages, world.order, span, used, cell, preferred, slot);
    };
    services.slot = [&](const auto& cell) { return widget_displacement::Slot(world.pages, cell); };
    return services;
}

inline DropPreviewList Preview(World& world, const GridCell& request, size_t count)
{
    DragSourceList sources; sources.hasExternalFiles = true;
    for (size_t index = 0; index < count; ++index)
    {
        DragSourceEntry entry; entry.kind = DropSourceKind::ExternalFile; entry.sourceIndex = index;
        sources.entries.push_back(std::move(entry));
    }
    const auto* page = widget_displacement::FindPage(world.pages, request.pageId);
    Require(page != nullptr, "external file preview resolves its configured target page");
    desktop_landings::Services services;
    services.grouped = [&](const auto& widget) { return world.Grouped(widget); };
    services.owned = [&](const auto& item) { return world.Owned(item); };
    services.normalize = Key;
    services.findFree = [&](auto span, auto& used, auto& cell, const auto& preferred, int slot) {
        return grid_occupancy::FindFree(world.pages, world.order, span, used, cell, preferred, slot);
    };
    services.generateId = [&](const auto& reserved) {
        auto ids = world.savedIds; ids.insert(ids.end(), reserved.begin(), reserved.end());
        return page_mapping::GenerateId(ids);
    };
    DropPreviewList preview; preview.targetKind = DropTargetKind::Desktop; preview.fileBacked = true; preview.anchorCell = request;
    preview.landings = desktop_landings::Build(sources, request, false, *page, world.widgets, world.items, world.columns, world.rows, services);
    return preview;
}

inline void Run(const std::filesystem::path& root, std::wstring_view section = L"all")
{
    Require(section == L"all" || section == L"files" || section == L"rejection", "file page diagnostics select a known nonempty section");
    size_t serial = 0;
    if (section == L"all" || section == L"files")
    for (bool async : {false, true})
    for (DWORD effect : {DROPEFFECT_COPY, DROPEFFECT_MOVE, DROPEFFECT_LINK})
    for (size_t count : {size_t{2}, size_t{1}})
    for (bool reverse : {false, true})
    for (bool horizontal : {false, true})
    for (bool differentReference : {true, false})
    {
        World world(root / std::to_wstring(serial++), 2, reverse);
        const GridSpan grid = horizontal ? GridSpan{6, 1} : GridSpan{1, 5};
        const GridSpan reference = differentReference ? (horizontal ? GridSpan{1, 5} : GridSpan{6, 1}) : grid;
        world.columns[L"fixed"] = grid.columns; world.rows[L"fixed"] = grid.rows;
        world.columns[L"target"] = reference.columns; world.rows[L"target"] = reference.rows;
        for (const auto& id : {std::wstring(L"fixed"), std::wstring(L"target")})
        {
            DesktopWidget filler; filler.id = L"filler-" + id; filler.type = DesktopWidgetType::Collection;
            filler.gridCell = {id, 0, 0}; filler.gridSpan = {world.columns.at(id), world.rows.at(id)};
            world.widgets.push_back(std::move(filler));
        }
        world.Apply(); world.Legal();
        const GridCell request{world.Named(L"Fixed"), 0, 0};
        std::filesystem::create_directories(world.root / L"input");
        std::vector<std::filesystem::path> paths{world.root / L"input" / L"A.txt"};
        std::ofstream(paths[0]) << "original A";
        if (count == 2)
        {
            paths.push_back(world.root / L"input" / L"folder");
            std::filesystem::create_directory(paths.back());
            std::ofstream(paths.back() / L"B.txt") << "original B";
        }
        Batch batch; batch.source = MakeSource(paths); batch.source->async = async; batch.effect = effect; batch.reverseOutputs = true;
        batch.destination = world.root / L"output"; std::filesystem::create_directory(batch.destination);
        external_file_destination::Destination destination;
        destination.preview.targetKind = DropTargetKind::Desktop; destination.preview.anchorCell = request;
        destination.preview.action = effect == DROPEFFECT_MOVE ? DropAction::Move : effect == DROPEFFECT_LINK ? DropAction::Link : DropAction::Copy;
        batch.captured = destination;
        const auto before = world.Snapshot();
        batch.desktopPreview = [&](const auto& anchor, size_t size) { return Preview(world, anchor, size); };
        DropPreviewList captured;
        DragSourceList actualSources;
        batch.capturePlan = [&](const auto& sources, const auto& preview) {
            captured = preview; actualSources = sources;
            for (const auto& entry : sources.entries)
                Require(std::any_of(paths.begin(), paths.end(), [&](const auto& path) { return std::filesystem::equivalent(entry.filePath, path); }),
                    "the marshaled source paths identify exactly the selected real files");
        };
        Harness harness;
        const DWORD reportedEffect = !async && effect == DROPEFFECT_MOVE ? DROPEFFECT_NONE : effect;
        Require(harness.Submit(batch, async) == reportedEffect, "file page scenario preserves the selected operation and optimized synchronous move reporting");
        PumpUntil([&] { return batch.completed == 1 && (!async || batch.source->ends == 1); });
        Require(batch.success && batch.executions == 1 && batch.result->outputs.size() == count &&
            batch.placements->size() == 1 && world.Snapshot() == before,
            "real file completion retains one captured placement without mutating page state during preview");
        Require(captured.landings.size() == count, "the unchanged file scene publishes one accepted landing for every selected source");
        for (const auto& entry : actualSources.entries)
        {
            const std::filesystem::path path(entry.filePath);
            const auto output = std::find_if(batch.result->outputs.begin(), batch.result->outputs.end(),
                [&](const auto& value) { return SamePath(value.source, path.wstring()); });
            Require(output != batch.result->outputs.end() && std::filesystem::exists(output->destination), "every physical source has one real operation output");
            Require(std::filesystem::exists(path) == (effect != DROPEFFECT_MOVE), "file page execution preserves sources for Copy/Link and removes them for Move");
            if (effect == DROPEFFECT_LINK)
                Require(std::filesystem::equivalent(item_location::ResolveRevealPath(output->destination), path), "page recovery retains a real shortcut pointing to its original file or directory");
            else
            {
                const auto copied = path.extension() == L".txt" ? std::filesystem::path(output->destination) : std::filesystem::path(output->destination) / L"B.txt";
                Require(Bytes(copied) == (path.extension() == L".txt" ? "original A" : "original B"), "page recovery retains the real copied or moved bytes");
            }
        }
        // Fresh Shell enumeration is an input, never a supplied final cell.
        for (const auto& output : std::filesystem::directory_iterator(batch.destination))
        {
            DesktopItem item; item.name = output.path().filename().wstring(); item.parsingName = output.path().wstring();
            item.layoutKey = Key(item.parsingName); item.gridCell = {world.pages[world.order.front()].id, 0, 0}; item.gridSpan = {1, 1};
            world.items.push_back(std::move(item));
        }
        std::reverse(world.items.begin(), world.items.end());
        auto& cache = batch.placements->front(); std::unordered_set<std::wstring> claimed;
        auto used = desktop_output_placement::Used(world.widgets, world.items, claimed, cache.existingDesktopKeys, Key,
            [&](const auto& widget) { return world.Grouped(widget); }, [&](const auto& item) { return world.Owned(item); });
        const auto services = Services(world);
        const bool changed = pending_drop::ApplyEntries(cache, world.widgets, world.items, claimed, Key,
            [](const DesktopItem&, const std::wstring&) -> bool { throw std::runtime_error("tracked file outputs cannot use a legacy name match"); },
            [](const auto&, const auto&) { return true; }, [&](auto& item, const auto& landing) {
                return desktop_output_placement::Place({world.savedIds, world.columns, world.rows, world.pages}, item, landing, used, services);
            }, [](const auto&) { throw std::runtime_error("desktop output cannot claim a logical component"); });
        Require(changed && claimed.size() == count && !cache.active,
            "completed file outputs fulfill each captured page cell despite reversed receipts");
        for (size_t index = 0; index < paths.size(); ++index)
        {
            const auto output = std::find_if(batch.result->outputs.begin(), batch.result->outputs.end(),
                [&](const auto& value) { return SamePath(value.source, actualSources.entries[index].filePath); });
            const auto item = std::find_if(world.items.begin(), world.items.end(), [&](const auto& value) { return SamePath(value.parsingName, output->destination); });
            const auto landing = std::find_if(captured.landings.begin(), captured.landings.end(), [&](const auto& value) { return value.sourceIndex == index; });
            Require(item != world.items.end() && landing != captured.landings.end() && item->gridCell.pageId == landing->cell.pageId &&
                item->gridCell.column == landing->cell.column && item->gridCell.row == landing->cell.row,
                "completed file outputs fulfill each captured page cell despite reversed receipts");
        }
        world.Legal(); world.Save(); world.Reload();
    }
    if (section == L"all" || section == L"rejection")
    {
        World world(root / L"missing-monitor", 1, false); world.Apply(); world.pages.clear(); world.order.clear();
        world.Item(L"already-created.txt", {}, {1, 1});
        PendingLandingEntry landing; landing.kind = DropLandingKind::DesktopCell;
        landing.cell = {page_mapping::GenerateId(world.savedIds), 0, 0};
        const auto before = world.Snapshot(); const auto files = widget_displacement_cases::Files(world.root / L"sources");
        grid_occupancy::Slots used;
        Require(!desktop_output_placement::Place({world.savedIds, world.columns, world.rows, world.pages}, world.items[0], landing, used, Services(world)) &&
            world.Snapshot() == before && used.empty() && widget_displacement_cases::Files(world.root / L"sources") == files,
            "unavailable page geometry rejects output placement without publishing an incomplete page or changing the real output");
    }
    if (section == L"all" || section == L"rejection")
    for (bool occupied : {false, true})
    {
        World world(root / (occupied ? L"unfit-full" : L"unfit-free"), 1, false); world.Apply();
        world.Item(L"large-output.txt", {world.pages[0].id, 0, 0}, {2, 2});
        world.items[0].largeIcon.emplace(); world.items[0].largeIcon->columns = 2; world.items[0].largeIcon->rows = 2;
        if (occupied)
        {
            DesktopWidget filler; filler.id = L"full"; filler.type = DesktopWidgetType::Collection;
            filler.gridCell = {world.pages[0].id, 0, 0}; filler.gridSpan = {world.pages[0].columns, world.pages[0].rows};
            world.widgets.push_back(std::move(filler));
        }
        PendingLandingEntry landing; landing.kind = DropLandingKind::DesktopCell;
        landing.cell = {page_mapping::GenerateId(world.savedIds), 0, 0}; landing.plannedPageSize = GridSpan{1, 1};
        const auto before = world.Snapshot(); const auto files = widget_displacement_cases::Files(world.root / L"sources");
        auto used = desktop_output_placement::Used(world.widgets, world.items, {}, {}, Key,
            [&](const auto& widget) { return world.Grouped(widget); }, [&](const auto& item) { return world.Owned(item); });
        const bool placed = desktop_output_placement::Place({world.savedIds, world.columns, world.rows, world.pages}, world.items[0], landing, used, Services(world));
        Require(std::find(world.savedIds.begin(), world.savedIds.end(), landing.cell.pageId) == world.savedIds.end() &&
            !world.columns.contains(landing.cell.pageId) && !world.rows.contains(landing.cell.pageId) &&
            widget_displacement_cases::Files(world.root / L"sources") == files && (placed || world.Snapshot() == before),
            "an unfit planned output page is never published after rejection or fallback to an existing page");
        if (placed) world.Legal();
    }
    std::cout << "real file completion, captured page cells, page registration and layout storage cases passed\n";
}
}
