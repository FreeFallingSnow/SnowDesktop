#pragma once

namespace native_desktop_compat_cases
{
using namespace native_ole_file_cases;

inline void CreateLink(const fs::path& path, const fs::path& target)
{
    ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persisted;
    Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link.As(&persisted)) &&
        SUCCEEDED(persisted->Save(path.c_str(), TRUE)), "D09: create a real original Shell shortcut");
}

inline void RunShortcuts()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const int target : {0, 1, 2})
    for (const bool directory : {false, true})
    for (const int depth : {1, 2})
    for (const bool asynchronous : {false, true})
    {
        native_drag_commit_cases::context = "D09 target=" + std::to_string(target) + " directory=" + std::to_string(directory) +
            " depth=" + std::to_string(depth) + " async=" + std::to_string(asynchronous);
        const auto root = profile.root / (L"shortcut-" + std::to_wstring(scenes++));
        fs::create_directories(root);
        const auto finalTarget = root / (directory ? L"Folder" : L"Original.txt");
        if (directory) fs::create_directories(finalTarget);
        const auto payload = directory ? finalTarget / L"Nested.txt" : finalTarget;
        { std::ofstream stream(payload, std::ios::binary); stream << "original-target-bytes"; }
        const auto first = root / L"First.lnk", second = root / L"Second.lnk";
        CreateLink(first, finalTarget); if (depth == 2) CreateLink(second, first);
        const auto input = depth == 2 ? second : first;
        const auto originalLink = Read(input);
        const auto destination = target == 2 ? root / L"mapped" : desktop;
        fs::create_directories(destination);
        const auto existing = destination / L"Existing.txt";
        { std::ofstream stream(existing, std::ios::binary); stream << "unchanged-target"; }
        auto& app = profile.Model(false, root / L"layout");
        if (target) Access::SeedExternalStorage(app, target == 2 ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection,
            target == 2 ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection, false, existing);
        else Access::PopulateModel(app, {existing}, 0);
        Access::PrepareReload(app); Access::BindAsync(app);
        auto source = ole_file_source_fixture::Make({input}, asynchronous);
        auto adapter = Access::ExternalAdapter(app);
        const POINT point = target ? POINT{350, 250} : POINT{150, 150};
        DWORD effect = 7; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), MK_ALT | MK_LBUTTON, Access::ExternalPoint(point), &effect)),
            "D09: actual native shortcut input must enter the OLE host");
        effect = 7; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragOver(MK_ALT | MK_LBUTTON, Access::ExternalPoint(point), &effect)),
            "D09: actual target must observe the explicit shortcut gesture");
        effect = 7; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->Drop(source.Get(), MK_ALT, Access::ExternalPoint(point), &effect)),
            "D09: actual native shortcut drop must execute");
        Wait([&] { return (target == 2 ? Access::FolderDropReadIdle(app) : Access::FileDropIdle(app)) &&
            (!asynchronous || source->starts == source->ends) && Outputs(destination, {existing}).size() == 1; },
            "D09: actual shortcut submission and read completion must publish one output");
        const auto outputs = Outputs(destination, {existing});
        const auto check = [&] {
            Require(outputs.size() == 1 && fs::equivalent(LinkTarget(outputs.front()), finalTarget),
                "D09: a shortcut made from another shortcut must point directly to its final target");
            Require(Read(input) == originalLink && Read(payload) == "original-target-bytes" && Read(existing) == "unchanged-target",
                "D09: shortcut creation preserves original shortcuts, target bytes and unrelated files");
            Require(!source->wrongApartment && !source->fileReadBeforeStart && Access::NoIngressPopup(app),
                "D09: source lifetime stays on its STA without presentation windows");
            if (target)
            {
                const auto& widget = native_ole_widget_cases::Widget(app, L"ingress-root");
                const bool owns = target == 2
                    ? std::any_of(widget.folderEntries.begin(), widget.folderEntries.end(), [&](const auto& entry) { return fs::equivalent(entry.fullPath, outputs.front()); })
                    : std::any_of(widget.itemKeys.begin(), widget.itemKeys.end(), [&](const auto& key) { return fs::equivalent(key, outputs.front()); });
                Require(owns, "D09: shortcut output must belong to the intended native target");
            }
            else { const auto& item = Find(app, ToUpperInvariant(outputs.front().wstring()));
                Require(Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item), "D09: shortcut output has an accessible desktop slot"); }
        };
        check(); Require(Access::Save(app), "persist actual shortcut result");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); source.Reset();
        if (target != 2) { native_dock_return_cases::RemoveOwnedEntry(desktop, outputs.front()); native_dock_return_cases::RemoveOwnedEntry(desktop, existing); }
    }
    std::cout << scenes << " native shortcut final-target, source lifetime and storage scenes passed\n";
}

