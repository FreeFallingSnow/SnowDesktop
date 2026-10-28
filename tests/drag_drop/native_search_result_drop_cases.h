#pragma once

namespace native_search_result_drop_cases
{
using namespace native_drag_commit_cases;
inline void Run()
{
    Profile profile;
    size_t scenes = 0;
    for (int source = 0; source < 3; ++source)
    for (int target = 0; target < 4; ++target)
    for (const int mods : {0, MK_CONTROL, MK_ALT})
    {
        const auto sourceType = source == 1 ? DesktopWidgetType::FolderMapping : DesktopWidgetType::FileCategories;
        context = "search file commit source=" + std::to_string(source) +
            " target=" + std::to_string(target) + " mods=" + std::to_string(mods);
        const auto root = profile.root / (L"search-commit-" + std::to_wstring(scenes++));
        const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
        const auto sourceRoot = desktop / root.filename();
        fs::create_directories(sourceRoot / L"nested"); fs::create_directories(root / L"target");
        fs::create_directories(root / L"inactive");
        const auto file = sourceRoot / L"nested" / (root.filename().wstring() + L"-needle.txt");
        { std::ofstream stream(file); stream << "actual-search-file-bytes"; }
        auto& app = profile.Model(false, root / L"layout");
        if (source == 2) Access::SeedSearchTypeSurface(app, {sourceRoot}, root / L"inactive", true);
        else Access::SeedFilterSurface(app, sourceType == DesktopWidgetType::FolderMapping
            ? std::vector<fs::path>{sourceRoot / L"nested"} : std::vector<fs::path>{sourceRoot}, sourceType, true, false);
        if (target == 1 || target == 2) Access::AddSearchDropTarget(app, root / L"target", target);
        Access::PrepareReload(app); Access::BindAsync(app);
        auto view = [&]() { return dynamic_cast<ScrollingItemWidget*>(Access::MemberContainer(app, source == 2 ? L"ordered-group" : L"ordered")); };
        Access::SetFolderSearchBoundary(*view(), [file](const std::wstring&, DWORD) {
            snowdesktop::QuickNavigationEverythingSearchResponse response;
            if (fs::exists(file)) response.results.push_back({file.filename().wstring(), file.wstring(), {}, false});
            return response;
        });
        view()->SetSearchText(L"needle");
        native_drag_async_reload_cases::Wait([&] { return !Access::FolderSearchPending(*view()); }, "recursive SDK response settles");
        Require(view()->GetSlotCount() == 1, "one actual descendant is offered to selection");
        view()->GetMemberItem(0)->SetSelected(true);
        const POINT point = target == 3 ? POINT{250, 400} : target ? POINT{950, 250} : POINT{850, 650};
        Access::BeginSearchDrop(app, *view(), point, mods);
        Access::EndActualGesture(app);
        Require(fs::exists(file) && Read(file) == "actual-search-file-bytes" && Access::NoPendingFileDrop(app) &&
            view()->GetSearchText() == L"needle" && view()->GetSelectedMemberIndices() == std::vector<size_t>{0},
            "cancelling a captured search drag performs no file operation and keeps query, result and selection");
        const auto captured = Access::BeginSearchDrop(app, *view(), point, mods);
        Require(captured.hasSearchResults && captured.SlotPayloadKind() == snowdesktop::slot_contract::DragPayloadKind::SearchResult,
            "capture classifies a recursive result independently from persisted folder membership");
        // Rebuilding during a drag must retain the exact captured file and query.
        Access::RebuildFolderSearch(app);
        Require(Access::CapturedSources(app).entries.front().filePath == file.wstring() && view()->GetSearchText() == L"needle",
            "runtime rebuild keeps captured source identity and active query");
        const auto preview = Access::ObserveModelPreview(app, point, mods);
        Require(!preview.Empty() && preview.fileBacked, "actual target resolution publishes a physical file transaction");
        Access::SubmitSearchDrop(app, mods);
        native_drag_async_reload_cases::Wait([&] { return Access::FileDropIdle(app) && Access::FolderReaderIdle(app); },
            "real Shell file operation and asynchronous placement refresh complete");
        const bool link = mods == MK_ALT, move = mods == 0;
        const bool physicalMapping = target == 1 || (target == 3 && source == 1);
        const auto output = (target == 1 ? root / L"target" : target == 3 && source == 1 ? sourceRoot : desktop) /
            (link ? (physicalMapping ? file.filename().wstring() : file.stem().wstring()) + L".lnk" : file.filename().wstring());
        Require(fs::exists(output) && fs::exists(file) == !move,
            "actual drop creates the destination and preserves/removes the source according to the effect");
        if (!link) Require(Read(output) == "actual-search-file-bytes", "Shell output contains original bytes");
        else
        {
            ComPtr<IShellLinkW> shortcut; ComPtr<IPersistFile> persist;
            Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shortcut))) &&
                SUCCEEDED(shortcut.As(&persist)) && SUCCEEDED(persist->Load(output.c_str(), STGM_READ)), "link is a readable actual Shell shortcut");
            wchar_t path[MAX_PATH]{};
            Require(SUCCEEDED(shortcut->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH)) &&
                _wcsicmp(path, file.c_str()) == 0, "shortcut targets the captured descendant");
        }
        Require(view()->GetSearchText() == L"needle", "drag completion keeps the search query");
        native_drag_async_reload_cases::Refresh(app);
        Require(view()->GetSearchText() == L"needle", "explicit asynchronous refresh keeps the query");
        native_drag_async_reload_cases::Wait([&] { return !Access::FolderSearchPending(*view()); }, "preserved search refreshes its results");
        Require(view()->GetSlotCount() == (target == 3 ? move ? 1u : 2u : move ? 0u : 1u),
            "refresh removes moved recursive results, retains originals for copy/link and includes new direct members at the source target");
        if (target == 2)
        {
            const auto& widgets = Access::Widgets(app);
            const auto found = std::find_if(widgets.begin(), widgets.end(), [](const auto& widget) { return widget.id == L"search-target"; });
            Require(found != widgets.end() && found->itemKeys == std::vector<std::wstring>{ToUpperInvariant(output.wstring())},
                "real pending-placement completion assigns exactly the materialized file to the target collection");
        }
        Access::UnbindAsync(app); Access::StopModel(app);
        fs::remove(output); fs::remove_all(sourceRoot);
    }
    std::cout << scenes << " actual recursive-search file transactions passed\n";
}
}
