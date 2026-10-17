#pragma once

#include "group_membership_cases.h"

namespace group_release_cases
{
using namespace group_membership_cases;

inline std::vector<GridPage> Pages()
{
    std::vector<GridPage> pages(2);
    pages[0].id = L"release-left"; pages[1].id = L"release-right";
    for (auto& page : pages) { page.columns = 8; page.rows = 8; }
    return pages;
}

inline bool ReleaseOne(Batch& batch, const std::wstring& id, GridCell cell, bool collection,
    const std::vector<GridPage>& pages, const grid_occupancy::Slots& occupied)
{
    return widget_group_drop::Release(batch.widgets, id,
        collection ? DesktopWidgetType::CollectionGroup : DesktopWidgetType::FileGroup,
        [&](size_t child, size_t owner) {
            auto used = occupied;
            // This layer receives the host's occupancy snapshot. Add children
            // already released by this request so multi-selection cannot overlap.
            for (const auto& widget : batch.widgets)
                if (std::none_of(batch.widgets.begin(), batch.widgets.end(), [&](const auto& group) {
                    return std::find(group.childWidgetIds.begin(), group.childWidgetIds.end(), widget.id) != group.childWidgetIds.end();
                })) grid_occupancy::Mark(used, widget.gridCell, widget.gridSpan);
            return widget_group_drop::PlanRelease(batch.widgets[child], cell, batch.widgets[owner].gridCell,
                pages, used, collection,
                [](const auto&, auto span, int, int) { return span; },
                [&](auto span, auto& slots, auto& result, const auto& page, int start) {
                    return grid_occupancy::FindFree(pages, {0, 1}, span, slots, result, page, start);
                });
        });
}

inline bool Execute(Harness& harness, Batch& batch, const std::vector<std::wstring>& ids,
    const std::vector<GridCell>& cells, const std::vector<GridPage>& pages, const grid_occupancy::Slots& occupied = {})
{
    const bool collection = batch.widgets[0].type == DesktopWidgetType::CollectionGroup;
    DragSourceList sources;
    sources.hasCollectionGroupEntries = collection; sources.hasFileGroupEntries = !collection;
    for (const auto& id : ids)
    {
        DragSourceEntry entry;
        entry.kind = collection ? DropSourceKind::CollectionGroupEntry : DropSourceKind::FileGroupEntry;
        entry.sourceIndex = sources.entries.size(); entry.widgetId = id;
        sources.entries.push_back(std::move(entry));
    }
    DropPreviewList preview;
    preview.targetKind = DropTargetKind::Desktop; preview.action = DropAction::Move;
    for (size_t index = 0; index < cells.size(); ++index)
    {
        DropLanding landing;
        landing.kind = DropLandingKind::DesktopCell; landing.sourceIndex = index; landing.cell = cells[index];
        preview.landings.push_back(std::move(landing));
    }
    if (preview.landings.size() > 1) std::reverse(preview.landings.begin(), preview.landings.end());
    int internal = 0, completed = 0;
    bool notified = false;
    batch.commitInternal = [&](const auto& list, const auto& plan) {
        ++internal;
        return widget_group_drop::ReleasePlan(list, plan, [&](const auto& id, auto cell, bool isCollection) {
            return ReleaseOne(batch, id, cell, isCollection, pages, occupied);
        });
    };
    const auto services = harness.PipelineServices(batch, [](const auto&, const auto&, auto, bool, auto) -> bool {
        throw std::runtime_error("group release cannot submit physical file operations");
    });
    const bool result = drop_pipeline::Execute(sources, preview, batch.widgets, batch.dock, services,
        [&](bool succeeded) { ++completed; notified = succeeded; }, true, batch.result);
    Require(internal == 1 && completed == 1 && notified == result && batch.executions == 0 &&
        batch.placements->empty() && batch.result->outputs.empty(),
        "group release dispatch completes one model operation without materializing any file");
    batch.commitInternal = {};
    return result;
}

inline void Preserved(const std::vector<DesktopWidget>& before, const std::vector<DesktopWidget>& after,
    const std::vector<std::wstring>& moved)
{
    auto normalized = after;
    for (size_t index = 0; index < before.size(); ++index)
        if (std::find(moved.begin(), moved.end(), before[index].id) != moved.end())
        {
            normalized[index].gridCell = before[index].gridCell;
            normalized[index].gridSpan = before[index].gridSpan;
        }
    UnchangedChildren(before, normalized);
}

inline void Run(const std::filesystem::path& root)
{
    Harness harness;
    const auto pages = Pages();
    size_t serial = 0;
    // U06/B10: release keeps the original child and its content, removes its
    // old label and uses the explicitly captured legal page/cell. Q05 group
    // dissolution and Q08 exact collision fallback are not prescribed here.
    for (auto type : {DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (int form : {0, 1, 2})
    for (const auto& page : pages)
    for (bool multiple : {false, true})
    {
        const auto path = root / (L"released-" + std::to_wstring(serial++));
        Batch batch; SeedWorld(batch, path, type, form, 0);
        batch.widgets[0].activeCategoryId = L"B";
        const auto before = batch.widgets;
        const auto dockBefore = batch.dock;
        const std::vector<std::wstring> moved = multiple ? std::vector<std::wstring>{L"B", L"D"} : std::vector<std::wstring>{L"B"};
        std::vector<GridCell> cells{{page.id, 2, 3}};
        if (multiple) cells.push_back({page.id, 5, 3});
        Require(Execute(harness, batch, moved, cells, pages) && batch.widgets[0].childWidgetIds ==
            (multiple ? std::vector<std::wstring>{L"A", L"C"} : std::vector<std::wstring>{L"A", L"C", L"D"}),
            "released labels leave only their original siblings in the group and preserve sibling order");
        Require(batch.widgets[1].childWidgetIds == std::vector<std::wstring>{L"X", L"Y"} &&
            batch.widgets[2].childWidgetIds == std::vector<std::wstring>{L"U"} && batch.dock.size() == dockBefore.size(),
            "releasing labels preserves unrelated groups and the source group's own Dock entrance");
        for (size_t index = 0; index < moved.size(); ++index)
        {
            const auto child = std::find_if(batch.widgets.begin(), batch.widgets.end(), [&](const auto& widget) { return widget.id == moved[index]; });
            Require(child != batch.widgets.end() && child->gridCell.pageId == page.id &&
                child->gridCell.column == cells[index].column && child->gridCell.row == cells[index].row &&
                child->gridSpan.columns == 2 && child->gridSpan.rows == 2,
                "released child preserves identity and footprint at its own captured screen and cell");
            Require(std::none_of(batch.widgets.begin(), batch.widgets.end(), [&](const auto& group) {
                return std::find(group.childWidgetIds.begin(), group.childWidgetIds.end(), moved[index]) != group.childWidgetIds.end();
            }), "released child no longer belongs to any group");
        }
        Require(std::find(batch.widgets[0].childWidgetIds.begin(), batch.widgets[0].childWidgetIds.end(),
            batch.widgets[0].activeCategoryId) != batch.widgets[0].childWidgetIds.end(),
            "releasing the active label leaves a remaining sibling accessible");
        Preserved(before, batch.widgets, moved); UnchangedFiles(path);
        CheckRoundTrip(path / L"layout.json", batch.widgets);
    }

    for (auto type : {DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (int form : {0, 1, 2})
    for (int invalid = 0; invalid < 6; ++invalid)
    {
        const auto path = root / (L"reject-" + std::to_wstring(serial++));
        Batch batch; SeedWorld(batch, path, type, form, 0);
        batch.widgets[0].activeCategoryId = L"B";
        grid_occupancy::Slots occupied;
        if (invalid == 0) for (const auto& page : pages) grid_occupancy::Mark(occupied, {page.id, 0, 0}, {8, 8});
        if (invalid == 1) batch.widgets.push_back(batch.widgets[4]);
        if (invalid == 2) batch.widgets[2].childWidgetIds.push_back(L"B");
        if (invalid == 3) batch.widgets[4].type = DesktopWidgetType::Guide;
        if (invalid == 4) std::erase(batch.widgets[0].childWidgetIds, L"B");
        const auto before = Scene(batch);
        Require(!Execute(harness, batch, {invalid == 5 ? L"missing" : L"B"}, {{L"release-left", 2, 3}}, pages, occupied) &&
            Scene(batch) == before,
            "rejected label release leaves membership, active label, selection, geometry and Dock unchanged");
        UnchangedFiles(path);
    }

    // FolderMapping labels enter a Widget-family host route. Exercise the
    // actual final Release transaction without pretending to cover that decoder.
    for (bool blocked : {false, true})
    {
        const auto path = root / (L"mapping-" + std::to_wstring(serial++));
        Batch batch; SeedWorld(batch, path, DesktopWidgetType::FileGroup, 0, 0);
        batch.widgets[4].type = DesktopWidgetType::FolderMapping;
        batch.widgets[4].sourceFolderPath = (path / L"B").wstring();
        const auto before = Scene(batch);
        const auto widgets = batch.widgets;
        grid_occupancy::Slots occupied;
        if (blocked) grid_occupancy::Mark(occupied, {L"release-left", 2, 3}, {2, 2});
        Require(ReleaseOne(batch, L"B", {L"release-left", 2, 3}, false, pages, occupied) == !blocked,
            "mapped folder release uses the same final transaction and exact landing collision decision");
        if (blocked) Require(Scene(batch) == before, "rejected mapped folder release does not cancel selection or detach its label");
        else
        {
            Require(batch.widgets[0].childWidgetIds == std::vector<std::wstring>{L"A", L"C", L"D"} &&
                batch.widgets[4].gridCell.pageId == L"release-left" && batch.widgets[4].gridCell.column == 2 && batch.widgets[4].gridCell.row == 3,
                "mapped release keeps the original directory mapping at the captured desktop landing");
            Preserved(widgets, batch.widgets, {L"B"}); CheckRoundTrip(path / L"layout.json", batch.widgets);
        }
        UnchangedFiles(path);
    }
    std::cout << "group release placement, rejection, original files and storage cases passed\n";
}
}
