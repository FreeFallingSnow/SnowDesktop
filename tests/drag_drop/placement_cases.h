#pragma once

#include "async_slot_transfer_cases.h"
#include "drag_drop/pending_drop_placement.h"
#include "data/widget_layout_codec.h"
#include "common/json_string.h"
#include <sstream>

namespace drop_placement_cases
{
using namespace async_slot_transfer_cases;

inline std::string Encode(const std::wstring& value)
{
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(bytes), 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), bytes, nullptr, nullptr);
    return result;
}
inline std::wstring Decode(const std::string& value)
{
    if (value.empty()) return {};
    const int characters = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(characters), 0);
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), characters);
    return result;
}
inline std::wstring Key(std::wstring text)
{
    if (!text.empty()) LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
        text.data(), static_cast<int>(text.size()), text.data(), static_cast<int>(text.size()), nullptr, nullptr, 0);
    return text;
}
inline bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
inline bool NoLegacyMatch(const std::wstring&, const std::wstring&)
{ throw std::runtime_error("tracked file outputs must use exact paths rather than fuzzy names"); }

struct Context
{
    const wchar_t* name;
    DesktopWidgetType type;
    const wchar_t* page;
    bool icon, list, popup, grouped;
};
// Orthogonal representation factors remain separate. The producer creates the
// same file request for these already-admitted target snapshots; hit testing,
// admission and the desktop grid allocator are outside this execution layer.
inline const Context contexts[]{
    {L"collection-component", DesktopWidgetType::Collection, L"screen-2", false, false, false, false},
    {L"collection-icon", DesktopWidgetType::Collection, L"screen-2", true, false, true, false},
    {L"collection-dock", DesktopWidgetType::Collection, L"__dock", false, true, true, false},
    {L"collection-child", DesktopWidgetType::Collection, L"group-page", false, true, false, true},
    {L"desktop-files-component", DesktopWidgetType::FileCategories, L"screen-2", false, false, false, false},
    {L"desktop-files-dock", DesktopWidgetType::FileCategories, L"__dock", false, true, true, false},
    {L"mapped-folder-component", DesktopWidgetType::FolderMapping, L"screen-2", false, false, false, false},
    {L"mapped-folder-dock", DesktopWidgetType::FolderMapping, L"__dock", false, true, true, false},
};

inline layout_storage::Document RoundTrip(const std::filesystem::path& file,
    const std::vector<DesktopWidget>& widgets)
{
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"widgets\":[";
    bool first = true;
    for (const auto& widget : widgets)
    {
        if (!first) json << ',';
        first = false;
        widget_layout_codec::Write(json, widget,
            [](const std::wstring& value) { return snowdesktop::json::EscapeString(Encode(value)); }, Encode);
    }
    json << "]}";
    std::string error;
    Require(layout_storage::SaveDocument(file, json.str(), &error), "save real layout through production atomic storage");
    layout_storage::Document loaded;
    Require(layout_storage::LoadDocument(file, loaded).status == layout_storage::LoadStatus::LoadedPrimary,
        "load the actual persisted layout through production parsing");
    return loaded;
}

inline void CheckRoundTrip(const std::filesystem::path& path, const std::vector<DesktopWidget>& widgets)
{
    const auto loaded = RoundTrip(path, widgets);
    Require(loaded.widgets.size() == widgets.size(), "layout retains every affected and unrelated component");
    for (size_t index = 0; index < widgets.size(); ++index)
    {
        const auto& before = widgets[index];
        const auto& saved = loaded.widgets[index];
        DesktopWidget restored; restored.type = before.type;
        widget_layout_codec::RestorePlacement(saved, restored, Decode);
        widget_layout_codec::RestoreMembers(saved, restored, Decode, Key);
        Require(Decode(saved.id) == before.id && Decode(saved.type) == widget_layout_codec::TypeName(before.type) &&
            restored.gridCell.pageId == before.gridCell.pageId && restored.gridCell.column == before.gridCell.column &&
            restored.gridCell.row == before.gridCell.row && restored.itemKeys == before.itemKeys &&
            restored.childWidgetIds == before.childWidgetIds,
            "reload preserves component identity, page, exact position, member order and group children");
        Require(saved.singleCellIconMode == before.singleCellIconMode && saved.listMode == before.listMode &&
            saved.fanPopup == before.fanPopup && saved.sourceFolderPath == Encode(before.sourceFolderPath) &&
            saved.folderSortMode == before.folderSortMode && saved.privacyMode == before.privacyMode,
            "drop persistence preserves representation and unrelated folder/privacy settings");
    }
}

