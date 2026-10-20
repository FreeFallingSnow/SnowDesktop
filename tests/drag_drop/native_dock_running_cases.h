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
    // Unmatched app, matched app, unmatched Explorer folder, existing folder
    // shortcut: the last source retains its real identity and file partition.
    for (const int kind : {0, 1, 2, 3})
    for (const bool mapping : {false, true})
    for (const bool retired : {false, true})
    {
        const bool matched = kind == 1 || kind == 3, explorer = kind >= 2;
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
        std::vector<native_dock_return_cases::Original> originals;
        originals.push_back({executable, explorer ? std::string{} : Read(executable), native_dock_return_cases::Identity(executable)});
        if (explorer) originals.push_back({executable / L"nested.txt", Read(executable / L"nested.txt"), native_dock_return_cases::Identity(executable / L"nested.txt")});
        for (const auto& path : existing) originals.push_back({path, Read(path), native_dock_return_cases::Identity(path)});
        auto& app = profile.Model(false, directory / L"layout");
        Access::PrepareRunningDock(app, existing); Access::PrepareReload(app); Access::BindAsync(app);
        Require(Access::Save(app), "save empty Dock before a generated running pin request");
        const auto saved = Read(Access::LayoutPath(app));
        const auto before = matched ? Find(app, ToUpperInvariant(original.wstring())).gridCell : GridCell{};
        const auto completed = Access::QueueNativeRunningDockPin(app, executable, matched, mapping, retired, explorer);
        Wait([&] { return completed.expired() && Access::FileDropIdle(app); }, "actual generated Dock lookup, queued commit and model delivery must finish");
        const bool accepted = matched && !retired;
        Require(Outputs(desktop, existing).empty(), "D10: a running item without a matching desktop entry cannot create a new shortcut by dragging");
        const auto& entries = Access::DockEntries(app);
        Require(entries.size() == static_cast<std::size_t>(accepted), "D10: only a live running application with a matching desktop entry may be pinned");
        if (accepted)
        {
            if (explorer) Wait([&] { return Access::DockPartitionFolderCount(app) == 1; },
                "D10: a matched folder shortcut must reach the real file partition after Shell classification");
            Require(entries.front().type == DockEntryType::DesktopItem && entries.front().keepOnDesktop == mapping &&
                PathsEqualInsensitive(entries.front().reference, original.wstring()), "U07: generated pin must retain the original desktop identity and requested mapping role");
            const auto& item = Find(app, ToUpperInvariant(original.wstring()));
            Require(mapping ? native_dock_return_cases::SameCell(item.gridCell, before) && Access::HasVisibleIcon(app, item)
                : item.gridCell.pageId == kDockPageId && !Access::HasVisibleIcon(app, item),
                "U07: only exclusive pinning may move the original logical entry out of the free desktop");
            Require(Access::Save(app), "save actual existing-entry running pin"); Access::Load(app); native_drag_async_reload_cases::Refresh(app, true);
            Require(Access::DockEntries(app).size() == 1 && PathsEqualInsensitive(Access::DockEntries(app).front().reference, original.wstring()) &&
                Access::DockEntries(app).front().keepOnDesktop == mapping, "U07: existing running pin identity and role must survive disk reload");
        }
        else Require(Read(Access::LayoutPath(app)) == saved, "B17: rejected or retired generated pin cannot change saved layout");
        native_dock_return_cases::OriginalFiles(originals);
        Require(Access::NoDockWindows(app), "generated pin model cannot create desktop or persistent Dock presentation windows");
        Access::StopModel(app);
        for (const auto& path : existing) native_dock_return_cases::RemoveOwnedEntry(desktop, path);
    }
    std::cout << scenes << " generated running Dock queue, rejection and existing-entry storage scenes passed\n";
}
}
