#pragma once

namespace native_dock_return_cases
{
using namespace native_ole_file_cases;
struct Gesture { const char* name; int modifiers; UINT command; DropAction action; bool menu, cancel; };
// A11/U07/U09: literal named user decisions, declared independently of preview
// and modifier conversion. Unknown combinations and generated items are separate.
inline constexpr Gesture gestures[]{
    {"ordinary", 0, 0, DropAction::Move, false, false},
    {"shift", MK_SHIFT, 0, DropAction::Move, false, false},
    {"control", MK_CONTROL, 0, DropAction::Copy, false, false},
    {"alt", MK_ALT, 0, DropAction::Link, false, false},
    {"right-move", 0, 1, DropAction::Move, true, false},
    {"right-copy", 0, 2, DropAction::Copy, true, false},
    {"right-link", 0, 3, DropAction::Link, true, false},
    {"right-cancel", 0, 4, DropAction::Move, true, true},
    {"right-dismiss", 0, 0, DropAction::Move, true, true},
};
struct Original
{
    fs::path path;
    std::string bytes;
    BY_HANDLE_FILE_INFORMATION identity{};
};
inline BY_HANDLE_FILE_INFORMATION Identity(const fs::path& path)
{
    const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "open original Dock member for physical identity");
    BY_HANDLE_FILE_INFORMATION result{};
    const bool success = GetFileInformationByHandle(handle, &result) != FALSE;
    CloseHandle(handle);
    Require(success, "read original Dock member identity");
    return result;
}
inline bool SameCell(const GridCell& left, const GridCell& right)
{ return left.pageId == right.pageId && left.column == right.column && left.row == right.row; }
inline void RemoveOwnedEntry(const fs::path& desktop, const fs::path& path)
{
    Require(fs::canonical(path).parent_path() == fs::canonical(desktop),
        "native Dock cleanup must resolve to one owned direct child of the isolated desktop");
    fs::remove_all(path);
}
inline std::vector<std::wstring> DockOrder(const DesktopApp& app)
{
    std::vector<std::wstring> order;
    for (const auto& entry : Access::DockEntries(app))
    {
        Require(entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop,
            "A23 ordinary fixed Dock entries must retain their type and exclusive role");
        order.push_back(ToUpperInvariant(entry.reference));
    }
    return order;
}
inline void OriginalFiles(const std::vector<Original>& originals)
{
    for (const auto& original : originals)
    {
        const auto now = Identity(original.path);
        Require(now.dwVolumeSerialNumber == original.identity.dwVolumeSerialNumber &&
            now.nFileIndexHigh == original.identity.nFileIndexHigh && now.nFileIndexLow == original.identity.nFileIndexLow &&
            CompareFileTime(&now.ftLastWriteTime, &original.identity.ftLastWriteTime) == 0,
            "A11: returning, copying or linking a Dock entry must retain its original file identity and modification time");
        if ((now.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            Require(Read(original.path) == original.bytes, "A11: Dock source, nested bytes and unrelated files must remain unchanged");
    }
}
inline void Run()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const auto& gesture : gestures)
    for (const bool secondary : {false, true})
    for (const int selection : {0, 1, 2}) // one file / two files / file plus directory
    for (const bool reverse : {false, true})
    for (const bool rebuild : {false, true})
    {
        // Only same-partition files have both input orders. Directories occupy
        // the Dock folder partition; a reverse raw seed is not a reverse visible
        // selection. Keep that distinct object mix without an invalid order.
        if ((selection != 1 && reverse) || (!gesture.menu && rebuild)) continue;
        const std::size_t count = selection == 0 ? 1u : 2u;
        native_drag_commit_cases::context = "A23 " + std::string(gesture.name) + " selection=" + std::to_string(selection) + " count=" + std::to_string(count) +
            " reverse=" + std::to_string(reverse) + " secondary=" + std::to_string(secondary) + " menuRebuild=" + std::to_string(rebuild);
        const auto prefix = L"dock-return-" + std::to_wstring(scenes++);
        std::vector<fs::path> sources{desktop / (prefix + L"-Payload.txt")};
        { std::ofstream stream(sources.front(), std::ios::binary); stream << "dock-original-A"; }
        if (selection == 1)
        {
            sources.push_back(desktop / (prefix + L"-Other.txt"));
            std::ofstream stream(sources.back(), std::ios::binary); stream << "dock-original-B";
        }
        if (selection == 2)
        {
            sources.push_back(desktop / (prefix + L"-Bundle")); fs::create_directory(sources.back());
            std::ofstream stream(sources.back() / L"nested.txt", std::ios::binary); stream << "dock-original-B";
        }
        const auto unselected = desktop / (prefix + L"-Unselected.txt");
        const auto sentinel = desktop / (prefix + L"-Sentinel.txt");
        { std::ofstream stream(unselected, std::ios::binary); stream << "unselected-Dock"; }
        { std::ofstream stream(sentinel, std::ios::binary); stream << "unrelated-desktop"; }
        auto originalPaths = sources; originalPaths.push_back(unselected); originalPaths.push_back(sentinel);
        std::vector<Original> originals;
        for (const auto& path : originalPaths)
        {
            originals.push_back({path, fs::is_directory(path) ? std::string{} : Read(path), Identity(path)});
            if (fs::is_directory(path)) originals.push_back({path / L"nested.txt", Read(path / L"nested.txt"), Identity(path / L"nested.txt")});
        }
        auto ordered = sources;
        if (reverse) std::reverse(ordered.begin(), ordered.end());
        auto seed = ordered; seed.push_back(unselected);
        auto& app = profile.Model(secondary, profile.root / prefix);
        Access::SeedDockFiles(app, seed, sentinel); Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto beforeDock = DockOrder(app);
        std::vector<std::wstring> selected;
        for (const auto& source : ordered) selected.push_back(ToUpperInvariant(source.wstring()));
        Require(Access::Save(app), "save actual native fixed-Dock baseline");
        const auto saved = Read(Access::LayoutPath(app));
        const LONG left = Access::Pages(app).front().bounds.left;
        const POINT point{left + 150, 150};
        const auto captured = Access::BeginDockFiles(app, selected, point, gesture.modifiers);
        Require(captured.entries.size() == count && captured.SourceSurfaceKind() == snowdesktop::slot_contract::SlotSurfaceKind::Dock &&
            captured.hasDesktopIcons && !captured.hasWidgets,
            "A23: actual Dock selection must capture every ordinary selected file and retain the source surface");
        for (std::size_t i = 0; i < count; ++i)
            Require(captured.entries[i].fromDock && captured.entries[i].desktopKey == selected[i] &&
                fs::equivalent(captured.entries[i].filePath, ordered[i]),
                "A23: native Dock capture must preserve the seeded source order and exact file identities");
        auto preview = Access::ObserveModelPreview(app, point, gesture.modifiers);
        Require(Access::Target(app) == Access::Desktop(app) && Access::Region(app) == HitRegion::Empty,
            "A23: actual host hit routing must admit the empty desktop cell");
        int modifiers = gesture.modifiers, menuCalls = 0;
        if (gesture.menu)
        {
            DWORD keys = MK_RBUTTON;
            const bool accepted = Access::ChooseDockFileAction(app, point, keys, [&](HMENU menu, POINT, HWND owner) {
                ++menuCalls;
                Require(menuCalls == 1 && owner == Access::completionWindow && Access::InternalMenuRetained(app) &&
                    Access::FileDropIdle(app) && Outputs(desktop, originalPaths).empty() && Read(Access::LayoutPath(app)) == saved,
                    "S02: internal menu must retain capture without committing, producing files or saving a new layout");
                native_right_drag_cases::Menu(menu, 7);
                if (rebuild) Access::RebuildDuringMenu(app);
                return gesture.command;
            });
            Require(accepted == !gesture.cancel && menuCalls == 1, "A11: internal right-menu decision must select the named verb or cancel");
            modifiers = static_cast<int>(keys & (MK_CONTROL | MK_SHIFT | MK_ALT));
        }
        if (gesture.cancel) Access::EndActualGesture(app);
        else
        {
            // Compare current live wrappers by identity before dereferencing any
            // retained pointer. A menu rebuild must fail here, not as a UAF.
            Require(Access::DockSourceBindingsCurrent(app),
                "S02: menu completion must retain live Dock source bindings for the originally captured identities");
            preview = Access::ObserveModelPreview(app, point, modifiers);
            Require(preview.action == gesture.action && Access::CurrentAction(app) == gesture.action &&
                preview.landings.size() == count && SameCell(preview.anchorCell, {L"__page:1", 1, 1}),
                "A11: named Dock verb and literal empty-cell anchor must reach the complete actual host preview");
            Access::DropDockFiles(app, modifiers);
            Wait([&] { return Access::FileDropIdle(app); }, "actual native Dock file submission/read/placement must finish");
        }
        const auto outputs = Outputs(desktop, originalPaths);
        const auto check = [&] {
            OriginalFiles(originals);
            const auto currentOutputs = Outputs(desktop, originalPaths);
            Require(currentOutputs.size() == outputs.size() && std::all_of(currentOutputs.begin(), currentOutputs.end(),
                [&](const auto& current) { return std::any_of(outputs.begin(), outputs.end(),
                    [&](const auto& original) { return fs::equivalent(current, original); }); }),
                "A11: refresh and reload cannot repeat the file operation or publish extra outputs");
            const bool materialized = !gesture.cancel && gesture.action != DropAction::Move;
            Require(outputs.size() == (materialized ? count : 0), "A11: exactly the chosen Dock copy/link operation may create files");
            Require(DockOrder(app) == (!gesture.cancel && gesture.action == DropAction::Move
                    ? std::vector<std::wstring>{ToUpperInvariant(unselected.wstring())} : beforeDock),
                "A11: Move removes only captured Dock references; Copy, Link and cancellation preserve the original Dock order/ownership");
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto& source = Find(app, selected[i]);
                const bool returned = !gesture.cancel && gesture.action == DropAction::Move;
                Require(returned ? Access::HasVisibleIcon(app, source) : source.gridCell.pageId == kDockPageId && !Access::HasVisibleIcon(app, source),
                    "A11: only an accepted Move restores the original source as a free desktop icon");
                if (gesture.cancel) continue;
                const auto landing = std::find_if(preview.landings.begin(), preview.landings.end(),
                    [&](const auto& value) { return value.sourceIndex == i; });
                Require(landing != preview.landings.end(), "A11: every captured identity must have its own published placement");
                const DesktopItem* result = &source;
                if (materialized)
                {
                    std::vector<fs::path> matching;
                    for (const auto& output : outputs)
                    {
                        bool same = false;
                        if (gesture.action == DropAction::Link)
                            same = _wcsicmp(output.extension().c_str(), L".lnk") == 0 && fs::equivalent(LinkTarget(output), ordered[i]);
                        else if (fs::is_directory(ordered[i])) same = fs::is_directory(output) && Read(output / L"nested.txt") == "dock-original-B";
                        else same = fs::is_regular_file(output) && Read(output) == Read(ordered[i]);
                        if (same) matching.push_back(output);
                    }
                    Require(matching.size() == 1 && !fs::equivalent(matching.front(), ordered[i]),
                        "A11: each copy/link must produce exactly one distinct output with original bytes or a resolvable source target");
                    result = &Find(app, ToUpperInvariant(matching.front().wstring()));
                }
                if (!SameCell(result->gridCell, landing->cell) || !Access::HasVisibleIcon(app, *result) || !Access::HasMatchingRuntimeSlot(app, *result))
                    std::cerr << "A23 source=" << i << " expected=" << WideToUtf8(landing->cell.pageId) << ':' << landing->cell.column << ',' << landing->cell.row
                        << " actual=" << WideToUtf8(result->gridCell.pageId) << ':' << result->gridCell.column << ',' << result->gridCell.row
                        << " visible=" << Access::HasVisibleIcon(app, *result) << " matching-slot=" << Access::HasMatchingRuntimeSlot(app, *result) << '\n';
                Require(SameCell(result->gridCell, landing->cell) && Access::HasVisibleIcon(app, *result) && Access::HasMatchingRuntimeSlot(app, *result),
                    "A11: actual Dock return/output placement and native runtime slots must fulfill the captured preview");
            }
            const auto& untouched = Find(app, ToUpperInvariant(sentinel.wstring()));
            const auto& unused = Find(app, ToUpperInvariant(unselected.wstring()));
            Require(SameCell(untouched.gridCell, {L"__page:1", 3, 2}) && Access::HasVisibleIcon(app, untouched) &&
                unused.gridCell.pageId == kDockPageId && !Access::HasVisibleIcon(app, unused),
                "A11: unselected desktop position and exclusive Dock entry must remain unchanged");
            Require(Access::NoDockWindows(app) && Access::NoIngressPopup(app), "native Dock return cannot allocate a desktop/popup presentation window");
        };
        const auto sceneContext = native_drag_commit_cases::context;
        native_drag_commit_cases::context = sceneContext + " phase=after-drop";
        check();
        if (gesture.cancel) Require(Read(Access::LayoutPath(app)) == saved, "A11: cancelled Dock menu cannot persist any change");
        else
        {
            Require(Access::Save(app), "save actual native Dock return/copy/link result");
            native_drag_commit_cases::context = sceneContext + " phase=after-refresh";
            native_drag_async_reload_cases::Refresh(app); check();
            native_drag_commit_cases::context = sceneContext + " phase=after-reload";
            native_drag_async_reload_cases::Refresh(app, true); check();
        }
        Access::StopModel(app);
        for (const auto& output : outputs) RemoveOwnedEntry(desktop, output);
        for (const auto& path : originalPaths) RemoveOwnedEntry(desktop, path);
    }
    std::cout << scenes << " actual native Dock source, menu, file/return placement and storage scenes passed\n";
}
}