inline void RunAppearance()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    for (const int destination : {0, 1, 2})
    {
        native_drag_commit_cases::context = "D11 appearance owner=" + std::to_string(destination);
        const auto file = desktop / (L"Appearance-" + std::to_wstring(destination) + L".txt");
        { std::ofstream stream(file, std::ios::binary); stream << "retained-appearance-file"; }
        const auto identity = native_dock_return_cases::Identity(file);
        snowdesktop::LargeIconConfig original;
        original.columns = 2; original.rows = 2; original.shape = 3;
        original.contentScale = .47; original.opacity = .38; original.manualColor = 0x337799;
        auto& app = profile.Model(false, profile.root / (L"appearance-" + std::to_wstring(destination)));
        Access::SeedCompatAppearance(app, file, destination, original);
        Access::PrepareReload(app); Access::BindAsync(app);
        const auto key = ToUpperInvariant(file.wstring());
        Require(Find(app, key).largeIcon == original, "D11 starts with the independently specified complete large-icon configuration");
        Access::BeginModel(app, 0, 1);
        const auto target = Access::CompatAppearancePoint(app, destination);
        const auto ingress = Access::ObserveModelPreview(app, target, 0);
        if (destination == 2)
            Require(Access::Target(app) == Access::Dock(app) && Access::Region(app) != HitRegion::None &&
                Access::Region(app) != HitRegion::Blocked && Access::Region(app) != HitRegion::Handoff,
                "D11 original large icon reaches the actual Dock insertion target");
        else
            Require(!ingress.Empty() && ingress.action == DropAction::Move && ingress.targetWidget &&
                ingress.targetWidget->id == L"compat-target",
                "D11 original large icon enters the intended logical storage owner");
        Access::SubmitCompatAppearance(app);
        Wait([&] { return Access::FileDropIdle(app); }, "D11 actual owner transfer and refresh must finish");
        const auto stored = [&] {
            const auto& item = Find(app, key);
            Require(item.largeIcon == original && item.gridSpan.columns == 1 && item.gridSpan.rows == 1,
                "D11: entering logical storage preserves complete large-icon intent while using one ordinary cell");
            Require(Read(file) == "retained-appearance-file", "D11 logical storage cannot change the original file bytes");
        };
        stored(); Require(Access::Save(app), "save D11's retained stored configuration");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); stored();
        const POINT landing{950, 650};
        Access::BeginCompatAppearanceReturn(app, key, destination, landing);
        const auto preview = Access::ObserveModelPreview(app, landing, 0);
        Require(preview.landings.size() == 1, "D11 return must preview the one original item");
        Access::SubmitCompatAppearance(app);
        const auto restored = [&] {
            const auto& item = Find(app, key);
            Require(item.largeIcon == original && item.gridSpan.columns == 2 && item.gridSpan.rows == 2 &&
                Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item),
                "D11: returning to the desktop restores the original configuration and intended large span");
            Require(native_dock_return_cases::SameCell(item.gridCell, preview.landings.front().cell) &&
                item.gridSpan.columns == preview.landings.front().span.columns && item.gridSpan.rows == preview.landings.front().span.rows,
                "D11: returned large icon must fulfill its actual preview cell and span");
            const auto now = native_dock_return_cases::Identity(file);
            Require(now.dwVolumeSerialNumber == identity.dwVolumeSerialNumber && now.nFileIndexHigh == identity.nFileIndexHigh &&
                now.nFileIndexLow == identity.nFileIndexLow && CompareFileTime(&now.ftLastWriteTime, &identity.ftLastWriteTime) == 0,
                "D11: storage and return preserve physical file identity and modification time");
        };
        restored(); Require(Access::Save(app), "persist returned large-icon intent");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); restored();
        Access::StopModel(app); native_dock_return_cases::RemoveOwnedEntry(desktop, file);
    }
    std::cout << "3 native large-icon storage and return journeys passed\n";
}

