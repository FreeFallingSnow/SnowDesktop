#pragma once
namespace native_ole_dock_cases
{
using namespace native_ole_file_cases;
inline void Check(DesktopApp& app, const std::vector<fs::path>& outputs)
{
    Require(Access::NoDockWindows(app), "A19 Dock cannot allocate any desktop or persistent Dock presentation window");
    const auto& entries = Access::DockEntries(app);
    Require(entries.size() == outputs.size(), "external folders must create exactly the corresponding Dock references");
    for (const auto& output : outputs)
    {
        Require(std::count_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.type == DockEntryType::DesktopItem && fs::equivalent(fs::path(entry.reference), output); }) == 1,
            "every actual shortcut must be referenced exactly once by the actual Dock model");
        const auto& item = Find(app, ToUpperInvariant(output.wstring()));
        Require(fs::equivalent(fs::path(item.parsingName), output), "A19: Dock reference must resolve through actual host desktop enumeration");
    }
    std::vector<fs::path> runtime;
    for (const auto& slot : Access::Dock(app)->GetSlots())
        if (const auto* item = dynamic_cast<const DockEntryItem*>(slot->GetItem())) runtime.emplace_back(item->GetReference());
    Require(runtime.size() == outputs.size(), "actual Dock runtime slots must expose every accepted reference");
    for (const auto& output : outputs)
        Require(std::count_if(runtime.begin(), runtime.end(), [&](const auto& path) { return fs::equivalent(path, output); }) == 1,
            "each accepted shortcut must be accessible once in actual Dock slots");
}
inline void Run(std::string_view mode)
{
    const bool right = mode != "alt_link";
    const bool accept = mode == "alt_link" || mode == "right_link";
    Require(accept || mode == "right_cancel" || mode == "right_dismiss", "declared independent Dock gesture required");
    const UINT command = mode == "right_link" ? 3u : mode == "right_cancel" ? 4u : 0u;
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    std::size_t scenes = 0;
    for (const bool asynchronous : {false, true})
    for (const std::size_t count : {1u, 2u})
    for (const bool rebuild : {false, true})
    for (const DWORD allowed : {7u, 4u, 0u})
    {
        if (!right && rebuild) continue;
        native_drag_commit_cases::context = "A19 Dock mode=" + std::string(mode) + " async=" + std::to_string(asynchronous) +
            " count=" + std::to_string(count) + " rebuild=" + std::to_string(rebuild) + " allowed=" + std::to_string(allowed);
        const auto directory = profile.root / (L"dock-" + std::to_wstring(scenes++));
        std::vector<fs::path> files;
        for (std::size_t i = 0; i < count; ++i)
        {
            files.push_back(directory / (L"source/Folder-" + std::to_wstring(i)));
            fs::create_directories(files.back());
            std::ofstream stream(files.back() / L"nested.txt", std::ios::binary); stream << "dock-source-" << i;
        }
        auto& app = profile.Model(false, directory / L"layout");
        Access::SeedDock(app); Access::PrepareReload(app); Access::BindAsync(app);
        Require(Access::Save(app), "save actual empty Dock baseline");
        const auto saved = Read(Access::LayoutPath(app));
        Check(app, {});
        auto source = ole_file_source_fixture::Make(files, asynchronous);
        int menus = 0;
        Access::RightMenuBridge bridge(app, [&](HMENU menu, POINT, HWND owner) {
            ++menus;
            Require(right && owner == Access::completionWindow && Access::RightMenuRetained(app) &&
                source->starts == 0 && source->ends == 0 && (!asynchronous || source->dataReads == 0) &&
                Outputs(desktop, {}).empty() && Read(Access::LayoutPath(app)) == saved,
                "A19: Dock menu must retain the destination before any content read, lease or submission");
            native_right_drag_cases::Menu(menu, 4);
            if (rebuild) Access::RebuildDuringMenu(app);
            Require(Access::RightMenuRetained(app) && Access::NoDockWindows(app), "A19: Dock model rebuild cannot lose pending menu context or create a host window");
            return command;
        });
        auto adapter = right ? ComPtr<OleDragDropAdapter>{} : Access::ExternalAdapter(app);
        if (right) adapter.Attach(new OleDragDropAdapter(&bridge));
        const POINT point = Access::DockPoint(app);
        const DWORD keys = right ? MK_RBUTTON : MK_LBUTTON | MK_ALT;
        DWORD feedback = allowed;
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), keys, Access::ExternalPoint(point), &feedback)), "actual OLE Dock Enter must succeed");
        feedback = allowed;
        Require(SUCCEEDED(adapter->DragOver(keys, Access::ExternalPoint(point), &feedback)), "actual OLE Dock Over must succeed");
        Require(feedback == (allowed ? 4u : 0u) && (!allowed || Access::Target(app) == Access::Dock(app)),
            "explicit external-folder Link must target the actual Dock and acknowledge source-offered Link");
        Require(source->starts == 0 && source->ends == 0 && (!asynchronous || source->dataReads == 0), "A19: Dock hover cannot acquire or render an async source");
        DWORD effect = allowed;
        Require(SUCCEEDED(adapter->Drop(source.Get(), right ? MK_CONTROL : MK_ALT, Access::ExternalPoint(point), &effect)), "actual OLE Dock Drop must execute through the host");
        const bool accepted = accept && allowed;
        Require(effect == (accepted ? 4u : 0u) && menus == (right && allowed ? 1 : 0), "A19: Dock chosen Link or cancellation must determine the immediate acknowledged effect");
        if (accepted && asynchronous) Require(source->starts == 1, "accepted async Dock Link must acquire one lease before returning");
        Wait([&] { return Access::FileDropIdle(app) && (!accepted || !asynchronous || source->ends == 1); }, "actual Dock file/read/completion and reload must finish");
        const auto outputs = Outputs(desktop, {});
        Require(outputs.size() == (accepted ? count : 0), "A19: Dock Link must materialize exactly one actual shortcut per accepted folder");
        for (std::size_t i = 0; i < count; ++i)
        {
            Require(Read(files[i] / L"nested.txt") == "dock-source-" + std::to_string(i), "U08: original external folders and nested bytes must stay at their source");
            if (accepted) Require(std::count_if(outputs.begin(), outputs.end(), [&](const auto& output) {
                Require(_wcsicmp(output.extension().c_str(), L".lnk") == 0, "A19: Dock Link outputs must be actual persisted shortcuts");
                return fs::equivalent(LinkTarget(output), files[i]); }) == 1, "each Dock shortcut must resolve exactly once to the original external folder");
        }
        Require(!source->wrongApartment && !source->fileReadBeforeStart, "A19: Dock source COM reads must retain the STA and lease ordering");
        if (accepted && asynchronous) Require(source->ends == 1 && SUCCEEDED(source->outcome) && source->endEffect == 4, "A19: Dock async Link must finish exactly one successful Link lease");
        if (!accepted) Require(source->starts == 0 && source->ends == 0 && Read(Access::LayoutPath(app)) == saved, "rejected or cancelled Dock input cannot lease or change saved layout");
        Check(app, outputs);
        if (accepted)
        {
            Require(Access::Save(app), "save actual accepted Dock references");
            Access::Load(app); native_drag_async_reload_cases::Refresh(app, true);
            Check(app, outputs);
        }
        Access::StopModel(app); source.Reset();
        for (const auto& output : outputs)
        {
            Require(fs::weakly_canonical(output.parent_path()) == fs::weakly_canonical(desktop), "remove only this isolated desktop shortcut output");
            // Background Shell providers intentionally outlive Stop(). Wait
            // only for this owned temporary link's delete-sharing fence;
            // all product assertions above have already completed.
            Wait([&] {
                std::error_code error;
                fs::remove(output, error);
                Require(!error || error.value() == ERROR_SHARING_VIOLATION ||
                    error.value() == ERROR_LOCK_VIOLATION || error == std::errc::permission_denied,
                    "temporary Dock shortcut cleanup failed outside the provider sharing fence");
                return !error;
            }, "stopped Shell providers must release the owned temporary Dock shortcut for cleanup");
        }
    }
    std::cout << scenes << " actual native external-folder Dock scenes passed: " << mode << '\n';
}
}