inline void Run(const std::filesystem::path& root)
{
    size_t serial = 0;
    for (const auto& context : contexts)
    for (DWORD effect : {DROPEFFECT_COPY, DROPEFFECT_MOVE, DROPEFFECT_LINK})
    for (size_t count : {size_t{1}, size_t{2}})
    {
        const auto path = root / (L"placement-" + std::to_wstring(serial++));
        const auto selected = Seed(path, count);
        Batch batch; batch.source = MakeSource(selected); batch.effect = effect; batch.destination = path / L"target";
        // Existing same-name file forces actual output identity to matter.
        const auto oldA = batch.destination / L"a.txt", oldB = batch.destination / L"keep.txt";
        std::ofstream(oldA) << "unrelated original";
        std::ofstream(oldB) << "keep";
        std::vector<DesktopWidget> widgets(context.grouped ? 3 : 2);
        auto& target = widgets[0];
        target.id = L"stable-target"; target.type = context.type;
        target.gridCell = {context.page, 3, 2}; target.gridSpan = {1, 1};
        target.singleCellIconMode = context.icon; target.listMode = context.list;
        target.fanPopup = context.popup; target.privacyMode = true;
        target.folderSortMode = folder_sort_rules::kManual;
        target.itemKeys = {L"K0", L"K1"};
        widgets[1].id = L"unrelated-auto-owner";
        widgets[1].type = DesktopWidgetType::Collection;
        if (context.grouped)
        {
            widgets[2].id = L"parent-group"; widgets[2].type = DesktopWidgetType::CollectionGroup;
            widgets[2].childWidgetIds = {target.id};
        }
        widgets[1].itemKeys = {L"UNRELATED", L"NEW-A"};
        if (count == 2) widgets[1].itemKeys.push_back(L"NEW-B");

        PendingLandingCache snapshot;
        if (context.type == DesktopWidgetType::FolderMapping)
        {
            target.sourceFolderPath = batch.destination.wstring();
            PendingFolderPlacement placement;
            placement.widgetId = target.id;
            placement.sourceFolderPath = target.sourceFolderPath;
            placement.insertIndex = 1;
            placement.existingPaths = {Key(oldA.wstring()), Key(oldB.wstring())};
            for (const auto& source : selected) placement.sourcePaths.push_back(source.wstring());
            snapshot.folderPlacements.push_back(placement);
        }
        else
        {
            snapshot.existingDesktopKeys = {L"K0", L"K1", L"UNRELATED"};
            for (size_t index = 0; index < count; ++index)
            {
                PendingLandingEntry entry;
                entry.sourceIndex = index; entry.sourcePath = selected[index].wstring();
                entry.sourceName = selected[index].filename().wstring();
                entry.kind = DropLandingKind::WidgetIndex; entry.widgetId = target.id;
                entry.insertIndex = 1 + index;
                snapshot.entries.push_back(entry);
            }
        }
        pending_drop::Queue queue;
        batch.reconcile = [&, snapshot](const auto& outputs) {
            auto reordered = outputs;
            std::reverse(reordered.outputs.begin(), reordered.outputs.end());
            pending_drop::Complete(queue, snapshot, reordered);
        };
        {
            Harness harness;
            Require(harness.Submit(batch) == effect, "model-bound transfer accepts its captured operation");
            PumpUntil([&] { return batch.source->ends == 1; });
            AssertSuccess(batch, selected);
        }
        Require(Bytes(oldA) == "unrelated original" && Bytes(oldB) == "keep" && queue.size() == 1,
            "collision handling preserves existing files and publishes only this operation's outputs");
        auto& cache = queue.front();
        if (context.type == DesktopWidgetType::FolderMapping)
        {
            // Fresh enumeration is an input boundary. Put it in reverse order
            // so the persisted insertion result cannot just mirror enumeration.
            target.folderEntries.clear();
            for (const auto& file : std::filesystem::directory_iterator(batch.destination))
            {
                FolderEntry entry; entry.fullPath = file.path().wstring(); entry.name = file.path().filename().wstring();
                target.folderEntries.push_back(std::move(entry));
            }
            std::sort(target.folderEntries.begin(), target.folderEntries.end(), [](const auto& a, const auto& b) {
                return a.fullPath > b.fullPath;
            });
            // Preserve the user's old manual order before reconciling new files.
            auto moveExisting = [&](const auto& name, size_t index) {
                auto found = std::find_if(target.folderEntries.begin(), target.folderEntries.end(),
                    [&](const auto& entry) { return entry.fullPath == name.wstring(); });
                auto entry = std::move(*found); target.folderEntries.erase(found);
                target.folderEntries.insert(target.folderEntries.begin() + static_cast<ptrdiff_t>(index), std::move(entry));
            };
            moveExisting(oldA, 0); moveExisting(oldB, 1);
            Require(pending_drop::ApplyFolder(cache, target, target.id, {}, NoLegacyMatch, Key, SamePath),
                "real mapped-folder reconciliation consumes exact operation outputs");
            std::vector<std::wstring> expected{oldA.wstring()};
            for (const auto& source : selected)
            {
                const auto output = std::find_if(batch.result->outputs.begin(), batch.result->outputs.end(),
                    [&](const auto& value) { return value.source == source.wstring(); });
                Require(output != batch.result->outputs.end(), "every selected folder member has an output");
                expected.push_back(output->destination);
            }
            expected.push_back(oldB.wstring());
            Require(target.itemKeys == expected && target.folderEntries.size() == count + 2 && !cache.active,
                "mapped files appear at the original insertion boundary with old members in their old order");
        }
        else
        {
            std::vector<DesktopItem> items(count + 1);
            items[0].layoutKey = L"UNRELATED"; items[0].name = L"a.txt"; items[0].parsingName = oldA.wstring();
            // Deliberately reverse item enumeration relative to captured source order.
            for (size_t index = 0; index < count; ++index)
            {
                auto& item = items[count - index];
                item.layoutKey = index ? L"NEW-B" : L"NEW-A";
                const auto output = std::find_if(batch.result->outputs.begin(), batch.result->outputs.end(),
                    [&](const auto& value) { return value.source == selected[index].wstring(); });
                Require(output != batch.result->outputs.end(), "each selected source has a real output before model refresh");
                item.parsingName = output->destination;
            }
            std::unordered_set<std::wstring> claimed;
            int invalidated = 0;
            Require(pending_drop::ApplyEntries(cache, widgets, items, claimed, Key,
                [](const DesktopItem&, const std::wstring&) -> bool {
                    throw std::runtime_error("tracked output must not fall back to same-name matching");
                }, [](const auto&, const auto&) { return true; },
                [](auto&, const auto&) -> bool { throw std::runtime_error("keyed drop must not use desktop allocator"); },
                [&](const auto& id) { Require(id == L"stable-target", "invalidate only the committed target"); ++invalidated; }),
                "freshly enumerated files are reconciled by the production keyed placement path");
            const std::vector<std::wstring> expected = count == 1
                ? std::vector<std::wstring>{L"K0", L"NEW-A", L"K1"}
                : std::vector<std::wstring>{L"K0", L"NEW-A", L"NEW-B", L"K1"};
            Require(target.itemKeys == expected && widgets[1].itemKeys == std::vector<std::wstring>{L"UNRELATED"} &&
                invalidated == static_cast<int>(count) && claimed.size() == count && !cache.active,
                "exact output identity restores membership and original multi-selection order without stealing namesakes");
            for (size_t index = 1; index < items.size(); ++index)
                Require(items[index].gridCell.pageId == target.gridCell.pageId && items[index].gridCell.column == 3 &&
                    items[index].gridCell.row == 2, "new members retain their requested component page and cell");
        }
        CheckRoundTrip(path / L"layout.json", widgets);
    }
    // Rejection is a valid matrix outcome: it must not move the item to the
    // target cell, claim its identity or remove any provisional owner.
    for (int reason = 0; reason < 3; ++reason)
    {
        std::vector<DesktopWidget> widgets(1);
        widgets[0].id = L"target"; widgets[0].gridCell = {L"target-page", 3, 2};
        widgets[0].itemKeys = {L"OLD"};
        std::vector<DesktopItem> items(1);
        items[0].layoutKey = L"NEW"; items[0].name = L"same.txt";
        items[0].parsingName = reason == 2 ? L"C:\\unrelated\\same.txt" : L"C:\\actual\\same.txt";
        items[0].gridCell = {L"original-page", 1, 1};
        PendingLandingCache cache; cache.active = true;
        PendingLandingEntry landing; landing.kind = DropLandingKind::WidgetIndex;
        landing.widgetId = reason == 1 ? L"removed-target" : L"target";
        landing.createdPath = L"C:\\actual\\same.txt";
        cache.entries.push_back(landing);
        std::unordered_set<std::wstring> claimed;
        const bool changed = pending_drop::ApplyEntries(cache, widgets, items, claimed, Key,
            [](const auto&, const auto&) -> bool { throw std::runtime_error("tracked path must remain exact"); },
            [](const auto&, const auto&) { return false; },
            [](auto&, const auto&) -> bool { throw std::runtime_error("rejection must not allocate a desktop cell"); },
            [](const auto&) { throw std::runtime_error("rejection must not invalidate committed slots"); });
        Require(!changed && claimed.empty() && widgets[0].itemKeys == std::vector<std::wstring>{L"OLD"} &&
            items[0].gridCell.pageId == L"original-page" && items[0].gridCell.column == 1,
            "rejected, deleted and namesake targets leave placement and ownership unchanged");
    }
    // Independent cell invariants: clear preview, occupied preview,
    // resized page, fallback monitor, and full-page rejection. These execute
    // the same occupancy/search/placement functions called by DesktopApp.
    for (int scenario = 0; scenario < 5; ++scenario)
    {
        std::vector<GridPage> pages(2);
        pages[0].id = L"page-a"; pages[0].columns = 2; pages[0].rows = 2;
        pages[1].id = L"page-b"; pages[1].columns = 2; pages[1].rows = 2;
        grid_occupancy::Slots used;
        if (scenario == 1) grid_occupancy::Mark(used, {L"page-a", 1, 1}, {1, 1});
        if (scenario >= 3) grid_occupancy::Mark(used, {L"page-a", 0, 0}, {2, 2});
        if (scenario == 4) grid_occupancy::Mark(used, {L"page-b", 0, 0}, {2, 2});
        const auto occupiedBefore = used;
        DesktopItem item; item.gridCell = {L"unplaced", 0, 0};
        PendingLandingEntry landing; landing.cell = {L"page-a", scenario == 2 ? 2 : 1, 1};
        const bool placed = pending_drop::PlaceDesktop(item, landing, used,
            [&](const auto& id) -> std::optional<GridPage> {
                for (const auto& page : pages) if (page.id == id) return page;
                return std::nullopt;
            }, [&](GridSpan span, const auto& slots, GridCell& found, const auto& preferred, int start) {
                return grid_occupancy::FindFree(pages, {1, 0}, span, slots, found, preferred, start);
            }, [](const GridCell& cell) { return cell.column * 2 + cell.row; });
        if (scenario == 4)
            Require(!placed && item.gridCell.pageId == L"unplaced" && !item.selected,
                "full pages reject allocation without publishing a fake selected position");
        else
        {
            const auto page = std::find_if(pages.begin(), pages.end(),
                [&](const auto& value) { return value.id == item.gridCell.pageId; });
            Require(placed && page != pages.end() && grid_occupancy::Fits(*page, item.gridCell, item.gridSpan) &&
                !grid_occupancy::Occupied(occupiedBefore, item.gridCell, item.gridSpan) && item.selected,
                "desktop allocation remains in bounds and avoids every preexisting occupied cell");
            if (scenario == 0)
                Require(item.gridCell.pageId == L"page-a" && item.gridCell.column == 1 && item.gridCell.row == 1,
                    "an available preview is the actual selected landing");
        }
    }
    std::cout << "model-bound async output, insertion, identity and storage cases passed\n";
}
}
