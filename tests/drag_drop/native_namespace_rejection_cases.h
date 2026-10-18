#pragma once

namespace native_namespace_rejection_cases
{
using namespace native_ole_file_cases;

inline void RunMixed(native_drag_commit_cases::Profile& profile)
{
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const bool grouped : {false, true})
    for (const int modifiers : {0, MK_CONTROL, MK_SHIFT, MK_ALT})
    {
        native_drag_commit_cases::context = "D15 mixed grouped=" + std::to_string(grouped) +
            " mods=" + std::to_string(modifiers);
        const auto root = profile.root / (L"namespace-mixed-" + std::to_wstring(scenes++));
        const auto mapped = root / L"mapped", ordinary = desktop / L"Ordinary.txt", member = mapped / L"Existing.txt";
        fs::create_directories(mapped);
        { std::ofstream stream(ordinary, std::ios::binary); stream << "eligible-file-bytes"; }
        { std::ofstream stream(member, std::ios::binary); stream << "unchanged-mapped-member"; }
        auto& app = profile.Model(false, root / L"layout");
        Access::SeedNamespaceMapping(app, ordinary, member, kDesktopIconClsidRecycleBin, grouped);
        Require(Access::Save(app), "save the original mixed namespace fixture");
        Access::PrepareReload(app); Access::BindAsync(app);
        Access::EnableNamespaceRead(app, kDesktopIconClsidRecycleBin);
        native_drag_async_reload_cases::Refresh(app, true);
        const auto systemKey = ToUpperInvariant(kDesktopIconClsidRecycleBin);
        const auto originalCell = Find(app, systemKey).gridCell;
        Access::BeginNamespaceMixed(app, kDesktopIconClsidRecycleBin);
        const auto captured = Access::CapturedSources(app);
        Require(captured.entries.size() == 2 && std::count_if(captured.entries.begin(), captured.entries.end(),
            [](const auto& entry) { return entry.protectedDesktopIcon; }) == 1,
            "D15 mixed source must contain the complete actual selection before production filtering");
        const auto eligible = std::find_if(captured.entries.begin(), captured.entries.end(),
            [](const auto& entry) { return !entry.protectedDesktopIcon; });
        const auto point = Access::NamespaceMappingPoint(app, grouped);
        const auto preview = Access::ObserveModelPreview(app, point, modifiers);
        Require(Access::Region(app) != HitRegion::Blocked && preview.landings.size() == 1 &&
            preview.landings.front().sourceIndex == eligible->sourceIndex,
            "D15 mixed feedback accepts only the ordinary file while retaining its original capture identity");
        Access::SubmitNamespaceMapping(app, modifiers);
        Wait([&] { return Access::FolderDropReadIdle(app) && Outputs(mapped, {member}).size() == 1; },
            "D15 actual partial-eligibility file transaction and target refresh must finish");
        const auto outputs = Outputs(mapped, {member});
        const bool link = (modifiers & MK_ALT) != 0, copy = (modifiers & MK_CONTROL) != 0 || link;
        const auto check = [&] {
            Require(outputs.size() == 1 && (link ? fs::equivalent(LinkTarget(outputs.front()), ordinary) : Read(outputs.front()) == "eligible-file-bytes"),
                "D15 mixed selection performs the promised file action once without namespace outputs");
            Require(fs::exists(ordinary) == copy && Read(member) == "unchanged-mapped-member",
                "D15 skipped objects cannot change the accepted file action or unrelated mapped members");
            const auto& special = Find(app, systemKey);
            Require(special.desktopIconClsid == kDesktopIconClsidRecycleBin &&
                native_dock_return_cases::SameCell(special.gridCell, originalCell) && Access::HasVisibleIcon(app, special),
                "D15 the skipped system object retains its visible desktop identity and location");
            const auto& target = native_ole_widget_cases::Widget(app, grouped ? L"ingress-active" : L"ingress-root");
            Require(target.folderEntries.size() == 2 && std::count_if(target.folderEntries.begin(), target.folderEntries.end(),
                [&](const auto& entry) { return fs::equivalent(entry.fullPath, outputs.front()); }) == 1,
                "D15 only the eligible output joins the physical mapping, including a grouped target");
        };
        check(); Require(Access::Save(app), "persist the accepted subset and untouched namespace");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Require(Access::NoIngressPopup(app), "D15 mixed file handling never opens presentation windows");
        Access::StopModel(app);
        if (fs::exists(ordinary)) native_dock_return_cases::RemoveOwnedEntry(desktop, ordinary);
    }
    std::cout << scenes << " native mixed namespace/file partial-eligibility and reload scenes passed\n";
}

