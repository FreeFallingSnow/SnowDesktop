#pragma once

namespace native_owned_content_rejection_cases
{
using namespace native_owned_content_cases;
// Copy permission is an independent protocol factor. These four scenes per
// target extend the existing target entry; they do not multiply every format
// by an equivalent unavailable-Copy set or add another CTest process.
inline void Run(std::string_view targetName)
{
    const bool keyed = targetName == "collection", mapped = targetName == "mapping", dock = targetName == "dock";
    Require(targetName == "desktop" || keyed || mapped || dock, "declared content rejection destination required");
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop; fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const bool virtualFiles : {false, true})
    for (const bool asynchronous : {false, true})
    {
        native_drag_commit_cases::context = "A26 no-Copy content target=" + std::string(targetName) +
            " virtual=" + std::to_string(virtualFiles) + " async=" + std::to_string(asynchronous);
        const auto directory = profile.root / (L"rejected-content-" + std::to_wstring(scenes++)); fs::create_directories(directory / L"mapped");
        const auto original = (mapped ? directory / L"mapped" : desktop) / L"Existing-Target.txt";
        { std::ofstream stream(original, std::ios::binary); stream << "existing-target"; }
        auto& app = profile.Model(false, directory / L"layout");
        if (keyed || mapped) Access::SeedExternalStorage(app, mapped ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection,
            mapped ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection, false, original);
        else { Access::PopulateModel(app, {original}, 0); if (dock) Access::SeedDock(app); }
        Access::PrepareReload(app); Access::BindAsync(app); Require(Access::Save(app), "save native content rejection baseline");
        const auto saved = Read(Access::LayoutPath(app)); const auto staged = Staged();
        auto source = Microsoft::WRL::Make<Source>(); source->asynchronous = asynchronous;
        if (virtualFiles) source->Virtual(2, false); else source->Image();
        auto adapter = Access::ExternalAdapter(app);
        const POINT point = dock ? Access::DockPoint(app) : keyed || mapped ? POINT{350, 250} : POINT{150, 150};
        constexpr DWORD allowed = DROPEFFECT_MOVE | DROPEFFECT_LINK;
        DWORD effect = allowed; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), MK_LBUTTON | MK_CONTROL, Access::ExternalPoint(point), &effect)), "actual native no-Copy Enter");
        effect = allowed; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragOver(MK_LBUTTON | MK_CONTROL, Access::ExternalPoint(point), &effect)) && effect == 0,
            "D08: explicit content Copy cannot advertise an effect that the producer did not offer");
        effect = allowed; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->Drop(source.Get(), MK_CONTROL, Access::ExternalPoint(point), &effect)) && effect == 0,
            "D08: unavailable content Copy must reject rather than substitute Move or Link");
        Wait([&] { return mapped ? Access::FolderDropReadIdle(app) : Access::FileDropIdle(app); }, "rejected content native model must remain idle");
        const auto destination = mapped ? original.parent_path() : desktop;
        Require(Outputs(destination, {original}).empty() && Staged() == staged && source->starts == 0 && source->ends == 0 &&
            Read(original) == "existing-target" && Read(Access::LayoutPath(app)) == saved && !source->wrongApartment,
            "B17: unavailable Copy cannot lease, stage, create files, mutate owners or save a drop");
        if (dock) Require(Access::DockEntries(app).empty(), "B17: unavailable Copy cannot create a Dock reference");
        Access::StopModel(app); source.Reset();
        if (!mapped) native_dock_return_cases::RemoveOwnedEntry(desktop, original);
    }
    std::cout << scenes << " unavailable-Copy native image/virtual source rejection scenes passed\n";
}
}
