#pragma once

namespace native_content_dock_cases
{
using namespace native_ole_file_cases;

inline void CheckDockRelations(DesktopApp& app, const std::vector<fs::path>& files,
    const std::vector<GridCell>& cells, bool frequent, std::size_t count, bool removed)
{
    Require(Access::NoDockWindows(app), "Dock relation tests cannot create a desktop or persistent Dock presentation window");
    for (std::size_t index = 0; index < files.size(); ++index)
    {
        const auto key = ToUpperInvariant(files[index].wstring());
        const auto& item = Find(app, key);
        Require(native_dock_return_cases::SameCell(item.gridCell, cells[index]),
            "B19: removing a Dock mapping or frequent relation must retain every original desktop location");
        if (index < 2)
        {
            Require(Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item),
                "B19: original desktop entries must remain accessible after Dock relation removal");
            const bool present = !removed || index >= count;
            if (frequent)
                Require(Access::HasDockUsage(app, key) == present,
                    "B19: only the captured frequent relations may be removed");
            else
                Require(std::count_if(Access::DockEntries(app).begin(), Access::DockEntries(app).end(), [&](const auto& entry) {
                    return entry.type == DockEntryType::DesktopItem && entry.keepOnDesktop && PathsEqualInsensitive(entry.reference, key);
                }) == static_cast<int>(present), "B19: only the captured Dock mappings may be removed");
        }
    }
    const auto fixed = ToUpperInvariant(files[2].wstring());
    Require(std::count_if(Access::DockEntries(app).begin(), Access::DockEntries(app).end(), [&](const auto& entry) {
        return entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop && PathsEqualInsensitive(entry.reference, fixed);
    }) == 1, "B19: an unrelated exclusive fixed entry must retain its distinct role and identity");
}

inline void RunDockRelations()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    std::size_t scenes = 0;
    // The index-changing rebuild is first so the original rebind fault fails
    // before any removal can touch the wrong entry. Mapping and ordinary
    // rebuild sequences remain in the same unfiltered candidate input domain.
    for (const bool frequent : {true, false})
    for (const int sequence : {2, 0, 1})
    for (const std::size_t count : {1u, 2u})
    for (const bool cancel : {false, true})
    {
        native_drag_commit_cases::context = "A25 Dock relation frequent=" + std::to_string(frequent) +
            " sequence=" + std::to_string(sequence) + " count=" + std::to_string(count) + " cancel=" + std::to_string(cancel);
        const auto prefix = L"dock-relation-" + std::to_wstring(scenes++);
        const std::vector<fs::path> files{desktop / (prefix + L"-A.url"), desktop / (prefix + L"-B.url"),
            desktop / (prefix + L"-Fixed.txt"), desktop / (prefix + L"-Sentinel.txt")};
        for (std::size_t index = 0; index < files.size(); ++index)
        {
            std::ofstream stream(files[index], std::ios::binary);
            stream << (index < 2 ? "[InternetShortcut]\r\nURL=https://example.invalid/" : "unrelated-") << index << "\r\n";
        }
        std::vector<native_dock_return_cases::Original> originals;
        for (const auto& file : files) originals.push_back({file, Read(file), native_dock_return_cases::Identity(file)});
        auto& app = profile.Model(false, profile.root / prefix);
        Access::SeedDockRelations(app, files, frequent); Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        std::vector<GridCell> cells;
        for (const auto& file : files) cells.push_back(Find(app, ToUpperInvariant(file.wstring())).gridCell);
        Require(Access::Save(app), "save actual Dock mapping/frequent baseline");
        const auto layout = Read(Access::LayoutPath(app));
        const auto usage = Read(Access::DockUsagePath(app));
        std::vector<std::wstring> keys;
        for (std::size_t index = 0; index < count; ++index) keys.push_back(ToUpperInvariant(files[index].wstring()));
        const POINT point{150, 150};
        const auto captured = Access::BeginDockRelations(app, keys, frequent, point);
        Require(captured.entries.size() == count && captured.entries.front().fromDock,
            "A25: actual Dock selection must capture the original mapping or frequent identities");
        if (sequence) Access::RebuildDockRelationSource(app, sequence == 2);
        Require(Access::DockRelationBindingsCurrent(app, keys),
            "S07: rebuilding or reordering enumeration must retain live Dock wrappers for the originally captured identities");
        if (cancel) Access::EndActualGesture(app);
        else Require(Access::CommitDockRelationRemoval(app, point), "B19: actual host drag-out mutation must remove the captured Dock relations");
        Wait([&] { return Access::FileDropIdle(app); }, "Dock relation removal must finish its actual model work");
        if (cancel)
            Require(Read(Access::LayoutPath(app)) == layout && Read(Access::DockUsagePath(app)) == usage,
                "B17: cancelling captured Dock relation removal cannot write layout or usage history");
        CheckDockRelations(app, files, cells, frequent, count, !cancel);
        native_dock_return_cases::OriginalFiles(originals);
        Require(Outputs(desktop, files).empty(), "B19: removing Dock relations cannot create copies, shortcuts or additional files");
        Access::Load(app); Access::ReloadDockUsage(app); native_drag_async_reload_cases::Refresh(app, true);
        CheckDockRelations(app, files, cells, frequent, count, !cancel);
        native_dock_return_cases::OriginalFiles(originals);
        Access::StopModel(app);
        for (const auto& file : files) native_dock_return_cases::RemoveOwnedEntry(desktop, file);
    }
    std::cout << scenes << " actual Dock mapping/frequent capture, removal, cancellation and storage scenes passed\n";
}
}
