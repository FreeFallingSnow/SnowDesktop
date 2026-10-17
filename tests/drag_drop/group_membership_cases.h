#pragma once

#include "placement_cases.h"
#include "widgets/widget_group_drop.h"

namespace group_membership_cases
{
using namespace drop_placement_cases;
inline const std::array<std::wstring, 7> childIds{L"A", L"B", L"C", L"D", L"X", L"Y", L"U"};

inline std::string Scene(const Batch& batch)
{
    std::ostringstream text;
    for (const auto& widget : batch.widgets)
    {
        widget_layout_codec::Write(text, widget,
            [](const auto& value) { return snowdesktop::json::EscapeString(Encode(value)); }, Encode);
        text << widget.selected;
    }
    for (const auto& entry : batch.dock)
        text << static_cast<int>(entry.type) << ':' << Encode(entry.reference) << ':' << entry.keepOnDesktop;
    return text.str();
}

inline void SeedWorld(Batch& batch, const std::filesystem::path& root,
    DesktopWidgetType groupType, int sourceForm, int targetForm)
{
    batch.widgets.resize(3 + childIds.size());
    for (size_t index = 0; index < 3; ++index)
    {
        auto& group = batch.widgets[index];
        group.id = L"G" + std::to_wstring(index + 1); group.type = groupType;
        group.title = L"Group " + group.id; group.privacyMode = true; group.listMode = true;
        group.gridCell = {L"screen-" + std::to_wstring(index), static_cast<int>(index), 2};
        group.gridSpan = {1, 1};
        const int form = index == 0 ? sourceForm : index == 1 ? targetForm : 0;
        group.singleCellIconMode = form == 1;
        if (form == 2)
        {
            group.gridCell.pageId = L"__dock";
            batch.dock.push_back({groupType == DesktopWidgetType::CollectionGroup
                ? DockEntryType::CollectionGroup : DockEntryType::FileGroup, group.id, false});
        }
    }
    batch.widgets[0].childWidgetIds = {L"A", L"B", L"C", L"D"}; batch.widgets[0].activeCategoryId = L"C";
    batch.widgets[1].childWidgetIds = {L"X", L"Y"}; batch.widgets[1].activeCategoryId = L"Y";
    batch.widgets[2].childWidgetIds = {L"U"}; batch.widgets[2].activeCategoryId = L"U";
    for (size_t index = 0; index < childIds.size(); ++index)
    {
        const auto directory = root / childIds[index];
        std::filesystem::create_directories(directory);
        std::ofstream(directory / L"keep.txt") << "unchanged-" << index;
        auto& child = batch.widgets[index + 3];
        child.id = childIds[index]; child.title = L"Child " + child.id;
        child.type = groupType == DesktopWidgetType::CollectionGroup ? DesktopWidgetType::Collection :
            index == 4 ? DesktopWidgetType::FolderMapping : DesktopWidgetType::FileCategories;
        if (child.type == DesktopWidgetType::FolderMapping) child.sourceFolderPath = directory.wstring();
        child.itemKeys = {(directory / L"keep.txt").wstring()};
        child.gridCell = {L"return-page", static_cast<int>(index), 5}; child.gridSpan = {2, 2};
        child.privacyMode = true; child.fanPopup = true; child.listMode = true; child.selected = true;
    }
}

inline void UnchangedFiles(const std::filesystem::path& root)
{
    Require(static_cast<size_t>(std::distance(std::filesystem::recursive_directory_iterator(root),
        std::filesystem::recursive_directory_iterator{})) == childIds.size() * 2,
        "logical group changes create no extra filesystem objects in the isolated source tree");
    for (size_t index = 0; index < childIds.size(); ++index)
    {
        const auto directory = root / childIds[index];
        Require(Bytes(directory / L"keep.txt") == "unchanged-" + std::to_string(index) &&
            std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == 1,
            "group-label operations preserve each child's original files and create no copies or shortcuts");
    }
}

inline void UnchangedChildren(const std::vector<DesktopWidget>& before, const std::vector<DesktopWidget>& after)
{
    Require(before.size() == after.size(), "group transfer retains every original component identity");
    for (size_t index = 3; index < before.size(); ++index)
    {
        const auto& a = before[index]; const auto& b = after[index];
        Require(a.id == b.id && a.type == b.type && a.title == b.title && a.itemKeys == b.itemKeys &&
            a.sourceFolderPath == b.sourceFolderPath && a.gridCell.pageId == b.gridCell.pageId &&
            a.gridCell.column == b.gridCell.column && a.gridCell.row == b.gridCell.row &&
            a.gridSpan.columns == b.gridSpan.columns && a.gridSpan.rows == b.gridSpan.rows &&
            a.privacyMode == b.privacyMode && a.listMode == b.listMode && a.fanPopup == b.fanPopup,
            "group transfer preserves child identity, contents, mapping path, return geometry and independent configuration");
    }
}

inline bool Execute(Harness& harness, Batch& batch, const std::vector<std::wstring>& moving,
    size_t targetIndex, size_t insertBefore, DesktopWidget* overrideTarget = nullptr, std::function<void()> beforeCommit = {})
{
    const bool collections = batch.widgets[0].type == DesktopWidgetType::CollectionGroup;
    DragSourceList sources;
    sources.hasOriginWidget = true; sources.originWidgetId = L"G1";
    sources.originWidgetType = batch.widgets[0].type;
    sources.hasOriginSurface = true;
    sources.originSurface = collections ? slot_contract::SlotSurfaceKind::CollectionGroup : slot_contract::SlotSurfaceKind::FileGroup;
    sources.hasCollectionGroupEntries = collections; sources.hasFileGroupEntries = !collections;
    for (const auto& id : moving)
    {
        DragSourceEntry entry;
        entry.kind = collections ? DropSourceKind::CollectionGroupEntry : DropSourceKind::FileGroupEntry;
        entry.widgetId = id; entry.sourceIndex = sources.entries.size();
        sources.entries.push_back(entry);
    }
    DropPreviewList preview;
    preview.targetKind = DropTargetKind::KeyedWidget; preview.action = DropAction::Move;
    preview.targetWidget = overrideTarget ? overrideTarget : &batch.widgets[targetIndex];
    preview.insertIndex = insertBefore;
    for (size_t index = 0; index < moving.size(); ++index)
    {
        DropLanding landing; landing.kind = DropLandingKind::WidgetIndex; landing.sourceIndex = index; landing.widget = preview.targetWidget; landing.widgetId = preview.targetWidget->id;
        preview.landings.push_back(landing);
    }
    if (beforeCommit) beforeCommit();
    int internal = 0, completed = 0;
    bool notified = false;
    batch.commitInternal = [&](const auto& list, const auto& plan) {
        ++internal;
        return widget_group_drop::ApplyPlan(batch.widgets, batch.dock, list, plan);
    };
    auto services = harness.PipelineServices(batch, [](const auto&, const auto&, auto, bool, auto) -> bool {
        throw std::runtime_error("group-label movement must never enter physical file execution");
    });
    const bool succeeded = drop_pipeline::Execute(sources, preview, batch.widgets, batch.dock, services,
        [&](bool value) { ++completed; notified = value; }, true, batch.result);
    Require(internal == (moving.empty() ? 0 : 1) && completed == internal && notified == succeeded &&
        batch.result->outputs.empty() && batch.placements->empty() && batch.executions == 0,
        "group-label dispatch executes one model transaction and never produces a physical file request");
    batch.commitInternal = {};
    return succeeded;
}

inline void Run(const std::filesystem::path& root)
{
    Harness harness;
    size_t serial = 0;
    struct Move { std::vector<std::wstring> ids; size_t boundary; std::vector<std::wstring> expected; };
    const Move reorders[]{
        {{L"B", L"D"}, 0, {L"B", L"D", L"A", L"C"}},
        {{L"B", L"D"}, 4, {L"A", L"C", L"B", L"D"}},
        {{L"B"}, 2, {L"A", L"B", L"C", L"D"}},
        {{L"C"}, 1, {L"A", L"C", L"B", L"D"}},
    };
    for (auto type : {DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (int form : {0, 1, 2})
    for (const auto& move : reorders)
    {
        const auto path = root / (L"same-" + std::to_wstring(serial++));
        Batch batch; SeedWorld(batch, path, type, form, 0);
        const auto before = batch.widgets;
        const auto dockBefore = batch.dock;
        Require(Execute(harness, batch, move.ids, 0, move.boundary) && batch.widgets[0].childWidgetIds == move.expected &&
            batch.widgets[0].activeCategoryId == L"C" && batch.widgets[1].childWidgetIds == std::vector<std::wstring>{L"X", L"Y"} &&
            batch.widgets[2].childWidgetIds == std::vector<std::wstring>{L"U"},
            "same-group label reorder follows the explicit insertion boundary and preserves unrelated order");
        Require(batch.dock.size() == dockBefore.size(), "reordering a group's labels retains the group's own Dock entrance");
        UnchangedChildren(before, batch.widgets); UnchangedFiles(path); CheckRoundTrip(path / L"layout.json", batch.widgets);
    }
    struct Transfer { std::vector<std::wstring> ids; size_t boundary; std::vector<std::wstring> source, target; };
    const Transfer transfers[]{
        {{L"B"}, 0, {L"A", L"C", L"D"}, {L"B", L"X", L"Y"}},
        {{L"B", L"D"}, 1, {L"A", L"C"}, {L"X", L"B", L"D", L"Y"}},
        {{L"A", L"B", L"C", L"D"}, 2, {}, {L"X", L"Y", L"A", L"B", L"C", L"D"}},
    };
    for (auto type : {DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (int sourceForm : {0, 1, 2})
    for (int targetForm : {0, 1, 2})
    for (const auto& move : transfers)
    {
        const auto path = root / (L"cross-" + std::to_wstring(serial++));
        Batch batch; SeedWorld(batch, path, type, sourceForm, targetForm);
        const auto before = batch.widgets;
        Require(Execute(harness, batch, move.ids, 1, move.boundary) && batch.widgets[0].childWidgetIds == move.source &&
            batch.widgets[1].childWidgetIds == move.target && batch.widgets[1].activeCategoryId == L"Y" &&
            batch.widgets[2].childWidgetIds == std::vector<std::wstring>{L"U"},
            "cross-group labels leave their original owner and retain selection order at the captured target boundary");
        for (const auto& id : childIds)
        {
            size_t owners = 0;
            for (size_t index = 0; index < 3; ++index)
                owners += static_cast<size_t>(std::count(batch.widgets[index].childWidgetIds.begin(), batch.widgets[index].childWidgetIds.end(), id));
            Require(owners == 1, "every original child has exactly one group owner after a transfer");
        }
        if (move.source.empty()) Require(batch.widgets[0].activeCategoryId.empty(), "an empty source group cannot retain a nonexistent active label");
        UnchangedChildren(before, batch.widgets); UnchangedFiles(path); CheckRoundTrip(path / L"layout.json", batch.widgets);
    }
    for (auto type : {DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (int invalid = 0; invalid < 7; ++invalid)
    {
        const auto path = root / (L"invalid-" + std::to_wstring(serial++));
        Batch batch; SeedWorld(batch, path, type, 0, 0);
        std::vector<std::wstring> moving{L"B"};
        if (invalid == 0) moving = {L"deleted-child"};
        if (invalid == 1) batch.widgets.push_back(batch.widgets[4]); // Duplicate B model identity.
        if (invalid == 2) batch.widgets[2].childWidgetIds.push_back(L"B");
        if (invalid == 3) batch.widgets[4].type = DesktopWidgetType::CollectionGroup;
        if (invalid == 4) batch.widgets.push_back(batch.widgets[1]); // Duplicate target identity.
        if (invalid == 6) moving.clear();
        DesktopWidget stale = batch.widgets[1];
        const auto before = Scene(batch);
        Require(!Execute(harness, batch, moving, 1, 1, invalid == 5 ? &stale : nullptr) && Scene(batch) == before,
            "invalid group-label source or target rejects without changing membership, order, selection or Dock data");
        UnchangedFiles(path);
    }
    // Captured identity remains decisive even when a live replacement occupies
    // the exact old address. The user selected G2, never replacement G4.
    for (auto type : {DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    {
        Batch batch; const auto path = root / (L"replaced-" + std::to_wstring(serial++));
        SeedWorld(batch, path, type, 0, 0);
        std::string before;
        Require(!Execute(harness, batch, {L"B"}, 1, 1, nullptr, [&] {
            batch.widgets[1].id = L"replacement-G4";
            before = Scene(batch);
        }) && Scene(batch) == before,
            "captured group identity cannot commit into a different group occupying the same address");
        UnchangedFiles(path);
    }
    // A repeated source identity may be rejected or applied idempotently, but
    // can never create repeated labels or multiple logical owners.
    {
        Batch batch; const auto path = root / L"repeated";
        SeedWorld(batch, path, DesktopWidgetType::CollectionGroup, 0, 0);
        const auto before = Scene(batch);
        if (Execute(harness, batch, {L"B", L"B"}, 1, 0))
            Require(batch.widgets[1].childWidgetIds == std::vector<std::wstring>{L"B", L"X", L"Y"} &&
                batch.widgets[0].childWidgetIds == std::vector<std::wstring>{L"A", L"C", L"D"},
                "repeated label identity cannot create duplicate members or retain a second source owner");
        else Require(Scene(batch) == before, "rejecting repeated labels leaves the model unchanged");
        UnchangedFiles(path);
    }
    std::cout << "group label reorder, transfer, identity, unchanged files and storage cases passed\n";
}
}
