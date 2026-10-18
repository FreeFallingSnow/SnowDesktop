#pragma once

// S04 preserves the native capture across real SelfOle callbacks. S05/S06
// hold only one owned posted read-completion message: source reads, Shell work,
// destination capture/commit, leases, refresh and persistence stay production.
namespace native_ole_sequence_cases
{
namespace fs = std::filesystem;
using Access = NativeDragIntegrationAccess;
using native_drag_commit_cases::Require;
using native_drag_commit_cases::Read;
using native_drag_reload_cases::Find;
using native_ole_file_cases::Wait;
using native_ole_file_cases::Outputs;
using native_ole_file_cases::LinkTarget;

inline void Write(const fs::path& path, std::string_view value)
{ fs::create_directories(path.parent_path()); std::ofstream stream(path, std::ios::binary); stream << value; }
inline bool Same(const GridCell& a, const GridCell& b)
{ return a.pageId == b.pageId && a.column == b.column && a.row == b.row; }
inline void CleanDesktop(const fs::path& desktop)
{
    // The profile owns this complete desktop directory; no build/user data.
    Require(desktop == snowdesktop::debug_profile::Current().configuration.desktop,
        "sequence cleanup requires the active isolated desktop");
    for (const auto& item : fs::directory_iterator(desktop)) fs::remove_all(item.path());
}
inline void ReloadAndCheck(DesktopApp& app, const std::vector<std::pair<std::wstring, GridCell>>& cells)
{
    Require(Access::Save(app), "sequence results must enter actual layout storage");
    Access::PerturbBeforeDiskRead(app); Access::RequestAsync(app, true);
    Wait([&] { return Access::FileDropIdle(app); }, "sequence disk reload must complete through real background delivery");
    for (const auto& [key, cell] : cells)
    {
        const auto& item = Find(app, key);
        Require(Same(item.gridCell, cell), "sequence disk reload must preserve each independent committed placement");
    }
}
inline void RunReturn()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    std::size_t scenes = 0;
    // Preserve source class, single/multi capture and negative page geometry.
    // Left modifiers cover all physical verbs; right Copy and Cancel protect
    // initiating-button/menu ownership without repeating the external matrix.
    for (int source : {0,1,2,3,4,5,6,7}) for (std::size_t count : {1u,2u})
    for (bool secondary : {false,true}) for (int decision : {0,1,2,3,4})
    {
        if ((secondary || decision >= 3) && source != 0) continue;
        if (secondary && decision >= 3) continue;
        native_drag_commit_cases::context = "S04 source=" + std::to_string(source) + " count=" +
            std::to_string(count) + " secondary=" + std::to_string(secondary) + " decision=" + std::to_string(decision);
        const auto root = profile.root / (L"return-" + std::to_wstring(scenes++));
        const bool mapped = source == 3 || source == 6;
        const auto sourceDirectory = mapped ? root / L"mapped" : desktop;
        const std::vector<fs::path> files{sourceDirectory / L"A.txt", sourceDirectory / L"B.txt"};
        Write(files[0], "return-A"); Write(files[1], "return-B");
        const auto sentinel = desktop / L"Sentinel.txt";
        if (source == 7) Write(sentinel,"unrelated-Dock-desktop");
        auto& app = profile.Model(secondary, root / L"layout");
        Access::SeedSequenceOrigin(app, files, sentinel, source); Access::PrepareReload(app); Access::BindAsync(app);
        Require(Access::Save(app), "S04: save the original native capture model");
        const auto before = Read(Access::LayoutPath(app));
        const POINT point{source ? 650L : secondary ? -650L : 150L, source ? 450L : 150L};
        Access::BeginSequenceOrigin(app, source, count, point);
        const auto captured = Access::CapturedSources(app);
        const bool right = decision >= 3;
        const DWORD mods = decision == 1 ? MK_CONTROL : decision == 2 ? MK_ALT : 0;
        auto originals = mapped ? std::vector<fs::path>{} : files;
        if (source == 7) originals.push_back(sentinel);
        auto data = Access::BeginSequenceSelfOle(app, right);
        int menus = 0;
        Access::RightMenuBridge bridge(app, [&](HMENU menu, POINT, HWND owner) {
            ++menus;
            Require(right && owner == Access::completionWindow && IsMenu(menu) && Outputs(desktop, originals).empty(),
                "S04: only an initiating right drag may show its actual menu, before any copy");
            return decision == 3 ? UINT{2} : UINT{4};
        });
        auto adapter = right ? ComPtr<OleDragDropAdapter>{} : Access::ExternalAdapter(app);
        if (right) adapter.Attach(new OleDragDropAdapter(&bridge));
        for (int pass = 0; pass != 2; ++pass)
        {
            DWORD effect = 7;
            Require(SUCCEEDED(adapter->DragEnter(data.Get(), (right ? MK_RBUTTON : MK_LBUTTON) | MK_SHIFT,
                    Access::ExternalPoint(point), &effect)) && effect == 0 && Access::SequenceSelfRetained(app, count),
                "S04: returning native OLE capture must retain its source and keep custom feedback hidden");
            effect = 7;
            Require(SUCCEEDED(adapter->DragOver((right ? MK_RBUTTON : MK_LBUTTON) | mods,
                    Access::ExternalPoint(point), &effect)) && effect == 0 && Access::SequenceSelfRetained(app, count),
                "S04: repeated return/hover must remain self transport without recapturing an external source");
            const auto& current = Access::CapturedSources(app);
            for (std::size_t i = 0; i < count; ++i)
                Require(current.entries[i].kind == captured.entries[i].kind &&
                    current.entries[i].sourceIndex == captured.entries[i].sourceIndex &&
                    current.entries[i].desktopKey == captured.entries[i].desktopKey &&
                    current.entries[i].filePath == captured.entries[i].filePath &&
                    current.entries[i].widgetId == captured.entries[i].widgetId,
                    "S04: return callbacks must preserve selected order, type, stable identity and original owner");
            Require(Outputs(desktop, originals).empty() && Read(Access::LayoutPath(app)) == before,
                "S04: entering or hovering back cannot submit or persist a drop");
        }
        DWORD effect = 7;
        if (right)
        {
            Require(SUCCEEDED(adapter->Drop(data.Get(), MK_LBUTTON | MK_ALT,
                Access::ExternalPoint(point), &effect)) && menus == 1,
                "S04: initiating right button must execute the actual native action menu once");
        }
        else Require(SUCCEEDED(adapter->Drop(data.Get(), mods, Access::ExternalPoint(point), &effect)),
            "S04: returned OLE Drop must enter actual host native dispatch");
        Access::EndSequenceSelfOle(app); data.Reset();
        Require(Access::SequenceSessionEnded(app), "S04: returned release must end its native context once");
        Wait([&] { return Access::FileDropIdle(app); }, "S04: native file work and refresh must complete");
        const auto outputs = Outputs(desktop, originals);
        const bool copy = decision == 1 || decision == 3, link = decision == 2, cancel = decision == 4;
        Require(outputs.size() == (copy || link || mapped ? count : 0),
            "S04: returned release must submit the selected physical action exactly once");
        std::vector<std::pair<std::wstring,GridCell>> cells;
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            if (mapped && !copy && !link && !cancel && i < count)
                Require(!fs::exists(files[i]), "S04: returned mapping Move must remove each original physical mapped source");
            else Require(Read(files[i]) == (i ? "return-B" : "return-A"),
                "S04: logical move/copy/link and untransferred members must preserve original physical files");
            if (i >= count || cancel || copy || link)
            {
                if (!source) Require(Find(app, ToUpperInvariant(files[i].wstring())).gridCell.row == 0,
                    "S04: untransferred source placement must remain unchanged");
            }
            if (i >= count || cancel) continue;
            const fs::path* output = nullptr;
            if (copy || link || mapped)
            {
                auto found = std::find_if(outputs.begin(), outputs.end(), [&](const auto& p) {
                    return link ? fs::equivalent(LinkTarget(p), files[i]) : Read(p) == (i ? "return-B" : "return-A"); });
                Require(found != outputs.end(), "S04: every captured identity must have its corresponding output");
                output = &*found;
            }
            else output = &files[i];
            const auto& item = Find(app, ToUpperInvariant(output->wstring()));
            Require(Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item),
                "S04: committed return output must have one accessible actual desktop slot");
            if (count == 1)
                Require(item.gridCell.pageId == L"__page:1" && item.gridCell.column == (source ? 6 : 1) &&
                    item.gridCell.row == (source ? 4 : 1),
                    "S04: an accepted single return must reach its independently requested unoccupied cell");
            cells.push_back({item.layoutKey,item.gridCell});
        }
        if (source && !mapped && source != 7)
        {
            const auto& widget = Access::Widgets(app).front();
            Require(widget.id == (source <= 2 ? L"native-origin" : L"ordered") && widget.itemKeys.size() ==
                (cancel || copy || link ? 2 : 2 - count),
                "S04: only a returned logical Move may remove captured members from their original component");
        }
        if (source == 7)
            Require(Access::DockEntries(app).size() == (copy || link ? 2u : 2u-count) && Read(sentinel) == "unrelated-Dock-desktop",
                "S04: ordinary Dock return must remove only moved references and preserve its unrelated desktop file");
        if (cancel) Require(effect == 0 && Read(Access::LayoutPath(app)) == before,
            "S04: cancelled returned drag cannot change the saved layout");
        ReloadAndCheck(app, cells); Access::StopModel(app); CleanDesktop(desktop);
    }
    std::cout << "Native self OLE return: " << scenes << " scenes passed\n";
}