inline void RunFolderHandoff()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const int depth : {1, 2})
    for (const DWORD keys : {DWORD{MK_CONTROL}, DWORD{MK_SHIFT}, DWORD{MK_ALT}})
    for (const bool asynchronous : {false, true})
    {
        native_drag_commit_cases::context = "D09 folder handoff depth=" + std::to_string(depth) +
            " keys=" + std::to_string(keys) + " async=" + std::to_string(asynchronous);
        const auto root = profile.root / (L"folder-handoff-" + std::to_wstring(scenes++));
        const auto directory = root / L"Destination";
        fs::create_directories(directory);
        const auto sourceFile = root / L"Source.txt";
        { std::ofstream stream(sourceFile, std::ios::binary); stream << "handoff-original"; }
        const auto first = depth == 1 ? desktop / L"Folder.lnk" : root / L"First.lnk";
        const auto shown = desktop / L"Folder.lnk";
        CreateLink(first, directory); if (depth == 2) CreateLink(shown, first);
        const auto originalLink = Read(shown);
        auto& app = profile.Model(false, root / L"layout");
        Access::PopulateModel(app, {shown}, 0); Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto key = ToUpperInvariant(shown.wstring());
        const auto point = Access::CompatFilePoint(app, key);
        auto source = ole_file_source_fixture::Make({sourceFile}, asynchronous);
        auto adapter = Access::ExternalAdapter(app);
        DWORD effect = 7;
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), keys | MK_LBUTTON, Access::ExternalPoint(point), &effect)),
            "D09: real files must enter the actual folder-shortcut hit target");
        effect = 7;
        Require(SUCCEEDED(adapter->DragOver(keys | MK_LBUTTON, Access::ExternalPoint(point), &effect)) && Access::Region(app) == HitRegion::Handoff,
            "D09: actual folder shortcut target must select its native Handoff route");
        effect = 7;
        Require(SUCCEEDED(adapter->Drop(source.Get(), keys, Access::ExternalPoint(point), &effect)),
            "D09: actual folder-shortcut handoff must execute the selected action");
        Wait([&] { return Access::FileDropIdle(app) && (!asynchronous || source->starts == source->ends) &&
            std::distance(fs::directory_iterator(directory), fs::directory_iterator{}) == 1; },
            "D09: selected folder-shortcut action must finish in the resolved original directory");
        const auto output = fs::directory_iterator(directory)->path();
        const auto check = [&] {
            Require(Read(shown) == originalLink && Outputs(desktop, {shown}).empty(),
                "D09: handoff keeps the original folder shortcut without desktop duplicates");
            Require(fs::exists(sourceFile) == (keys != MK_SHIFT), "D09: only the selected Move removes the original source");
            if (keys == MK_ALT) Require(fs::equivalent(LinkTarget(output), sourceFile), "D09: folder Handoff Link points to the original source");
            else Require(Read(output) == "handoff-original", "D09: resolved folder receives the original selected bytes");
            if (keys != MK_SHIFT) Require(Read(sourceFile) == "handoff-original", "D09: Copy and Link preserve source bytes");
            const auto& item = Find(app, key);
            Require(Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item) && Access::NoIngressPopup(app),
                "D09: handoff preserves the accessible desktop shortcut and creates no presentation window");
        };
        check(); Require(Access::Save(app), "save native folder-shortcut Handoff result");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); source.Reset(); native_dock_return_cases::RemoveOwnedEntry(desktop, shown);
    }
    std::cout << scenes << " native resolved folder-shortcut handoff and storage scenes passed\n";
}
}
