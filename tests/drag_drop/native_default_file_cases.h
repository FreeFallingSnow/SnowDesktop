#pragma once

namespace native_default_file_cases
{
using namespace native_ole_file_cases;

inline std::wstring Volume(const fs::path& path)
{
    // Independent environment qualification, not the product's decision.
    std::array<wchar_t, 32768> mount{}, name{};
    Require(GetVolumePathNameW(path.c_str(), mount.data(), static_cast<DWORD>(mount.size())) &&
        GetVolumeNameForVolumeMountPointW(mount.data(), name.data(), static_cast<DWORD>(name.size())),
        "A20: qualify the real fixture volume before running its declared relation");
    return name.data();
}
class OtherVolume
{
    fs::path parent_;
public:
    fs::path root;
    explicit OtherVolume(const fs::path& desktop)
    {
        for (const auto& candidate : {fs::current_path(), fs::temp_directory_path()})
        {
            if (Volume(candidate) == Volume(desktop)) continue;
            GUID guid{}; wchar_t text[40]{};
            Require(SUCCEEDED(CoCreateGuid(&guid)), "A20: allocate isolated cross-volume identity");
            StringFromGUID2(guid, text, 40);
            std::error_code error;
            const auto trial = candidate / (std::wstring(L"SnowDesktopNative-Volume-") + text);
            if (!fs::create_directory(trial, error)) continue;
            parent_ = fs::weakly_canonical(candidate); root = trial; break;
        }
    }
    ~OtherVolume()
    {
        std::error_code error;
        const auto actual = fs::weakly_canonical(root, error);
        if (!error && !root.empty() && actual.parent_path() == parent_ &&
            actual.filename().wstring().starts_with(L"SnowDesktopNative-Volume-")) fs::remove_all(actual, error);
    }
};
inline int Run(std::string_view targetName, std::string_view relation)
{
    const bool desktopTarget = targetName == "desktop";
    const auto found = std::find_if(std::begin(native_ole_widget_cases::targets), std::end(native_ole_widget_cases::targets),
        [&](const auto& value) { return value.name == targetName; });
    Require(desktopTarget || found != std::end(native_ole_widget_cases::targets), "A20: declared native destination required");
    Require(relation == "same" || relation == "cross" || relation == "mixed", "A20: declared physical volume relation required");
    const bool move = relation == "same";
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    OtherVolume other(desktop);
    if (!move && other.root.empty())
    {
        std::cout << "A20 environment blocked: no writable second real volume in test working directory or system temp; "
            << targetName << ' ' << relation << " not executed\n";
        return 77;
    }
    std::size_t scenes = 0;
    for (const DWORD allowed : {3u, 7u})
    for (const bool icon : {false, true})
    for (const bool asynchronous : {false, true})
    for (const std::size_t count : {1u, 2u})
    {
        if ((desktopTarget && icon) || (relation == "mixed" && count == 1)) continue;
        native_drag_commit_cases::context = "A20 target=" + std::string(targetName) + " relation=" + std::string(relation) +
            " allowed=" + std::to_string(allowed) + " icon=" + std::to_string(icon) +
            " async=" + std::to_string(asynchronous) + " count=" + std::to_string(count);
        const auto directory = profile.root / (L"default-" + std::to_wstring(scenes++));
        const auto cross = other.root / (L"default-" + std::to_wstring(scenes));
        fs::create_directories(directory / L"source"); fs::create_directories(directory / L"mapped");
        if (!move) fs::create_directory(cross);
        const bool mapped = !desktopTarget && found->mapped;
        const auto destination = mapped ? directory / L"mapped" : desktop;
        const auto original = destination / L"Existing-Target.txt";
        { std::ofstream stream(original, std::ios::binary); stream << "existing-target"; }
        const auto firstParent = relation == "cross" ? cross : directory / L"source";
        std::vector<fs::path> files{firstParent / L"Alpha-Payload.txt"};
        { std::ofstream stream(files.front(), std::ios::binary); stream << "ingress-file"; }
        if (count == 2)
        {
            files.push_back((move ? directory / L"source" : cross) / L"Bundle"); fs::create_directory(files.back());
            std::ofstream stream(files.back() / L"nested.txt", std::ios::binary); stream << "ingress-directory";
        }
        for (std::size_t i = 0; i < files.size(); ++i)
            Require((Volume(files[i]) == Volume(destination)) == (move || (relation == "mixed" && i == 0)),
                "A20: every real source must satisfy the independent same/cross/mixed-volume input");
        auto& app = profile.Model(false, directory / L"layout");
        if (desktopTarget) Access::PopulateModel(app, {original}, 0);
        else Access::SeedExternalStorage(app, found->root, found->member, icon, original);
        Access::PrepareReload(app); Access::BindAsync(app);
        Require(Access::Save(app), "A20: save actual native baseline");
        const auto saved = Read(Access::LayoutPath(app));
        const auto configuration = desktopTarget ? std::string{} : native_ole_widget_cases::Configuration(app, *found);
        auto source = ole_file_source_fixture::Make(relation == "mixed" ? std::vector<fs::path>{files.front()} : files, asynchronous);
        if (relation == "mixed") source->multiParentPaths = files;
        auto adapter = Access::ExternalAdapter(app);
        const POINT point = desktopTarget ? POINT{150, 150} : icon ? POINT{50, 50} : POINT{350, 250};
        for (const bool entering : {true, false})
        {
            Access::FreshExternalDwell(app); DWORD feedback = allowed;
            const auto result = entering ? adapter->DragEnter(source.Get(), MK_LBUTTON, Access::ExternalPoint(point), &feedback)
                : adapter->DragOver(MK_LBUTTON, Access::ExternalPoint(point), &feedback);
            Require(SUCCEEDED(result) && (feedback == 1 || feedback == 2), "A20: default hover must offer a supported file action");
            Require(source->starts == 0 && source->ends == 0 && (!asynchronous || source->dataReads == 0) &&
                Outputs(destination, {original}).empty() && Read(Access::LayoutPath(app)) == saved,
                "A20: hover cannot render delayed paths, lease, submit files or save layout");
        }
        Access::FreshExternalDwell(app); DWORD returned = allowed;
        Require(SUCCEEDED(adapter->Drop(source.Get(), 0, Access::ExternalPoint(point), &returned)), "A20: real adapter default Drop must reach host");
        if (asynchronous) Require((returned == 1 || returned == 2) && source->starts == 1,
            "A20: delayed default must acknowledge accepted work and own exactly one lease before returning");
        Wait([&] { return (mapped ? Access::FolderDropReadIdle(app) : Access::FileDropIdle(app)) &&
            (!asynchronous || source->ends == 1); }, "A20: actual file/read/model completion must finish");
        const auto outputs = Outputs(destination, {original});
        Require(outputs.size() == files.size(), "A20: default batch must produce exactly one output per selected identity");
        std::vector<fs::path> ordered;
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            Require(fs::exists(files[i]) == !move, "A20: same-volume default must move; cross/mixed defaults must preserve every source");
            const auto output = destination / files[i].filename(); ordered.push_back(output);
            Require(fs::exists(output) && Read(i ? output / L"nested.txt" : output) == (i ? "ingress-directory" : "ingress-file"),
                "A20: real default outputs must preserve file and directory-descendant bytes");
            if (!move) Require(Read(i ? files[i] / L"nested.txt" : files[i]) == (i ? "ingress-directory" : "ingress-file"),
                "A20: copied sources must retain original bytes");
        }
        Require(!source->wrongApartment && !source->fileReadBeforeStart, "A20: real source reads must respect STA and lease ordering");
        if (asynchronous) Require(source->ends == 1 && SUCCEEDED(source->outcome) && source->endEffect == (move ? 0u : 1u),
            "A20: completion must report actual optimized Move or Copy after path-based resolution");
        std::vector<GridCell> cells;
        const auto check = [&] {
            if (!desktopTarget) { native_ole_widget_cases::Check(app, *found, icon, original, ordered, configuration); return; }
            Require(Read(original) == "existing-target", "A20: old desktop bytes must remain unchanged");
            std::unordered_set<std::wstring> occupied;
            for (std::size_t i = 0; i < ordered.size(); ++i)
            {
                const auto& item = Find(app, ToUpperInvariant(ordered[i].wstring()));
                Require(Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item) &&
                    occupied.insert(item.gridCell.pageId + L":" + std::to_wstring(item.gridCell.column) + L"," + std::to_wstring(item.gridCell.row)).second,
                    "A20: desktop outputs must have distinct accessible runtime slots");
                if (cells.size() <= i) cells.push_back(item.gridCell);
                Require(item.gridCell.pageId == cells[i].pageId && item.gridCell.column == cells[i].column && item.gridCell.row == cells[i].row &&
                    (i != 0 || (item.gridCell.pageId == L"__page:1" && item.gridCell.column == 1 && item.gridCell.row == 1)),
                    "A20: requested first landing and accepted batch cells must survive reload");
            }
        };
        check(); Require(Access::Save(app), "A20: save accepted native default result");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        if (mapped) Require(Outputs(desktop, {}).empty(), "A20: mapped default cannot publish onto free desktop");
        Require(Access::NoIngressPopup(app), "A20: no desktop presentation or popup may be created");
        Access::StopModel(app); source.Reset();
        for (const auto& output : outputs)
        {
            Require(fs::weakly_canonical(output.parent_path()) == fs::weakly_canonical(destination), "A20: cleanup must remain inside owned destination");
            fs::remove_all(output);
        }
        fs::remove(original);
    }
    std::cout << scenes << " actual native default-volume file/model/storage scenes passed: " << targetName << ' ' << relation << '\n';
    return 0;
}
}
