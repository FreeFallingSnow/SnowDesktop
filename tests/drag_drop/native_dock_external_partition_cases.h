#pragma once
namespace native_dock_external_partition_cases
{
using namespace native_ole_file_cases;
inline void RunFolderPreviews()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const bool shortcut : {false, true})
    for (const bool external : {false, true})
    for (const bool asynchronous : {false, true})
    {
        if (!external && asynchronous) continue;
        native_drag_commit_cases::context = "cold folder preview shortcut=" + std::to_string(shortcut) +
            " external=" + std::to_string(external) + " async=" + std::to_string(asynchronous);
        const auto prefix = L"folder-preview-" + std::to_wstring(scenes++);
        const auto directory = profile.root / prefix; fs::create_directory(directory);
        const auto target = directory / L"target"; fs::create_directory(target);
        const auto sourcePath = desktop / (prefix + (shortcut ? L".lnk" : L"-folder"));
        if (shortcut)
        {
            ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persisted;
            Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
                SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link.As(&persisted)) &&
                SUCCEEDED(persisted->Save(sourcePath.c_str(), TRUE)), "create the real folder-shortcut preview source");
        }
        else fs::create_directory(sourcePath);
        const auto fixed = desktop / (prefix + L"-fixed.txt");
        { std::ofstream stream(fixed); stream << "fixed source"; }
        const auto folder = desktop / (prefix + L"-existing"); fs::create_directory(folder);
        const std::vector<fs::path> files{sourcePath, fixed, folder};
        auto& app = profile.Model(false, directory / L"layout");
        Access::SeedColdFolderPreview(app, files); Access::BindAsync(app);
        Require(Access::ColdSourceStillUnknown(app, sourcePath),
            "first preview must start without a completed folder source cache or warm-up drag");
        const RECT fixedSlot = Access::Dock(app)->GetSlots().front()->GetBounds();
        const POINT point{fixedSlot.left + 2, fixedSlot.top + 2};
        auto source = external ? ole_file_source_fixture::Make({sourcePath}, asynchronous) : ComPtr<ole_file_source_fixture::Source>{};
        auto adapter = external ? Access::ExternalAdapter(app) : ComPtr<OleDragDropAdapter>{};
        for (int attempt = 0; attempt != 2; ++attempt)
        {
            if (external)
            {
                DWORD effect = 7;
                Require(SUCCEEDED(adapter->DragEnter(source.Get(), MK_LBUTTON | MK_ALT, Access::ExternalPoint(point), &effect)),
                    "real OLE folder/shortcut Enter must admit the preview");
                effect = 7;
                Require(SUCCEEDED(adapter->DragOver(MK_LBUTTON | MK_ALT, Access::ExternalPoint(point), &effect)) && effect == 4 &&
                    Access::ExternalSummary(app).foldersOnly,
                    "external folders and shortcuts must identify the file partition before Drop");
                Require(!asynchronous || (source->identityReads > 0 && source->fileReads == 0 &&
                    source->dataReads == 0 && source->starts == 0),
                    "async folder preview may read selection identity but cannot render or start delayed content");
            }
            else
            {
                Access::BeginModel(app, 0, 1); Access::Resolve(app, point);
            }
            const auto range = Access::DockPartitionPreviewRange(app);
            Require(Access::Target(app) == Access::Dock(app) && range.begin == 1 && range.end == 2 &&
                Access::DockPartitionPreviewIndex(app) == 1,
                "first and repeated folder/shortcut landings over a fixed icon must preview the file section's leading boundary");
            Access::EndPartitionPreview(app);
        }
        Require(Access::NoDockWindows(app) && (!external || !source->wrongApartment),
            "folder preview must retain native source ownership without presentation windows");
        Access::StopModel(app); source.Reset(); adapter.Reset();
        for (const auto& file : files) native_dock_return_cases::RemoveOwnedEntry(desktop, file);
    }
    std::cout << scenes << " cold internal/external directory and folder-shortcut preview scenes passed twice\n";
}
inline void Run(bool asynchronousFirst = false)
{
    RunFolderPreviews();
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop; fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const bool asynchronous : asynchronousFirst ? std::array{true, false} : std::array{false, true})
    {
        native_drag_commit_cases::context = "A27 external mixed tail async=" + std::to_string(asynchronous);
        const auto prefix = L"external-mixed-" + std::to_wstring(scenes++);
        const auto directory = profile.root / prefix; fs::create_directory(directory);
        std::vector<fs::path> baseline;
        for (int index = 0; index < 6; ++index)
        {
            const auto path = desktop / (prefix + L"-" + std::to_wstring(index) + (index < 3 ? L".txt" : L"-folder"));
            if (index < 3) { std::ofstream stream(path, std::ios::binary); stream << "baseline-" << index; }
            else { fs::create_directory(path); std::ofstream stream(path / L"nested.txt", std::ios::binary); stream << "baseline-" << index; }
            baseline.push_back(path);
        }
        const std::vector<fs::path> files{directory / L"Source.txt", directory / L"SourceFolder"};
        { std::ofstream stream(files[0], std::ios::binary); stream << "external-file-original"; }
        fs::create_directory(files[1]); { std::ofstream stream(files[1] / L"nested.txt", std::ios::binary); stream << "external-folder-original"; }
        std::vector<native_dock_return_cases::Original> originals;
        for (std::size_t index = 0; index < baseline.size(); ++index)
        {
            originals.push_back({baseline[index], index < 3 ? Read(baseline[index]) : std::string{}, native_dock_return_cases::Identity(baseline[index])});
            if (index >= 3) originals.push_back({baseline[index] / L"nested.txt", Read(baseline[index] / L"nested.txt"), native_dock_return_cases::Identity(baseline[index] / L"nested.txt")});
        }
        originals.push_back({files[0], Read(files[0]), native_dock_return_cases::Identity(files[0])});
        originals.push_back({files[1], {}, native_dock_return_cases::Identity(files[1])});
        originals.push_back({files[1] / L"nested.txt", Read(files[1] / L"nested.txt"), native_dock_return_cases::Identity(files[1] / L"nested.txt")});
        auto& app = profile.Model(false, directory / L"layout");
        Access::SeedDockPartitions(app, baseline, true, false); Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        Wait([&] { return Access::DockPartitionFolderCount(app) == 3; }, "external mixed source requires the real existing file partition");
        RECT target{};
        for (const auto& slot : Access::Dock(app)->GetSlots())
            if (const auto* entry = dynamic_cast<const DockEntryItem*>(slot->GetItem());
                entry && PathsEqualInsensitive(entry->GetReference(), baseline.back().wstring())) target = slot->GetBounds();
        Require(!IsRectEmpty(&target), "external mixed source requires an actual last file-partition slot");
        const POINT point{target.right - 2, target.top + 2}; // Slot edge, outside its handoff interior.
        auto source = ole_file_source_fixture::Make(files, asynchronous);
        auto adapter = Access::ExternalAdapter(app);
        DWORD effect = 7; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), MK_LBUTTON | MK_ALT, Access::ExternalPoint(point), &effect)), "actual external mixed Enter");
        effect = 7; Access::FreshExternalDwell(app);
        const auto over = adapter->DragOver(MK_LBUTTON | MK_ALT, Access::ExternalPoint(point), &effect);
        Require(SUCCEEDED(over) &&
            Access::Target(app) == Access::Dock(app) && Access::Region(app) == HitRegion::SortAfter,
            "D10: external mixed source must hit the actual last file slot's trailing edge");
        Require(!asynchronous || (source->starts == 0 && source->dataReads == 0), "external mixed hover cannot render an asynchronous producer");
        effect = 7; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->Drop(source.Get(), MK_ALT, Access::ExternalPoint(point), &effect)) && effect == 4,
            "B06: external mixed Dock input acknowledges the explicit persistent Link action");
        Wait([&] { return Access::FileDropIdle(app) && (!asynchronous || source->starts == source->ends) && Outputs(desktop, baseline).size() == 2; },
            "actual external mixed Link operation, source completion and native output publication must finish");
        const auto outputs = Outputs(desktop, baseline);
        fs::path fileLink, folderLink;
        for (const auto& output : outputs)
        {
            Require(_wcsicmp(output.extension().c_str(), L".lnk") == 0, "B06: each mixed external entry produces a real persistent shortcut");
            const auto targetPath = LinkTarget(output);
            if (fs::equivalent(targetPath, files[0])) fileLink = output;
            if (fs::equivalent(targetPath, files[1])) folderLink = output;
        }
        Require(!fileLink.empty() && !folderLink.empty() && fileLink != folderLink, "D10: complete external file and directory identities must each produce exactly one reference");
        Wait([&] { return Access::DockPartitionFolderCount(app) == 4; }, "external mixed folder shortcut must classify into the real file partition");
        const std::vector<fs::path> expected{baseline[0], baseline[1], baseline[2], fileLink, baseline[3], baseline[4], baseline[5], folderLink};
        const auto check = [&] {
            const auto& entries = Access::DockEntries(app);
            Require(entries.size() == expected.size() && std::equal(entries.begin(), entries.end(), expected.begin(), expected.end(), [&](const auto& entry, const auto& path) {
                return entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop && PathsEqualInsensitive(entry.reference, path.wstring());
            }), "D10: mixed external files must append to each type partition's captured trailing boundary");
            native_dock_return_cases::OriginalFiles(originals);
            Require(!source->wrongApartment && !source->fileReadBeforeStart && Access::NoDockWindows(app), "D10: mixed external completion preserves physical sources, COM lifetime and windowless ownership");
        };
        check(); Require(Access::Save(app), "save actual external mixed partition tail insertion");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); source.Reset();
        for (const auto& output : outputs) native_owned_content_cases::RemoveOwnedOutput(desktop, output);
        for (const auto& file : baseline) native_dock_return_cases::RemoveOwnedEntry(desktop, file);
    }
    std::cout << scenes << " actual external mixed file/folder partition tail and storage scenes passed\n";
}
}