// Holding the actual posted completion keeps its original ownership and data;
// only delivery order is controlled, never read/commit outcomes or callbacks.
struct HeldRead
{
    MSG message{};
    ~HeldRead() { if (message.hwnd) DispatchMessageW(&message); }
    void Capture()
    {
        const auto started = GetTickCount64();
        while (!message.hwnd)
        {
            Require(GetTickCount64() - started < 12000, "S05: real delayed source read must post its own completion");
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG next{};
            while (PeekMessageW(&next, nullptr, 0, 0, PM_REMOVE))
            {
                if (next.hwnd == Access::completionWindow && next.message == kShellFileOperationCompletedMessage)
                {
                    message = next;
                    const auto* value = reinterpret_cast<const snowdesktop::ShellFileOperationUiCompletion*>(message.lParam);
                    Require(value && !value->fileOperation && value->succeeded,
                        "S05: gate must hold the successful real source-read completion before destination commit");
                    break;
                }
                DispatchMessageW(&next);
                Access::ThrowAsyncException();
            }
        }
    }
    void Deliver() { auto value = message; message = {}; DispatchMessageW(&value); Access::ThrowAsyncException(); }
};
inline DropPreviewList Submit(DesktopApp& app, ole_file_source_fixture::Source& source, POINT point)
{
    auto adapter = Access::ExternalAdapter(app); DWORD effect = 7;
    Require(SUCCEEDED(adapter->DragEnter(&source, MK_LBUTTON | MK_CONTROL, Access::ExternalPoint(point), &effect)) && effect == 1,
        "S05: actual delayed source must negotiate Copy before capture");
    Access::FreshExternalDwell(app);
    const auto preview = Access::ObserveOlePreview(app, point, MK_CONTROL);
    effect = 7;
    Require(SUCCEEDED(adapter->Drop(&source, MK_CONTROL, Access::ExternalPoint(point), &effect)) && effect == 1 &&
        source.starts == 1 && source.ends == 0,
        "S05: each delayed task must acknowledge and independently own its source lease immediately");
    return preview;
}
inline void RunAsync()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    std::size_t scenes = 0;
    // Same-name A/B protect actual output-path identity. A can fail after its
    // read, and the group can change its active child while A is pending.
    for (bool group : {false,true}) for (bool failA : {false,true})
    {
        native_drag_commit_cases::context = "S05/S06 group=" + std::to_string(group) + " failA=" + std::to_string(failA);
        const auto root = profile.root / (L"sequences-" + std::to_wstring(scenes++));
        const auto a = root / L"A/Same.txt", b = root / L"B/Same.txt", existing = desktop / L"Existing.txt";
        Write(a, "independent-A"); Write(b, "independent-B"); Write(existing, "unchanged-existing");
        auto& app = profile.Model(false, root / L"layout");
        if (group) Access::SeedSequenceGroup(app, existing); else Access::PopulateModel(app, {existing}, 0);
        Access::PrepareReload(app); Access::BindAsync(app); Require(Access::Save(app), "S05: save the initial independent targets");
        auto sourceA = ole_file_source_fixture::Make({a}, true), sourceB = ole_file_source_fixture::Make({b}, true);
        const POINT pointA{150,150}, pointB{group ? 650L : 350L, group ? 450L : 250L};
        const auto previewA = Submit(app, *sourceA.Get(), pointA);
        HeldRead held; held.Capture();
        Require(sourceA->starts == 1 && sourceA->ends == 0 && Outputs(desktop, {existing}).empty(),
            "S05: holding A's read completion must retain A without performing its destination mutation");
        if (group) Access::SwitchSequenceActive(app);
        if (failA) Require(fs::remove(a), "S06: independently remove the captured source before A's actual commit");
        const auto previewB = Submit(app, *sourceB.Get(), pointB);
        Wait([&] { return sourceB->ends == 1 && Access::FileDropIdle(app); },
            "S05: B must finish through actual host commit while A is still retained");
        Require(SUCCEEDED(sourceB->outcome) && sourceB->endEffect == 1 && sourceA->ends == 0,
            "S05: B completion must not end A's unrelated source lease");
        auto outputsB = Outputs(desktop, {existing});
        Require(outputsB.size() == 1 && Read(outputsB[0]) == "independent-B", "S05: only B may exist before A's delivery");
        const auto keyB = ToUpperInvariant(outputsB[0].wstring()); const auto cellB = Find(app,keyB).gridCell;
        Require(previewB.landings.size() == 1 && Same(cellB,previewB.landings[0].cell) &&
            Access::HasVisibleIcon(app, Find(app,keyB)) && Access::HasMatchingRuntimeSlot(app,Find(app,keyB)),
            "S05: B must fulfill B's captured preview in an accessible native slot");
        Access::RequestAsync(app);
        Wait([&] { return Access::FileDropIdle(app); }, "S05: intervening real Shell refresh must complete before A");
        Require(Same(Find(app,keyB).gridCell,cellB) && sourceA->ends == 0,
            "S05: refresh between completions must preserve B and keep A's task independent");
        held.Deliver();
        Wait([&] { return sourceA->ends == 1 && Access::FileDropIdle(app); }, "S05: retained A must finish after B and refresh");
        Require((failA ? FAILED(sourceA->outcome) : SUCCEEDED(sourceA->outcome)) && sourceA->starts == 1 && sourceB->ends == 1,
            "S05/S06: each real task must report its own success/failure exactly once");
        auto outputs = Outputs(desktop, {existing});
        Require(outputs.size() == (failA ? 1u : 2u) && Read(outputsB[0]) == "independent-B" &&
            Same(Find(app,keyB).gridCell,cellB), "S05: a later or failed A cannot erase, rename or move completed B");
        std::vector<std::pair<std::wstring,GridCell>> cells{{keyB,cellB}};
        if (!failA)
        {
            auto outputA = std::find_if(outputs.begin(),outputs.end(), [&](const auto& p) { return Read(p) == "independent-A"; });
            Require(outputA != outputs.end() && !fs::equivalent(*outputA,outputsB[0]),
                "S05: same-name tasks require distinct physical outputs bound to their actual content");
            const auto& item = Find(app,ToUpperInvariant(outputA->wstring()));
            if (!group) Require(previewA.landings.size() == 1 && Same(item.gridCell,previewA.landings[0].cell) && !Same(item.gridCell,cellB),
                "S05: A must fulfill A's original placement instead of B's later landing");
            else
            {
                const auto& widgets = Access::Widgets(app);
                const auto old = std::find_if(widgets.begin(),widgets.end(),[](const auto& w){return w.id == L"ingress-active";});
                const auto active = std::find_if(widgets.begin(),widgets.end(),[](const auto& w){return w.id == L"ingress-inactive";});
                Require(old != widgets.end() && active != widgets.end() && old->itemKeys.size() == 2 && active->itemKeys.empty() &&
                    std::find(old->itemKeys.begin(),old->itemKeys.end(),item.layoutKey) != old->itemKeys.end(),
                    "S06: active-tab change cannot redirect A from its captured stable child to the current child");
            }
            cells.push_back({item.layoutKey,item.gridCell});
        }
        Require(Read(b) == "independent-B" && (failA || Read(a) == "independent-A") && Read(existing) == "unchanged-existing" &&
            !sourceA->wrongApartment && !sourceB->wrongApartment && !sourceA->fileReadBeforeStart && !sourceB->fileReadBeforeStart,
            "S05/S06: independent source contents, original STA and source lease timing must remain valid");
        ReloadAndCheck(app,cells);
        if (group)
        {
            const auto& widgets = Access::Widgets(app);
            const auto rootWidget = std::find_if(widgets.begin(),widgets.end(),[](const auto& w){return w.id == L"ingress-root";});
            Require(rootWidget != widgets.end() && rootWidget->activeCategoryId == L"ingress-inactive",
                "S06: disk reload must retain the user's intervening active-child choice");
        }
        Access::StopModel(app); CleanDesktop(desktop);
    }
    std::cout << "Native OLE completion sequences: " << scenes << " scenes passed\n";
}
}
