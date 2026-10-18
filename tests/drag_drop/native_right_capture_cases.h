#pragma once

namespace native_right_capture_cases
{
using namespace native_ole_file_cases;
inline void Run()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const int source : {0, 1, 2, 3}) // desktop / collection / files / mapping
    for (const bool keepOriginal : {true, false})
    for (const int sequence : {2, 0, 1}) // active rebuild first / menu selection only / menu rebuild
    {
        native_drag_commit_cases::context = "A24 source=" + std::to_string(source) + " keepOriginal=" + std::to_string(keepOriginal) +
            " sequence=" + std::to_string(sequence);
        const auto root = profile.root / (L"right-capture-" + std::to_wstring(scenes++));
        const auto directory = source == 3 ? root / L"mapped" : desktop;
        fs::create_directories(directory);
        const std::vector<fs::path> files{directory / L"A.txt", directory / L"B.txt"};
        { std::ofstream stream(files[0], std::ios::binary); stream << "captured-A"; }
        { std::ofstream stream(files[1], std::ios::binary); stream << "uncaptured-B"; }
        const auto a = native_dock_return_cases::Identity(files[0]), b = native_dock_return_cases::Identity(files[1]);
        auto& app = profile.Model(false, root / L"layout");
        Access::SeedMenuFileSource(app, files, source); Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto originalWidgets = Access::Widgets(app);
        Require(Access::Save(app), "save native menu-capture baseline");
        const auto saved = Read(Access::LayoutPath(app));
        const POINT point{650, 450};
        const auto captured = Access::BeginMenuFileSource(app, source, point);
        Require(captured.entries.size() == 1 && captured.entries.front().sourceIndex == 0 && fs::equivalent(captured.entries.front().filePath, files[0]),
            "S02: initial actual capture must contain only the independently selected A");
        auto preview = Access::ObserveModelPreview(app, point, 0);
        Require(Access::Target(app) == Access::Desktop(app) && Access::Region(app) == HitRegion::Empty,
            "A24 actual host target must be the empty desktop outside the source component");
        const auto frozenCapture = [&] {
            const auto& current = Access::CapturedSources(app);
            if (current.entries.size() != 1 || !fs::equivalent(current.entries.front().filePath, files[0]))
                std::cerr << "A24 captured-count=" << current.entries.size() << " wanted=A\n";
            Require(current.entries.size() == 1 && current.entries.front().sourceIndex == 0 && fs::equivalent(current.entries.front().filePath, files[0]),
                "S02: a later selection must not widen or replace the originally captured A");
        };
        if (sequence == 2)
        {
            Access::ChangeMenuFileSelection(app, keepOriginal);
            Access::RebuildDuringMenu(app); // Actual active-drag rebuild, before entering any menu code.
            frozenCapture();
            Require(Access::MenuFileBindingsCurrent(app, source), "S07: active rebuild must restore the original captured A's current wrappers");
            preview = Access::ObserveModelPreview(app, point, 0);
        }
        int menus = 0;
        DWORD keys = MK_RBUTTON;
        const bool accepted = Access::ChooseDockFileAction(app, point, keys, [&](HMENU menu, POINT, HWND owner) {
            ++menus;
            const bool retained = Access::InternalMenuRetained(app), idle = Access::FileDropIdle(app);
            const bool noOutputs = Outputs(desktop, files).empty(), sameLayout = Read(Access::LayoutPath(app)) == saved;
            if (menus != 1 || owner != Access::completionWindow || !retained || !idle || !noOutputs || !sameLayout)
                std::cerr << "A24 menu-count=" << menus << " owner=" << (owner == Access::completionWindow)
                    << " retained=" << retained << " idle=" << idle << " noOutputs=" << noOutputs << " sameLayout=" << sameLayout << '\n';
            Require(menus == 1 && owner == Access::completionWindow && retained && idle && noOutputs && sameLayout,
                "S02: right menu cannot copy files or save a drop before the decision");
            native_right_drag_cases::Menu(menu, 7);
            if (sequence != 2) Access::ChangeMenuFileSelection(app, keepOriginal);
            if (sequence == 1) Access::RebuildDuringMenu(app);
            return 2u; // Independent user choice: copy the originally captured A.
        });
        Require(accepted && menus == 1, "S02: A still exists under the same source identity, so its captured Copy decision remains executable");
        frozenCapture();
        Require(Access::MenuFileBindingsCurrent(app, source), "S02: the frozen file payload must use live current source wrappers after the menu");
        const auto modifiers = static_cast<int>(keys & (MK_CONTROL | MK_SHIFT | MK_ALT));
        preview = Access::ObserveModelPreview(app, point, modifiers);
        Require(preview.action == DropAction::Copy && preview.fileBacked && preview.landings.size() == 1,
            "S02: only the frozen captured A may enter the actual Copy preview");
        Access::DropDockFiles(app, modifiers);
        Wait([&] { return Access::FileDropIdle(app); }, "A24 actual file copy, completion and placement must finish");
        const auto outputs = Outputs(desktop, files);
        Require(outputs.size() == 1 && Read(outputs.front()) == "captured-A" && !fs::equivalent(outputs.front(), files[0]),
            "S02: Copy must materialize exactly one distinct A output and never copy the uncaptured B");
        const auto check = [&] {
            Require(Read(files[0]) == "captured-A" && Read(files[1]) == "uncaptured-B", "S02: captured and uncaptured original files retain their bytes");
            const auto nowA = native_dock_return_cases::Identity(files[0]), nowB = native_dock_return_cases::Identity(files[1]);
            Require(nowA.dwVolumeSerialNumber == a.dwVolumeSerialNumber && nowA.nFileIndexHigh == a.nFileIndexHigh && nowA.nFileIndexLow == a.nFileIndexLow &&
                nowB.dwVolumeSerialNumber == b.dwVolumeSerialNumber && nowB.nFileIndexHigh == b.nFileIndexHigh && nowB.nFileIndexLow == b.nFileIndexLow &&
                CompareFileTime(&nowA.ftLastWriteTime, &a.ftLastWriteTime) == 0 && CompareFileTime(&nowB.ftLastWriteTime, &b.ftLastWriteTime) == 0,
                "S02: changing selection and copying cannot replace or modify either original file");
            const auto currentOutputs = Outputs(desktop, files);
            Require(currentOutputs.size() == 1 && fs::equivalent(currentOutputs.front(), outputs.front()), "S02: refresh cannot repeat or broaden the captured copy");
            const auto& output = Find(app, ToUpperInvariant(outputs.front().wstring()));
            Require(native_dock_return_cases::SameCell(output.gridCell, preview.landings.front().cell) &&
                Access::HasVisibleIcon(app, output) && Access::HasMatchingRuntimeSlot(app, output),
                "S02: the single copied A must fulfill its captured placement and actual runtime slot");
            Require(Access::Widgets(app).size() == originalWidgets.size(), "S02: menu capture cannot add or remove source components");
            for (const auto& original : originalWidgets)
            {
                const auto& current = native_widget_pair_cases::Widget(app, original.id);
                Require(native_widget_pair_cases::Configuration(current, true, false) == native_widget_pair_cases::Configuration(original, true, false) &&
                    native_widget_pair_cases::OrderedKeys(current) == native_widget_pair_cases::OrderedKeys(original),
                    "S02: source component identity, settings, membership and order must survive the copy and reload");
            }
            Require(Access::NoPresentationWindows(app) && Access::NoIngressPopup(app), "A24 native menu-capture tests cannot create a desktop presentation window");
        };
        check(); Require(Access::Save(app), "save actual frozen-capture Copy result");
        native_drag_async_reload_cases::Refresh(app); check();
        native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app);
        for (const auto& output : outputs) native_dock_return_cases::RemoveOwnedEntry(desktop, output);
        if (source != 3) for (const auto& file : files) native_dock_return_cases::RemoveOwnedEntry(desktop, file);
    }
    std::cout << scenes << " actual native frozen-capture right-menu, selection changes, Copy and storage scenes passed\n";
}
}