inline void Run()
{
    native_drag_commit_cases::Profile profile;
    std::size_t scenes = 0;
    for (const auto* clsid : {kDesktopIconClsidRecycleBin, kDesktopIconClsidThisPC})
    for (const bool grouped : {false, true})
    for (const int modifiers : {0, MK_CONTROL, MK_SHIFT, MK_ALT})
    {
        native_drag_commit_cases::context = "D15 grouped=" + std::to_string(grouped) +
            " mods=" + std::to_string(modifiers) + " source=" + std::to_string(scenes);
        const auto root = profile.root / (L"namespace-reject-" + std::to_wstring(scenes++));
        const auto mapped = root / L"mapped", ordinary = root / L"Ordinary.txt", member = mapped / L"Existing.txt";
        fs::create_directories(mapped);
        { std::ofstream stream(ordinary, std::ios::binary); stream << "ordinary-source"; }
        { std::ofstream stream(member, std::ios::binary); stream << "unchanged-mapped-member"; }
        auto& app = profile.Model(false, root / L"layout");
        Access::SeedNamespaceMapping(app, ordinary, member, clsid, grouped);
        Require(Access::Save(app), "persist original namespace and mapped destination");
        const auto saved = Read(Access::LayoutPath(app));
        Access::BeginModel(app, 0, 1);
        const auto captured = Access::CapturedSources(app);
        Require(captured.entries.size() == 1 && captured.entries.front().protectedDesktopIcon &&
            captured.entries.front().desktopKey == ToUpperInvariant(clsid),
            "D15 captures the actual system DesktopIcon wrapper with its protected identity");
        const auto point = Access::NamespaceMappingPoint(app, grouped);
        const auto preview = Access::ObserveModelPreview(app, point, modifiers);
        Require(Access::Region(app) == HitRegion::Blocked && preview.Empty(),
            "D15: an unsupported system object over a physical mapping must reject during hover, before any submission");
        Require(Access::CurrentNativeDragHint(app, modifiers).empty(),
            "D15: rejected namespace drop cannot advertise move, copy, insertion or shortcut acceptance");
        int menus = 0; DWORD keys = MK_RBUTTON;
        const bool chosen = Access::ChooseDockFileAction(app, point, keys,
            [&](HMENU, POINT, HWND) { ++menus; return 1u; });
        Require(!chosen && menus == 0,
            "D15: an unsupported namespace mapping cannot open an actionable right-drag menu");
        Access::SubmitNamespaceMapping(app, modifiers);
        Require(Access::NoPendingFileDrop(app) && Read(Access::LayoutPath(app)) == saved &&
            Outputs(mapped, {member}).empty() && Read(member) == "unchanged-mapped-member" && Read(ordinary) == "ordinary-source",
            "D15: even a release on the rejected target queues no file operation and leaves sources, destination and saved layout unchanged");
        Require(Access::NoIngressPopup(app), "D15 never opens presentation windows for rejected content");
        Access::StopModel(app);
    }
    std::cout << scenes << " native namespace/mapped-folder hover, menu and submission rejection scenes passed\n";
    RunMixed(profile);
}
}
