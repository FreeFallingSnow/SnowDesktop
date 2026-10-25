#pragma once

namespace native_dock_running_cases
{
using namespace native_ole_file_cases;
inline void CreateLink(const fs::path& linkPath, const fs::path& target)
{
    ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persist;
    Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link.As(&persist)) && SUCCEEDED(persist->Save(linkPath.c_str(), TRUE)),
        "create real matching desktop application shortcut for generated Dock pin tests");
}
inline void Run()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop; fs::create_directories(desktop);
    std::size_t scenes = 0;
    // Explorer may create its default launcher or a recognized folder link.
    // Unknown folders and unmatched ordinary applications must create nothing.
    // The Shell lookup result is supplied; queue, pin, files and reload are real.
    for (const int kind : {0, 1, 2, 3, 4, 5, 6, 7})
    for (const bool mapping : {false, true})
    for (const bool retired : {false, true})
    {
        const bool matched = kind == 1 || kind == 3 || kind == 6 || kind == 7, explorer = kind >= 2;
        const bool explorerFixed = kind == 4 || kind == 6, folderKnown = kind != 5;
        native_drag_commit_cases::context = "A28 running Dock queue kind=" + std::to_string(kind) +
            " mapping=" + std::to_string(mapping) + " retired=" + std::to_string(retired);
        const auto prefix = L"running-dock-" + std::to_wstring(scenes++);
        const auto directory = profile.root / prefix; fs::create_directory(directory);
        const auto executable = directory / (explorer ? L"Current-Explorer-Folder" : L"FixtureApplication.exe");
        if (explorer) fs::create_directory(executable);
        { std::ofstream stream(explorer ? executable / L"nested.txt" : executable, std::ios::binary); stream << "local source identity, never launched"; }
        const auto original = desktop / (prefix + L"-Application.lnk");
        const auto sentinel = desktop / (prefix + L"-Sentinel.txt");
        { std::ofstream stream(sentinel, std::ios::binary); stream << "unrelated desktop"; }
        if (matched)
        {
            CreateLink(original, executable);
            Require(fs::equivalent(LinkTarget(original), executable), "running fixture's existing launcher must resolve to the exact original application or directory");
        }
        std::vector<fs::path> existing{sentinel}; if (matched) existing.push_back(original);
        if (kind == 7)
        {
            const auto duplicate = desktop / (prefix + L"-Duplicate.lnk");
            CreateLink(duplicate, executable);
            existing.push_back(duplicate);
        }
        std::vector<native_dock_return_cases::Original> originals;
        originals.push_back({executable, explorer ? std::string{} : Read(executable), native_dock_return_cases::Identity(executable)});
        if (explorer) originals.push_back({executable / L"nested.txt", Read(executable / L"nested.txt"), native_dock_return_cases::Identity(executable / L"nested.txt")});
        for (const auto& path : existing) originals.push_back({path, Read(path), native_dock_return_cases::Identity(path)});
        auto& app = profile.Model(false, directory / L"layout");
        Access::PrepareRunningDock(app, existing); Access::PrepareReload(app); Access::BindAsync(app);
        Require(Access::Save(app), "save empty Dock before a generated running pin request");
        const auto saved = Read(Access::LayoutPath(app));
        const auto before = matched ? Find(app, ToUpperInvariant(original.wstring())).gridCell : GridCell{};
        const auto completed = Access::QueueNativeRunningDockPin(app, executable, matched, mapping, retired, explorer,
            explorerFixed, folderKnown);
        Wait([&] { return completed.expired() && Access::FileDropIdle(app); }, "actual generated Dock lookup, queued commit and model delivery must finish");
        const bool generated = kind == 2 || explorerFixed || kind == 7;
        const bool accepted = (matched || generated) && !retired;
        const auto outputs = Outputs(desktop, existing);
        Require(outputs.size() == static_cast<std::size_t>(accepted && generated),
            "D10: only live Explorer with a defined target may generate one shortcut by dragging");
        const auto& entries = Access::DockEntries(app);
        Require(entries.size() == static_cast<std::size_t>(accepted), "D10: accept existing desktop entries or defined Explorer targets, reject unknown or retired sources");
        if (accepted)
        {
            const auto pinned = generated ? outputs.front() : original;
            const bool keepOnDesktop = matched && !generated && mapping;
            if (explorer && !explorerFixed) Wait([&] { return Access::DockPartitionFolderCount(app) == 1; },
                "D10: the exact Explorer folder shortcut must reach the real file partition after Shell classification");
            else Require(Access::DockPartitionFolderCount(app) == 0, "Explorer itself belongs in Fixed, never Files");
            if (generated)
            {
                auto expected = executable;
                if (explorerFixed)
                {
                    wchar_t windows[32768]{};
                    Require(GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows))) != 0, "read system Explorer target");
                    expected = fs::path(windows) / L"explorer.exe";
                }
                Require(pinned.extension() == L".lnk" && fs::equivalent(LinkTarget(pinned), expected),
                    "Explorer pin must create a real link to the exact folder or the default Explorer executable");
            }
            Require(entries.front().type == DockEntryType::DesktopItem && entries.front().keepOnDesktop == keepOnDesktop &&
                PathsEqualInsensitive(entries.front().reference, pinned.wstring()), "U07: reuse original identity and mapping role, or create an exclusive Explorer pin");
            const auto& item = Find(app, ToUpperInvariant(pinned.wstring()));
            Require(keepOnDesktop ? native_dock_return_cases::SameCell(item.gridCell, before) && Access::HasVisibleIcon(app, item)
                : item.gridCell.pageId == kDockPageId && !Access::HasVisibleIcon(app, item),
                "U07: only exclusive pinning may move the original logical entry out of the free desktop");
            if ((kind == 2 || kind == 7) && !mapping)
            {
                const auto repeated = Access::QueueNativeRunningDockPin(app, executable, false, false, false, true);
                Wait([&] { return repeated.expired() && Access::FileDropIdle(app); }, "repeated folder pin completes through the real queue");
                Require(Outputs(desktop, existing).size() == 1 && Access::DockEntries(app).size() == 1,
                    "repeating an Explorer folder pin must reuse its created identity without duplicate files or Dock entries");
            }
            Require(Access::Save(app), "save actual existing-entry running pin"); Access::Load(app); native_drag_async_reload_cases::Refresh(app, true);
            Require(Access::DockEntries(app).size() == 1 && PathsEqualInsensitive(Access::DockEntries(app).front().reference, pinned.wstring()) &&
                Access::DockEntries(app).front().keepOnDesktop == keepOnDesktop, "U07: running pin identity and role must survive disk reload");
            Wait([&] { return Access::DockPartitionFolderCount(app) == static_cast<std::size_t>(explorer && !explorerFixed); },
                "Explorer default launcher and folder must retain their distinct partitions after reload");
        }
        else Require(Read(Access::LayoutPath(app)) == saved, "B17: rejected or retired generated pin cannot change saved layout");
        native_dock_return_cases::OriginalFiles(originals);
        Require(Access::NoDockWindows(app), "generated pin model cannot create desktop or persistent Dock presentation windows");
        Access::StopModel(app);
        for (const auto& path : outputs) native_dock_return_cases::RemoveOwnedEntry(desktop, path);
        for (const auto& path : existing) native_dock_return_cases::RemoveOwnedEntry(desktop, path);
    }
    std::cout << scenes << " generated running Dock queue, rejection and existing-entry storage scenes passed\n";
}
}
