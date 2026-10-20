#pragma once

#include "placement_cases.h"

namespace pipeline_decision_cases
{
using namespace drop_placement_cases;

inline std::vector<std::filesystem::path> Files(const std::filesystem::path& path, size_t count, bool program)
{
    auto files = Seed(path, count);
    if (program)
    {
        const auto executable = files[0].parent_path() / L"a.exe";
        std::filesystem::rename(files[0], executable);
        files[0] = executable; // Plain fixture bytes; never launched.
    }
    return files;
}

inline void Target(Batch& batch, DesktopWidgetType type)
{
    batch.widgets.resize(1);
    auto& target = batch.widgets[0];
    target.id = L"decision-target"; target.type = type;
    target.gridCell = {L"captured-screen", 3, 4}; target.gridSpan = {2, 3};
    target.title = L"User title"; target.userRenamed = true; target.privacyMode = true;
    target.showSearchBox = true;
    if (type == DesktopWidgetType::CollectionGroup || type == DesktopWidgetType::FileGroup)
        target.childWidgetIds = {L"already-missing-child"};
    external_file_destination::Destination captured;
    captured.widgetId = target.id; captured.widgetType = type;
    captured.preview.targetKind = DropTargetKind::KeyedWidget;
    captured.preview.insertIndex = 0; captured.preview.action = DropAction::Copy;
    batch.captured = captured;
    // Resolve from the live vector; modal callbacks may erase or relocate it.
    batch.resolveTarget = [&](const auto& id, bool) -> DesktopWidget* {
        const auto found = std::find_if(batch.widgets.begin(), batch.widgets.end(),
            [&](const auto& widget) { return widget.id == id; });
        return found == batch.widgets.end() ? nullptr : &*found;
    };
}

inline void UnchangedSources(const std::vector<std::filesystem::path>& files)
{
    Require(Bytes(files[0]) == "alpha" && (files.size() == 1 || Bytes(files[1] / L"b.txt") == "beta"),
        "confirmation, rejection and copying preserve every original source file");
}

inline void Membership(Batch& batch, const std::vector<std::filesystem::path>& files,
    const std::filesystem::path& layout)
{
    auto& queue = *batch.placements;
    Require(queue.size() == 1, "confirmed pipeline publishes exactly one real output snapshot");
    std::vector<DesktopItem> items(files.size());
    for (size_t index = 0; index < files.size(); ++index)
    {
        items[index].layoutKey = index ? L"FILE-B" : L"FILE-A";
        const auto output = std::find_if(batch.result->outputs.begin(), batch.result->outputs.end(),
            [&](const auto& value) { return value.source == files[index].wstring(); });
        Require(output != batch.result->outputs.end(), "confirmed selection has an actual physical output per source");
        items[index].parsingName = output->destination;
    }
    std::unordered_set<std::wstring> claimed;
    Require(pending_drop::ApplyEntries(queue[0], batch.widgets, items, claimed, Key,
        [](const auto&, const auto&) -> bool { throw std::runtime_error("confirmed files use exact outputs"); },
        [](const auto&, const auto&) { return true; },
        [](auto&, const auto&) -> bool { throw std::runtime_error("confirmed target cannot become a desktop landing"); },
        [](const auto&) {}), "confirmed files reach the captured logical target through real reconciliation");
    const std::vector<std::wstring> expected = files.size() == 1
        ? std::vector<std::wstring>{L"FILE-A"} : std::vector<std::wstring>{L"FILE-A", L"FILE-B"};
    Require(batch.widgets[0].itemKeys == expected && claimed.size() == files.size(),
        "confirmed files retain their selected order and unique ownership");
    CheckRoundTrip(layout, batch.widgets);
}

inline void Run(const std::filesystem::path& root)
{
    size_t serial = 0;
    // U13 explicitly promises program -> Collection, ordinary files ->
    // Desktop files, preserving the original component position and size.
    struct Payload { bool program; DesktopWidgetType converted; };
    const Payload payloads[]{{false, DesktopWidgetType::FileCategories}, {true, DesktopWidgetType::Collection}};
    for (auto group : {DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (const auto& payload : payloads)
    for (size_t count : {size_t{1}, size_t{2}})
    for (bool accepted : {true, false})
    for (bool asynchronous : {true, false})
    {
        const auto path = root / (L"conversion-" + std::to_wstring(serial++));
        const auto files = Files(path, count, payload.program);
        Batch batch; batch.source = MakeSource(files); batch.source->async = asynchronous;
        batch.destination = path / L"target"; batch.confirmConversion = accepted;
        Target(batch, group);
        batch.onConfirmation = [&](bool conversion) {
            Require(conversion && batch.widgets[0].type == group && batch.widgets[0].itemKeys.empty() &&
                batch.executions == 0 && std::filesystem::is_empty(batch.destination),
                "conversion prompt precedes model conversion, membership changes and all file operations");
            UnchangedSources(files);
        };
        {
            Harness harness;
            const DWORD expectedReturn = !asynchronous && !accepted ? DROPEFFECT_NONE : DROPEFFECT_COPY;
            Require(harness.Submit(batch, asynchronous) == expectedReturn, "conversion decision has the correct deferred or synchronous result");
            if (asynchronous) PumpUntil([&] { return batch.source->ends == 1; });
            Require(batch.conversionPrompts == 1 && batch.conversionPrograms == payload.program &&
                batch.programPrompts == 0 && !batch.programsEnabled && batch.programSaveAttempts == 0,
                "empty-group conversion chooses the promised content type without changing global program collection");
            if (accepted) AssertSuccess(batch, files, asynchronous);
        }
        auto& target = batch.widgets[0];
        Require(target.id == L"decision-target" && target.gridCell.pageId == L"captured-screen" &&
            target.gridCell.column == 3 && target.gridCell.row == 4 && target.gridSpan.columns == 2 &&
            target.gridSpan.rows == 3 && target.title == L"User title" && target.privacyMode && target.showSearchBox,
            "conversion and cancellation preserve original identity, page, position, size and independent configuration");
        if (accepted)
        {
            Require(target.type == payload.converted && target.childWidgetIds.empty() &&
                batch.viewsCreated == 1 && batch.viewsReleased == 1 && batch.layoutSaves > 0,
                "confirmed conversion creates the promised type and releases its temporary view after value capture");
            Membership(batch, files, path / L"layout.json");
        }
        else
            Require(target.type == group && target.childWidgetIds == std::vector<std::wstring>{L"already-missing-child"} &&
                target.itemKeys.empty() && batch.executions == 0 && batch.completed == 1 && !batch.success &&
                batch.viewsCreated == 0 && batch.layoutSaves == 0 && batch.result->outputs.empty() &&
                batch.placements->empty() && std::filesystem::is_empty(batch.destination),
                "cancelled conversion leaves type, children, layout, membership and physical files untouched");
        UnchangedSources(files);
    }

    struct ProgramCase { bool enabled, accept, saved, program, success; int prompts, attempts; };
    const ProgramCase programs[]{
        {false, false, true, true, false, 1, 0},
        {false, true, true, true, true, 1, 1},
        {false, true, false, true, false, 1, 1},
        {true, false, true, true, true, 0, 0},
        {false, false, true, false, true, 0, 0},
    };
    for (const auto& scenario : programs)
    for (size_t count : {size_t{1}, size_t{2}})
    for (bool asynchronous : {true, false})
    {
        const auto path = root / (L"program-" + std::to_wstring(serial++));
        const auto files = Files(path, count, scenario.program);
        Batch batch; batch.source = MakeSource(files); batch.source->async = asynchronous;
        batch.destination = path / L"target"; batch.programsEnabled = scenario.enabled;
        batch.confirmPrograms = scenario.accept; batch.programSaveAccepted = scenario.saved;
        Target(batch, DesktopWidgetType::FileCategories);
        batch.onConfirmation = [&](bool conversion) {
            Require(!conversion && batch.executions == 0 && batch.programSaveAttempts == 0 &&
                !batch.programsEnabled && std::filesystem::is_empty(batch.destination),
                "program confirmation precedes enabling settings and copying any selected member");
        };
        {
            Harness harness;
            const DWORD expectedReturn = !asynchronous && !scenario.success ? DROPEFFECT_NONE : DROPEFFECT_COPY;
            Require(harness.Submit(batch, asynchronous) == expectedReturn, "program opt-in determines the final synchronous result");
            if (asynchronous) PumpUntil([&] { return batch.source->ends == 1; });
            if (scenario.success) AssertSuccess(batch, files, asynchronous);
        }
        Require(batch.programPrompts == scenario.prompts && batch.programSaveAttempts == scenario.attempts &&
            batch.conversionPrompts == 0 && batch.programsEnabled == (scenario.enabled ||
                (scenario.program && scenario.accept && scenario.saved)),
            "program opt-in requires explicit acceptance and a successful settings save; ordinary files need neither");
        if (scenario.success) Membership(batch, files, path / L"layout.json");
        else Require(batch.executions == 0 && batch.completed == 1 && !batch.success &&
            batch.result->outputs.empty() && batch.placements->empty() && batch.widgets[0].itemKeys.empty() &&
            std::filesystem::is_empty(batch.destination),
            "cancelled or unsaved program opt-in cannot copy files or change target membership");
        UnchangedSources(files);
    }

    for (bool conversion : {false, true})
    for (int mutation = 0; mutation < 4; ++mutation)
    {
        const auto path = root / (L"modal-change-" + std::to_wstring(serial++));
        const auto files = Files(path, 2, !conversion);
        Batch batch; batch.source = MakeSource(files); batch.destination = path / L"target";
        batch.confirmConversion = true; batch.confirmPrograms = true;
        Target(batch, conversion ? DesktopWidgetType::CollectionGroup : DesktopWidgetType::FileCategories);
        batch.onConfirmation = [&](bool actualConversion) {
            Require(actualConversion == conversion, "controlled mutation occurs in the intended modal decision");
            if (mutation == 0) batch.widgets[0].type = DesktopWidgetType::FolderMapping;
            if (mutation == 1) batch.widgets.clear();
            if (mutation == 2) batch.widgets.reserve(batch.widgets.capacity() + 8);
            if (mutation == 3) batch.exiting = true;
        };
        Harness harness;
        Require(harness.Submit(batch) == DROPEFFECT_COPY, "modal target mutation follows asynchronous source admission");
        PumpUntil([&] { return batch.source->ends == 1; });
        Require(batch.completed == 1 && !batch.success && batch.source->outcome == E_ABORT &&
            batch.executions == 0 && batch.programSaveAttempts == 0 && !batch.programsEnabled &&
            batch.viewsCreated == 0 && batch.layoutSaves == 0 && batch.result->outputs.empty() &&
            batch.placements->empty() && std::filesystem::is_empty(batch.destination),
            "changed modal target cancels before enabling programs, converting models or executing files");
        UnchangedSources(files);
    }
    std::cout << "conversion and program decisions, live target checks, file effects and storage cases passed\n";
}
}
