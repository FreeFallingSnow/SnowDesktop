#pragma once
namespace native_folder_delivery_cases
{
using namespace native_ole_file_cases;
inline const DesktopWidget& Data(const DesktopApp& app)
{
    const auto& widgets = Access::Widgets(app);
    Require(widgets.size() == 1 && widgets.front().id == L"ingress-root" &&
        widgets.front().type == DesktopWidgetType::FolderMapping,
        "deferred folder delivery must retain the original mapped container identity");
    return widgets.front();
}
inline void Run()
{
    native_drag_commit_cases::context = "native folder delivery after drag, without another filesystem event";
    native_drag_commit_cases::Profile profile;
    const auto folder = profile.root / L"mapped";
    fs::create_directory(folder);
    const auto original = folder / L"Existing.txt", added = folder / L"Added.txt";
    { std::ofstream stream(original); stream << "original-bytes"; }
    auto& app = profile.Model(false, profile.root / L"layout");
    Access::SeedExternalStorage(app, DesktopWidgetType::FolderMapping, DesktopWidgetType::FolderMapping, false, original);
    Access::PrepareReload(app); Access::BindAsync(app);
    Require(Access::Save(app), "save actual mapped model before deferred directory read");
    Access::BeginExternal(app, 1, {350, 250});
    { std::ofstream stream(added); stream << "added-bytes"; }
    const auto messages = Access::asyncReadMessages;
    Access::RequestMappedRead(app, folder.wstring());
    Wait([&] { return Access::FolderReaderIdle(app) && Access::asyncReadMessages > messages; },
        "actual folder worker and background message must finish while the drag retains its model");
    Require(Data(app).folderEntries.size() == 1 && fs::equivalent(Data(app).folderEntries.front().fullPath, original),
        "completed directory reads cannot replace a model still held by a drag");
    Access::EndExternalModel(app);
    Require(!Access::ShellReloadPending(app) && Access::FolderReaderIdle(app),
        "controlled delivery release must not request another desktop or directory read");
    Wait([&] { return Data(app).folderEntries.size() == 2; },
        "completed directory snapshot must publish after drag release without another event");
    for (const auto& file : {original, added})
        Require(std::count_if(Data(app).folderEntries.begin(), Data(app).folderEntries.end(),
            [&](const auto& entry) { return fs::equivalent(entry.fullPath, file); }) == 1,
            "deferred folder delivery must publish each actual file identity once");
    Require(Read(original) == "original-bytes" && Read(added) == "added-bytes" && Access::NoIngressPopup(app),
        "deferred directory publication must preserve file bytes and avoid desktop presentation windows");
    Access::StopModel(app);
    std::cout << "1 actual native deferred-folder delivery scene passed\n";
}
}
