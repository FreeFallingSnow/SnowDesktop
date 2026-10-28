#pragma once
#include "common/l10n.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace native_group_folder_search_cases
{
using namespace native_drag_commit_cases;
namespace pair = native_widget_pair_cases;
inline void Run()
{
    // Retain the production scope builder, asynchronous coalescer, view, slots,
    // selection projection, clipboard path and drag capture. Replace only SDK
    // answers: these tests need no installed/running Everything or desktop UI.
    Profile profile;
    size_t scenes = 0;
    for (const bool list : {false, true})
    for (int surface = 0; surface < 3; ++surface)
    {
        context = "native folder search surface=" + std::to_string(surface) + " list=" + std::to_string(list);
        const auto root = profile.root / (L"search-" + std::to_wstring(scenes++));
        const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
        const auto first = desktop / L"project A";
        const auto second = desktop / L"project B";
        const auto mapped = root / L"mapped folder";
        fs::create_directories(first / L"sub"); fs::create_directories(second);
        fs::create_directories(mapped / L"sub");
        const auto direct = desktop / L"direct-needle.txt";
        const auto mappedDirect = mapped / L"mapped-needle.txt";
        const auto a = first / L"a-needle.txt", b = first / L"sub" / L"b-needle.txt";
        const auto c = second / L"c-needle.txt", d = mapped / L"sub" / L"d-needle.txt";
        for (const auto& path : {direct, mappedDirect, a, b, c, d}) { std::ofstream out(path); out << "fixture"; }
        auto& app = profile.Model(false, root / L"layout");
        if (surface == 2) Access::SeedSearchTypeSurface(app, {direct, first, second}, mapped, list);
        else Access::SeedFilterSurface(app, surface == 0 ? std::vector<fs::path>{direct, first, second}
            : std::vector<fs::path>{mappedDirect}, surface == 0 ? DesktopWidgetType::FileCategories : DesktopWidgetType::FolderMapping, list, false);
        auto* view = dynamic_cast<ScrollingItemWidget*>(Access::MemberContainer(app, surface == 2 ? L"ordered-group" : L"ordered"));
        Require(view != nullptr, "native source must be a real scrolling component");
        const auto original = Access::Widgets(app);
        auto queries = std::make_shared<std::vector<std::wstring>>();
        const std::vector<fs::path> answers{direct, mappedDirect, a, b, c, d, a,
            fs::path(first.wstring() + L"2") / L"outside-needle.txt"};
        Access::SetFolderSearchBoundary(*view, [queries, answers](const std::wstring& query, DWORD) {
            queries->push_back(query);
            snowdesktop::QuickNavigationEverythingSearchResponse response;
            for (const auto& path : answers) response.results.push_back({path.filename().wstring(), path.wstring(), {}, false});
            return response;
        });
        view->SetSearchText(L"needle"); view->EnsureFolderSearch();
        native_drag_async_reload_cases::Wait([&] { return !Access::FolderSearchPending(*view); }, "scoped worker must complete without UI-thread SDK work");
        Require(view->UsesFolderSearch(), "successful Everything boundary activates native scoped results");
        const size_t expected = surface == 0 ? 4u : surface == 1 ? 2u : 6u;
        Require(view->GetSlotCount() == expected, "preserve direct matches, include descendants in every root, deduplicate, and reject sibling-prefix/out-of-scope results");
        const auto& sections = Access::FolderSearchSections(*view);
        Require(sections.size() == (surface == 2 ? 4u : 2u), "desktop roots share one recursive section; file-group projects each retain their own headers");
        Require(!sections.back().title.empty(), "recursive results have a source title");
        Require(queries->size() == (surface == 2 ? 2u : 1u), "file-group sources are queried separately");
        for (const auto& query : *queries)
            Require(query.find(L"\\\"") != std::wstring::npos && query.find(L"> <needle>") != std::wstring::npos,
                "scoped query quotes spaced paths, retains a directory boundary, and groups the user expression");
        const size_t recursive = sections.back().first;
        const RECT directCell = view->GetMemberLayoutRect(0), recursiveCell = view->GetMemberLayoutRect(recursive);
        Require(recursiveCell.top > directCell.bottom, "section headers reserve geometry between direct and recursive results");
        auto* selected = view->GetMemberItem(recursive);
        Require(selected && dynamic_cast<FolderEntryIcon*>(selected), "recursive match is a native file entry with the actual path");
        const auto selectedPath = selected->GetPath();
        selected->SetSelected(true);
        Require(view->GetSelectedMemberIndices() == std::vector<size_t>{recursive}, "result selection uses presentation indices");
        Require(Access::FolderSearchTargets(app, surface == 2 ? L"ordered-group" : L"ordered").size() == expected,
            "range/select-all targets include recursive native results");
        Require(Access::FolderSearchClipboard(app) == std::vector<std::wstring>{selectedPath}, "clipboard gathers recursive selected paths");
        const auto captured = Access::CaptureFolderSearch(app, *view);
        Require(captured.entries.size() == 1 && captured.entries.front().filePath == selectedPath,
            "real native drag capture retains the recursive file identity");
        const auto desktopPreview = Access::FolderSearchDesktopPreview(app, *view);
        Require(!desktopPreview.Empty() && desktopPreview.fileBacked && desktopPreview.targetKind == DropTargetKind::Desktop,
            "recursive file result must reach the real desktop target and publish a physical file landing, not stop at capture or source-member reorder");
        view->GetMemberItem(0)->SetSelected(true);
        const auto mixed = Access::FolderSearchClipboard(app);
        const auto firstDirect = surface == 1 ? mappedDirect.wstring() : direct.wstring();
        Require(mixed.size() == 2 && std::find(mixed.begin(), mixed.end(), selectedPath) != mixed.end() &&
            std::find(mixed.begin(), mixed.end(), firstDirect) != mixed.end(),
            "mixed direct and recursive selection preserves every chosen file in one operation");
        Access::ClearFolderSearchSelection(app);
        Require(view->GetSelectedMemberIndices().empty(), "clear-selection includes runtime-only entries");
        const RECT originalBounds = view->GetWidgetData()->bounds;
        view->GetWidgetData()->bounds.bottom = originalBounds.top + view->Cu(180.0f);
        view->GetWidgetData()->scrollOffset = view->GetMaxScrollOffset();
        view->InvalidateSlots();
        const auto& scrolled = view->GetSlots();
        const RECT viewport = view->GetContentViewportRect();
        Require(!scrolled.empty() && scrolled.size() < expected && scrolled.back()->GetIndex() == expected - 1,
            "scrolling materializes the final visible results and excludes earlier sections");
        for (const auto& slot : scrolled)
        {
            const RECT cell = slot->GetBounds();
            Require(slot->GetItem() && cell.bottom > viewport.top && cell.top < viewport.bottom,
                "source headers never become file slots and only viewport-intersecting result cells are materialized");
        }
        RECT marquee = scrolled.back()->GetBounds(); OffsetRect(&marquee, 0, view->GetScrollOffset());
        view->ApplyMarqueeSelection(marquee);
        Require(view->GetSelectedMemberIndices() == std::vector<size_t>{expected - 1},
            "marquee uses the same source-header offsets as scrolled native hit slots");
        Access::ClearFolderSearchSelection(app);
        view->GetWidgetData()->bounds = originalBounds;
        view->GetWidgetData()->scrollOffset = 0;
        view->InvalidateSlots();
        for (const auto& widget : original)
        {
            const auto& current = pair::Widget(app, widget.id);
            Require(current.itemKeys == widget.itemKeys && current.folderEntries.size() == widget.folderEntries.size() &&
                current.sourceFolderPath == widget.sourceFolderPath, "search never persists descendants or changes source membership");
        }
        if (surface == 0)
        {
            struct Gate { std::mutex mutex; std::condition_variable cv; bool entered = false; bool release = false; };
            auto gate = std::make_shared<Gate>();
            Access::SetFolderSearchBoundary(*view, [gate, a, c](const std::wstring& query, DWORD) {
                const bool firstQuery = query.find(L"a-needle") != std::wstring::npos;
                if (firstQuery)
                {
                    std::unique_lock lock(gate->mutex);
                    gate->entered = true; gate->cv.notify_all();
                    gate->cv.wait_for(lock, std::chrono::seconds(5), [&] { return gate->release; });
                }
                const auto& path = firstQuery ? a : c;
                snowdesktop::QuickNavigationEverythingSearchResponse response;
                response.results.push_back({path.filename().wstring(), path.wstring(), {}, false});
                return response;
            });
            view->SetSearchText(L"a-needle"); view->EnsureFolderSearch();
            native_drag_async_reload_cases::Wait([&] { std::scoped_lock lock(gate->mutex); return gate->entered; }, "old query reaches the controlled SDK boundary");
            Require(view->UsesFolderSearch() && Access::FolderSearchPending(*view) &&
                view->FolderSearchNotice() == _LW("settings.widgets.searching"),
                "an unfinished real request displays searching instead of an empty-result claim");
            view->SetSearchText(L"c-needle"); view->EnsureFolderSearch();
            { std::scoped_lock lock(gate->mutex); gate->release = true; } gate->cv.notify_all();
            native_drag_async_reload_cases::Wait([&] { return !Access::FolderSearchPending(*view); }, "latest query completes after the older blocked result");
            Require(view->UsesFolderSearch() && view->GetSlotCount() == 1 && view->GetMemberItem(0)->GetPath() == c.wstring(),
                "typing a newer query coalesces the same worker and never displays the stale result");
        }
        view->ClearSearchText(); Require(!view->UsesFolderSearch(), "clearing query discards the recursive view");
        Access::SetFolderSearchBoundary(*view, [](const std::wstring&, DWORD) {
            snowdesktop::QuickNavigationEverythingSearchResponse response; response.error = ERROR_NOT_READY; return response;
        });
        view->SetSearchText(L"needle"); view->EnsureFolderSearch();
        native_drag_async_reload_cases::Wait([&] { return !Access::FolderSearchPending(*view); }, "unavailable Everything must settle");
        Require(!view->UsesFolderSearch() && view->GetSlotCount() == (surface == 2 ? 2u : 1u), "unavailable Everything retains original direct search");
        Access::StopModel(app);
        fs::remove_all(first); fs::remove_all(second); fs::remove(direct);
    }
    std::cout << scenes << " native scoped folder-search scenes passed\n";
}

inline void Pagination()
{
    Profile profile;
    for (int surface = 0; surface < 3; ++surface)
    {
        context = "per-title search pagination surface=" + std::to_string(surface);
        const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
        const auto project = desktop / (L"pagination-project-" + std::to_wstring(surface));
        const auto mapped = profile.root / (L"pagination-map-" + std::to_wstring(surface));
        fs::create_directories(project); fs::create_directories(mapped / L"nested");
        std::vector<fs::path> direct, answers;
        for (int i = 0; i < 26; ++i)
        {
            const auto name = L"direct-needle-" + std::to_wstring(i) + L".txt";
            for (const auto& file : {desktop / name, mapped / name})
            { std::ofstream stream(file); stream << "direct"; answers.push_back(file); }
            direct.push_back(desktop / name);
        }
        for (int i = 0; i < 45; ++i)
            for (const auto& parent : {project, mapped / L"nested"})
            {
                const auto file = parent / (L"recursive-needle-" + std::to_wstring(i) + L".txt");
                std::ofstream stream(file); stream << "recursive"; answers.push_back(file);
            }
        direct.push_back(project);
        auto& app = profile.Model(false, profile.root / (L"pagination-layout-" + std::to_wstring(surface)));
        if (surface == 2)
        {
            Access::SeedSearchTypeSurface(app, direct, mapped, true);
            auto* group = dynamic_cast<FileGroup*>(Access::MemberContainer(app, L"ordered-group"));
            group->GetWidgetData()->activeCategoryId = group->GetWidgetData()->childWidgetIds.back();
        }
        else Access::SeedFilterSurface(app, surface == 0 ? direct : std::vector<fs::path>{mapped / L"direct-needle-0.txt"},
            surface == 0 ? DesktopWidgetType::FileCategories : DesktopWidgetType::FolderMapping, true, false);
        const auto id = surface == 2 ? L"ordered-group" : L"ordered";
        auto view = [&]() { return dynamic_cast<ScrollingItemWidget*>(Access::MemberContainer(app, id)); };
        Access::SetFolderSearchBoundary(*view(), [answers](const std::wstring&, DWORD) {
            snowdesktop::QuickNavigationEverythingSearchResponse response;
            for (const auto& file : answers) response.results.push_back({file.filename().wstring(), file.wstring(), {}, false});
            return response;
        });
        view()->SetSearchText(L"needle"); view()->SetSearchFocused(true); view()->SetSearchCursorPosition(3);
        native_drag_async_reload_cases::Wait([&] { return !Access::FolderSearchPending(*view()); }, "pagination query completes");
        const auto sections = Access::FolderSearchSections(*view());
        Require(sections.size() == (surface == 2 ? 4u : 2u), "every source keeps separate direct and recursive sections");
        for (const auto& section : sections) Require(section.count == 20, "each title initially exposes exactly twenty items");
        if (surface == 2)
            Require(sections.front().key.starts_with(view()->GetWidgetData()->activeCategoryId + L"\n") &&
                view()->GetMemberItem(0)->GetPath().starts_with(mapped.wstring()),
                "active tab's title and actual contents precede all other sources");
        const size_t index = 1;
        const auto expand = [&] {
            const RECT before = view()->GetFolderSearchMoreRect(index);
            const RECT viewport = view()->GetContentViewportRect();
            view()->GetWidgetData()->scrollOffset = std::clamp<int>(view()->GetScrollOffset() + before.top - viewport.top,
                0, view()->GetMaxScrollOffset()); view()->InvalidateSlots();
            const RECT button = view()->GetFolderSearchMoreRect(index);
            const POINT point{(button.left + button.right) / 2, (button.top + button.bottom) / 2};
            Require(view()->HitTestWidget(point) == WidgetHit::SearchMoreBtn, "expansion footer is a real button, never a file slot");
            Slot* slot = nullptr;
            Require(view()->WidgetContainer::HitTestDrag(point, slot) == HitRegion::Blocked && !slot,
                "expansion footer cannot accidentally accept a file drop");
            Require(view()->ExpandFolderSearchAtPoint(point), "clicking that footer expands its own section");
        };
        expand();
        Require(Access::FolderSearchSections(*view())[index].count == 40, "first expansion adds exactly twenty");
        Require(Access::FolderSearchSections(*view())[0].count == 20, "expanding one title does not expand its neighbor");
        Access::RebuildFolderSearch(app);
        Require(view()->GetSearchText() == L"needle" && view()->GetSearchCursorPosition() == 3 && view()->IsSearchFocused(),
            "container rebuild preserves query, cursor and focus");
        native_drag_async_reload_cases::Wait([&] { return !Access::FolderSearchPending(*view()); }, "rebuild refreshes the preserved query");
        Require(Access::FolderSearchSections(*view())[index].count == 40, "refresh preserves independent expansion limits");
        expand();
        Require(Access::FolderSearchSections(*view())[index].count == 45 && IsRectEmptyRect(view()->GetFolderSearchMoreRect(index)),
            "last expansion reveals only the remaining five and removes the footer");
        Require(view()->GetWidgetData()->scrollOffset > 0, "clear-scroll regression starts from the actually scrolled search results");
        view()->ClearSearchText();
        Require(view()->GetWidgetData()->scrollOffset == 0 && view()->GetScrollOffset() == 0,
            "clearing the search resets persisted content scroll instead of carrying result position into normal contents");
        Require(!view()->UsesFolderSearch(), "explicit clear exits result mode");
        Access::StopModel(app);
        for (const auto& file : direct) { std::error_code error; fs::remove_all(file, error); }
    }
}
}
