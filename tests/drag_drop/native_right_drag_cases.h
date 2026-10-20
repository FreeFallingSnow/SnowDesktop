#pragma once

namespace native_right_drag_cases
{
using namespace native_ole_file_cases;
struct Decision { std::string_view name; UINT command; DWORD effect, modifiers; const char* label; };
// U09/A01-A03/A16 are named verbs. These values are independent of the host's
// menu generation, modifier conversion and effect-selection helpers.
inline constexpr Decision decisions[]{
    {"move", 1, 2, MK_SHIFT, "widget.base.move"},
    {"copy", 2, 1, MK_CONTROL, "widget.base.copy_label"},
    {"link", 3, 4, MK_ALT, "widget.base.create_shortcut"},
    {"cancel", 4, 0, 0, "app.settings.cancel"},
    {"dismiss", 0, 0, 0, "dismiss"},
};
inline void Menu(HMENU menu, DWORD allowed)
{
    Require(IsMenu(menu), "right-drag UI service must receive an actual host-created menu");
    int offered = 0;
    for (const auto& decision : decisions)
    {
        if (!decision.effect) continue;
        const bool expected = (allowed & decision.effect) != 0;
        MENUITEMINFOW item{}; item.cbSize = sizeof(item); item.fMask = MIIM_STATE;
        Require((GetMenuItemInfoW(menu, decision.command, FALSE, &item) != FALSE) == expected,
            "native right-drag menu must offer exactly the independently named source-supported verbs");
        if (!expected) continue;
        ++offered;
        std::array<wchar_t, 512> label{};
        Require((item.fState & (MFS_DISABLED | MFS_GRAYED)) == 0 &&
            GetMenuStringW(menu, decision.command, label.data(), static_cast<int>(label.size()), MF_BYCOMMAND) > 0 &&
            std::wstring(label.data()) == _LW(decision.label),
            "native right-drag entries must retain their promised localized label and enabled state");
    }
    MENUITEMINFOW cancel{}; cancel.cbSize = sizeof(cancel); cancel.fMask = MIIM_STATE;
    Require(GetMenuItemInfoW(menu, 4, FALSE, &cancel) && (cancel.fState & (MFS_DISABLED | MFS_GRAYED)) == 0 &&
        GetMenuItemCount(menu) == offered + 2,
        "native right-drag menu must include cancellation and its separator without extra actions");
}
inline void Run(std::string_view name)
{
    const auto selected = std::find_if(std::begin(decisions), std::end(decisions),
        [&](const auto& value) { return value.name == name; });
    Require(selected != std::end(decisions), "native right-drag tests require a declared menu decision");
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    std::size_t scenes = 0;
    std::uint64_t menus = 0, completions = 0;
    for (const bool secondary : {false, true})
    for (const bool asynchronous : {false, true})
    for (const std::size_t count : {1u, 2u})
    for (const bool rebuild : {false, true})
    for (const DWORD allowed : {7u,0u,1u,2u,3u,4u,5u,6u})
    {
        native_drag_commit_cases::context = "right native=" + std::string(name) + " allowed=" + std::to_string(allowed) +
            " count=" + std::to_string(count) + " async=" + std::to_string(asynchronous) +
            " secondary=" + std::to_string(secondary) + " rebuild=" + std::to_string(rebuild);
        const auto directory = profile.root / (L"right-native-" + std::to_wstring(scenes++));
        fs::create_directories(directory / L"source");
        std::vector<fs::path> files{directory / L"source/Payload.txt"};
        { std::ofstream stream(files.front(), std::ios::binary); stream << "right-file"; }
        if (count == 2)
        {
            files.push_back(directory / L"source/Bundle"); fs::create_directory(files.back());
            std::ofstream stream(files.back() / L"nested.txt", std::ios::binary); stream << "right-directory";
        }
        const std::vector<fs::path> existing{desktop / L"Existing-A.txt", desktop / L"Existing-B.txt"};
        for (std::size_t i = 0; i < existing.size(); ++i)
        { std::ofstream stream(existing[i], std::ios::binary); stream << "existing-file-" << i; }
        auto& app = profile.Model(secondary, directory / L"layout");
        Access::PopulateModel(app, existing, 0); Access::PrepareReload(app); Access::BindAsync(app);
        Require(Access::Save(app), "save actual native desktop before right-drag decision");
        const auto saved = Read(Access::LayoutPath(app));
        auto source = ole_file_source_fixture::Make(files, asynchronous);
        int menuCalls = 0;
        Access::RightMenuBridge bridge(app, [&](HMENU menu, POINT, HWND owner) {
            ++menuCalls;
            Require(menuCalls == 1 && owner == Access::completionWindow && Access::RightMenuRetained(app) &&
                Access::NoPresentationWindows(app) && source->starts == 0 && source->ends == 0 &&
                (!asynchronous || source->dataReads == 0) && Outputs(desktop, existing).empty() &&
                Read(Access::LayoutPath(app)) == saved,
                "native right-menu decision must freeze the retained session before source reads, leases or commits");
            Menu(menu, allowed);
            if (rebuild) Access::RebuildDuringMenu(app);
            Require(Access::RightMenuRetained(app) && Access::NoPresentationWindows(app),
                "native right-menu model rebuild must preserve the pending decision without a desktop window");
            return selected->command;
        });
        ComPtr<OleDragDropAdapter> adapter; adapter.Attach(new OleDragDropAdapter(&bridge));
        const POINT point{secondary ? -650L : 150L, 150};
        DWORD feedback = allowed;
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), MK_RBUTTON,
            Access::ExternalPoint({point.x + 100, point.y}), &feedback)) && (feedback & ~allowed) == 0,
            "actual right-drag adapter entry cannot advertise an unoffered effect");
        feedback = allowed;
        Require(SUCCEEDED(adapter->DragOver(MK_RBUTTON, Access::ExternalPoint(point), &feedback)) &&
            (feedback & ~allowed) == 0 && Access::Target(app) == Access::Desktop(app) &&
            Access::TargetSlot(app) && Access::TargetSlot(app)->GetIndex() == 5,
            "actual right-drag hover must capture the requested real desktop target");
        const auto summaryCount = asynchronous ? 1u : count;
        Require(Access::ExternalSummary(app).fileCount == static_cast<int>(summaryCount) && source->starts == 0 &&
            source->ends == 0 && (!asynchronous || source->dataReads == 0),
            "native right-drag hover must probe the real source without rendering delayed content");
        const auto preview = Access::ObserveOlePreview(app, point, static_cast<int>(selected->modifiers));
        const DWORD expected = selected->effect & allowed;
        DWORD returned = allowed;
        // Right was released; another button and Ctrl are reported now. The
        // real adapter must retain the initiating right button and live Ctrl;
        // the actual menu decision must then determine the executed verb.
        Require(SUCCEEDED(adapter->Drop(source.Get(), MK_LBUTTON | MK_CONTROL, Access::ExternalPoint(point), &returned)) &&
            bridge.dropKeys == (MK_RBUTTON | MK_CONTROL) && menuCalls == (allowed ? 1 : 0),
            "native OLE right-button ownership must reach the real host decision exactly once");
        if (expected && asynchronous)
        {
            // IDataObjectAsyncCapability requires StartOperation before Drop
            // returns. The pending operation acknowledges the chosen verb;
            // optimized-move cleanup is reported after completion, not here.
            Require(returned == expected,
                ("native right-menu asynchronous Drop must acknowledge the chosen effect before returning: expected=" +
                    std::to_string(expected) + " actual=" + std::to_string(returned)).c_str());
            Require(source->starts == 1,
                "native right-menu asynchronous Drop must start exactly one source lease before returning");
        }
        if (expected)
            Wait([&] { return Access::FileDropIdle(app) && Access::Items(app).size() == 2 + count &&
                    (!asynchronous || source->ends == 1); },
                "chosen native right-drag verb must finish actual file submission and model refresh");
        const auto outputs = Outputs(desktop, existing);
        Require(outputs.size() == (expected ? count : 0),
            "native right-drag decision must create exactly the independently expected physical outputs");
        if (!expected)
            Require(returned == 0 && source->starts == 0 && source->ends == 0 &&
                Read(Access::LayoutPath(app)) == saved && Access::Items(app).size() == 2,
                "cancel, dismissal and unoffered native right-menu verbs cannot submit files or alter saved layout");
        std::vector<std::pair<std::wstring, GridCell>> published;
        std::unordered_set<std::wstring> cells;
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            Require(fs::exists(files[i]) == (expected != 2),
                "chosen native right-menu move must remove originals while copy, link and cancellation preserve them");
            if (expected != 2) Require(Read(i ? files[i] / L"nested.txt" : files[i]) == (i ? "right-directory" : "right-file"),
                "native right-menu actions that retain sources must preserve all original bytes");
            if (!expected) continue;
            const auto output = std::find_if(outputs.begin(), outputs.end(), [&](const auto& file) {
                return expected == 4 ? fs::equivalent(LinkTarget(file), files[i]) : file.filename() == files[i].filename(); });
            Require(output != outputs.end(), "native right-menu output must correspond to the selected physical source identity");
            if (expected == 4) Require(_wcsicmp(output->extension().c_str(), L".lnk") == 0,
                "native right-menu link decision must create an actual Shell shortcut");
            else Require(Read(i ? *output / L"nested.txt" : *output) == (i ? "right-directory" : "right-file"),
                "native right-menu move/copy must preserve actual file and nested-directory bytes");
            const auto& item = Find(app, ToUpperInvariant(output->wstring()));
            if (i < summaryCount)
            {
                const auto landing = std::find_if(preview.landings.begin(), preview.landings.end(),
                    [&](const auto& value) { return value.sourceIndex == i; });
                Require(landing != preview.landings.end() && item.gridCell.pageId == landing->cell.pageId &&
                    item.gridCell.column == landing->cell.column && item.gridCell.row == landing->cell.row,
                    "native right-menu file results must fulfill the accepted pre-decision landing preview");
            }
            Require(Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item) &&
                cells.insert(item.gridCell.pageId + L":" + std::to_wstring(item.gridCell.column) + L"," +
                    std::to_wstring(item.gridCell.row)).second,
                "native right-menu results require distinct visible runtime placements");
            published.emplace_back(item.layoutKey, item.gridCell);
        }
        Require(!source->wrongApartment && !source->fileReadBeforeStart,
            "native right-menu source reads must retain original STA and asynchronous lease ownership");
        if (expected && asynchronous)
            Require(source->starts == 1 && source->ends == 1 && SUCCEEDED(source->outcome) &&
                source->endEffect == (expected == 2 ? 0u : expected),
                "native chosen verb must complete one actual source lease with optimized-move ownership");
        CheckOriginals(app, existing);
        if (expected)
        {
            Access::Load(app); native_drag_async_reload_cases::Refresh(app, true);
            CheckOriginals(app, existing);
            for (const auto& [key, cell] : published)
            {
                const auto& item = Find(app, key);
                Require(item.gridCell.pageId == cell.pageId && item.gridCell.column == cell.column &&
                    item.gridCell.row == cell.row && Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item),
                    "native right-menu outputs must retain their accepted cells after real disk reload");
            }
        }
        menus += static_cast<std::uint64_t>(menuCalls); completions += Access::fileCompletionMessages;
        Require(Access::NoPresentationWindows(app), "native right-menu fixture must never create desktop presentation windows");
        Access::StopModel(app); source.Reset();
        for (const auto& output : outputs)
        {
            Require(fs::weakly_canonical(output.parent_path()) == fs::weakly_canonical(desktop),
                "only isolated native desktop right-drag outputs may be removed");
            fs::remove_all(output);
        }
        for (const auto& file : existing) fs::remove(file);
    }
    std::cout << scenes << " actual native right-drag verb/files/placement scenes passed: " << name << "; "
        << menus << " host menus, " << completions << " file completions; menu UI return is the only supplied decision\n";
}
}
