#pragma once

namespace native_dock_partition_cases
{
using namespace native_ole_file_cases;
inline void Check(DesktopApp& app, const std::vector<std::wstring>& expected, bool mapping,
    const std::vector<GridCell>& originalCells, const std::vector<fs::path>& files)
{
    const auto& entries = Access::DockEntries(app);
    Require(entries.size() == expected.size(), "D10: mixed Dock partition operation must retain exactly one entry per original identity");
    for (std::size_t index = 0; index < expected.size(); ++index)
        Require(entries[index].type == DockEntryType::DesktopItem && entries[index].keepOnDesktop == mapping &&
            PathsEqualInsensitive(entries[index].reference, expected[index]),
            "D10: mixed selection must land in the proper type partitions and retain the captured order inside each partition");
    for (std::size_t index = 0; index < files.size(); ++index)
    {
        const auto& item = Find(app, ToUpperInvariant(files[index].wstring()));
        if (mapping) Require(native_dock_return_cases::SameCell(item.gridCell, originalCells[index]) && Access::HasVisibleIcon(app, item),
            "U07: reordering or creating Dock mappings must preserve the original desktop positions");
        else Require(item.gridCell.pageId == kDockPageId && !Access::HasVisibleIcon(app, item),
            "U07: exclusive fixed entries retain their logical Dock ownership without duplicate free desktop icons");
    }
    std::vector<std::wstring> accessible;
    for (const auto& slot : Access::Dock(app)->GetSlots())
        if (const auto* item = dynamic_cast<const DockEntryItem*>(slot->GetItem())) accessible.push_back(item->GetReference());
    Require(accessible.size() == expected.size() && std::equal(accessible.begin(), accessible.end(), expected.begin(), expected.end(), PathsEqualInsensitive),
        "D10: actual Dock slots must expose every partition entry in the persisted order");
    Require(Access::NoDockWindows(app), "D10: partition model must not allocate any desktop or persistent Dock window");
}
inline void Run()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop; fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const bool pinned : {true, false})
    for (const bool mapping : {false, true})
    for (const bool tail : {false, true})
    for (const bool multiple : {false, true})
    for (const bool rebuild : {false, true})
    {
        // Desktop ingress contains the same four interleaved semantic types;
        // the Dock-source mode separately covers first/last insertion boundaries.
        if (!pinned && tail) continue;
        native_drag_commit_cases::context = "A27 mixed Dock partitions pinned=" + std::to_string(pinned) +
            " mapping=" + std::to_string(mapping) + " tail=" + std::to_string(tail) +
            " multiple=" + std::to_string(multiple) + " rebuild=" + std::to_string(rebuild);
        const auto prefix = L"dock-partitions-" + std::to_wstring(scenes++);
        std::vector<fs::path> files;
        for (int index = 0; index < 6; ++index)
        {
            const auto path = desktop / (prefix + L"-" + std::to_wstring(index) + (index < 3 ? L".txt" : L"-folder"));
            if (index < 3) { std::ofstream stream(path, std::ios::binary); stream << "partition-" << index; }
            else { fs::create_directory(path); std::ofstream stream(path / L"nested.txt", std::ios::binary); stream << "partition-" << index; }
            files.push_back(path);
        }
        std::vector<native_dock_return_cases::Original> originals;
        for (std::size_t index = 0; index < files.size(); ++index)
        {
            originals.push_back({files[index], index < 3 ? Read(files[index]) : std::string{}, native_dock_return_cases::Identity(files[index])});
            if (index >= 3) originals.push_back({files[index] / L"nested.txt", Read(files[index] / L"nested.txt"), native_dock_return_cases::Identity(files[index] / L"nested.txt")});
        }
        auto& app = profile.Model(false, profile.root / prefix); Access::SeedDockPartitions(app, files, pinned, mapping);
        Access::PrepareReload(app); Access::BindAsync(app); native_drag_async_reload_cases::Refresh(app);
        if (pinned) Wait([&] { return Access::DockPartitionFolderCount(app) == 3; },
            "real Shell classification must deliver the existing main/file partition before capturing a mixed reorder");
        std::vector<GridCell> originalCells; std::vector<std::wstring> keys;
        for (const auto& file : files) { keys.push_back(ToUpperInvariant(file.wstring())); originalCells.push_back(Find(app, keys.back()).gridCell); }
        std::vector<std::size_t> selected;
        std::vector<std::size_t> order;
        if (pinned)
        {
            selected = multiple ? (tail ? std::vector<std::size_t>{0, 1, 3, 4} : std::vector<std::size_t>{1, 2, 4, 5})
                : std::vector<std::size_t>{1, 4};
            order = multiple ? (tail ? std::vector<std::size_t>{2, 0, 1, 5, 3, 4} : std::vector<std::size_t>{1, 2, 0, 4, 5, 3})
                : (tail ? std::vector<std::size_t>{0, 2, 1, 3, 5, 4} : std::vector<std::size_t>{1, 0, 2, 4, 3, 5});
            std::vector<std::wstring> capture; for (const auto index : selected) capture.push_back(keys[index]);
            const auto captured = Access::BeginDockFiles(app, capture, {150, 250}, 0);
            Require(captured.entries.size() == selected.size(), "A27: real Dock must capture selected main and folder entries together");
        }
        else
        {
            // Native desktop capture order is supplied explicitly by the model
            // enumeration, interleaving file/folder identities on purpose.
            selected = multiple ? std::vector<std::size_t>{2, 5, 0, 3} : std::vector<std::size_t>{2, 5};
            auto ordered = files;
            std::vector<fs::path> input; for (const auto index : selected) input.push_back(ordered[index]);
            Access::PopulateModel(app, input, 0); Access::SeedDock(app); Access::BeginModel(app, 0, input.size());
            // Only captured members enter an initially empty Dock. Unselected
            // baseline files remain actual filesystem/model enumeration inputs.
            order = multiple ? std::vector<std::size_t>{2, 0, 5, 3} : std::vector<std::size_t>{2, 5};
        }
        if (rebuild) Access::RebuildDuringMenu(app);
        Access::CommitDockPartition(app, tail ? Access::DockEntries(app).size() : 0, mapping ? MK_CONTROL : 0);
        native_drag_async_reload_cases::Refresh(app);
        Wait([&] { return Access::DockPartitionFolderCount(app) == (pinned ? 3u : multiple ? 2u : 1u); },
            "actual Shell folder classification must reach the native mixed Dock outputs");
        std::vector<std::wstring> expected; for (const auto index : order) expected.push_back(keys[index]);
        if (pinned) Check(app, expected, mapping, originalCells, files);
        else
        {
            const auto& entries = Access::DockEntries(app);
            Require(entries.size() == expected.size() && std::equal(entries.begin(), entries.end(), expected.begin(), expected.end(), [&](const auto& entry, const auto& key) {
                return entry.keepOnDesktop == mapping && PathsEqualInsensitive(entry.reference, key);
            }), "D10: interleaved desktop capture must preserve capture order separately in main and file sections");
        }
        native_dock_return_cases::OriginalFiles(originals);
        Require(Outputs(desktop, files).empty(), "D10: partition changes must not copy, link or move physical source files");
        Require(Access::Save(app), "save real native mixed partition ordering"); Access::Load(app); native_drag_async_reload_cases::Refresh(app, true);
        if (pinned) Check(app, expected, mapping, originalCells, files);
        else
        {
            const auto& entries = Access::DockEntries(app);
            Require(entries.size() == expected.size() && std::equal(entries.begin(), entries.end(), expected.begin(), expected.end(), [&](const auto& entry, const auto& key) {
                return entry.keepOnDesktop == mapping && PathsEqualInsensitive(entry.reference, key);
            }), "D10: native mixed ingress partition order must survive actual disk reload");
        }
        native_dock_return_cases::OriginalFiles(originals); Access::StopModel(app);
        for (const auto& file : files) native_dock_return_cases::RemoveOwnedEntry(desktop, file);
    }
    for (const bool mapping : {false, true})
    for (const bool lastFixed : {false, true})
    for (const bool after : {false, true})
    for (const bool multiple : {false, true})
    {
        native_drag_commit_cases::context = "A27 folder over fixed area mapping=" + std::to_string(mapping) +
            " lastFixed=" + std::to_string(lastFixed) + " after=" + std::to_string(after) +
            " multiple=" + std::to_string(multiple);
        const auto prefix = L"dock-leading-folder-" + std::to_wstring(scenes++);
        std::vector<fs::path> files;
        for (int index = 0; index < 6; ++index)
        {
            const auto file = desktop / (prefix + L"-" + std::to_wstring(index) + (index < 3 ? L".txt" : L"-folder"));
            if (index < 3) { std::ofstream stream(file, std::ios::binary); stream << "leading-" << index; }
            else { fs::create_directory(file); std::ofstream stream(file / L"nested.txt", std::ios::binary); stream << "leading-" << index; }
            files.push_back(file);
        }
        std::vector<native_dock_return_cases::Original> originals;
        for (std::size_t index = 0; index < files.size(); ++index)
        {
            originals.push_back({files[index], index < 3 ? Read(files[index]) : std::string{}, native_dock_return_cases::Identity(files[index])});
            if (index >= 3) originals.push_back({files[index] / L"nested.txt", Read(files[index] / L"nested.txt"), native_dock_return_cases::Identity(files[index] / L"nested.txt")});
        }
        auto& app = profile.Model(false, profile.root / prefix); Access::SeedDockPartitions(app, files, true, mapping);
        Access::PrepareReload(app); Access::BindAsync(app); native_drag_async_reload_cases::Refresh(app);
        Wait([&] { return Access::DockPartitionFolderCount(app) == 3; },
            "actual Shell classification must separate main and file sections before crossing the fixed area");
        std::vector<std::wstring> keys; std::vector<GridCell> originalCells;
        for (const auto& file : files) { keys.push_back(ToUpperInvariant(file.wstring())); originalCells.push_back(Find(app, keys.back()).gridCell); }
        const std::vector<std::wstring> selected = multiple ? std::vector<std::wstring>{keys[4], keys[5]}
            : std::vector<std::wstring>{keys[5]};
        const auto captured = Access::BeginDockFiles(app, selected, {150, 250}, 0);
        Require(captured.entries.size() == selected.size(), "folder-only native capture must retain every selected identity");
        Access::CommitDockFolderAtFixedEntry(app, keys[lastFixed ? 2 : 0], after);
        native_drag_async_reload_cases::Refresh(app);
        const auto order = multiple ? std::vector<std::size_t>{0, 1, 2, 4, 5, 3}
            : std::vector<std::size_t>{0, 1, 2, 5, 3, 4};
        std::vector<std::wstring> expected; for (const auto index : order) expected.push_back(keys[index]);
        Check(app, expected, mapping, originalCells, files);
        native_dock_return_cases::OriginalFiles(originals);
        Require(Outputs(desktop, files).empty(), "fixed-area folder insertion must not create, move or copy any physical source");
        Require(Access::Save(app), "save native folder insertion at the file section beginning");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true);
        Check(app, expected, mapping, originalCells, files);
        native_dock_return_cases::OriginalFiles(originals); Access::StopModel(app);
        for (const auto& file : files) native_dock_return_cases::RemoveOwnedEntry(desktop, file);
    }
    std::cout << scenes << " native Dock mixed and fixed-area folder partition capture/commit/storage scenes passed\n";
}
}
