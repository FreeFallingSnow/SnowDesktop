#pragma once
#include "data/layout_storage.h"
#include "navigation/search_match.h"
#include "navigation/quick_navigation_search_async.h"
#include "widget/permissions/widget_permission_broker.h"
#include "widget/tasks/widget_task_broker.h"
#include "widget/api/widget_api_registry.h"
#include "dock/dock_window_pin.h"
#include "common/json_value.h"
#include "widget/packages/widget_package.h"
#include <set>
#include <utility>
#include "diagnostics/diagnostic_log_modules.h"
#include <condition_variable>
#include <mutex>

namespace behavior
{
// Real native JSON persistence; only host notifications are observed here.
// This protects controller draft/commit semantics, not desktop side effects.
struct SettingsObserver final : snowdesktop::SettingsHostActions
{
    int previews = 0, commits = 0;
    bool rejectNextCommit = false;
    snowdesktop::SettingsActionResult OnSettingsPreview(const snowdesktop::SettingsSnapshot&, snowdesktop::SettingsDomain) override
    { ++previews; return snowdesktop::SettingsActionResult::Success(); }
    snowdesktop::SettingsActionResult OnSettingsCommitted(const snowdesktop::SettingsSnapshot&, snowdesktop::SettingsDomain domains) override
    {
        ++commits;
        if (std::exchange(rejectNextCommit, false))
            return snowdesktop::SettingsActionResult::Failure(L"controlled host refusal", domains);
        return snowdesktop::SettingsActionResult::Success(domains);
    }
    snowdesktop::SettingsActionResult OnSettingsRouteChanged(const snowdesktop::SettingsRoute&) override
    { return snowdesktop::SettingsActionResult::Success(); }
    snowdesktop::SettingsActionResult Invoke(const Request&) override
    { return snowdesktop::SettingsActionResult::Success(); }
};
void Settings()
{
    Profile profile;
    using namespace snowdesktop;
    auto store = CreateNativeSettingsStore();
    SettingsObserver observer;
    SettingsController controller(store, &observer);
    Require(controller.Initialize().Succeeded(), "load real settings store");
    SettingsValues before; Require(store->Load(before).Succeeded(), "read baseline settings");
    auto draft = before.general; draft.doubleClickHideDesktop = !before.general.doubleClickHideDesktop;
    controller.UpdateGeneral(draft, SettingsUpdateMode::Draft);
    Require(controller.FlushPending().Succeeded(), "flush draft queue");
    SettingsValues disk; Require(store->Load(disk).Succeeded(), "read settings after draft");
    Require(disk.general.doubleClickHideDesktop == before.general.doubleClickHideDesktop && observer.commits == 0 && observer.previews == 0,
        "draft cannot persist or emit host actions");
    Require(controller.Reload().status == SettingsActionStatus::Busy && controller.Snapshot()->values.general.doubleClickHideDesktop == draft.doubleClickHideDesktop,
        "ordinary reload preserves pending draft");
    // A failed host application must not make an unaccepted setting durable.
    observer.rejectNextCommit = true;
    controller.RequestCommit(SettingsDomain::General);
    Require(!controller.FlushPending().Succeeded() && observer.commits == 1,
        "host refusal reports a failed commit");
    Require(store->Load(disk).Succeeded() && disk.general.doubleClickHideDesktop == before.general.doubleClickHideDesktop,
        "host refusal preserves the persisted setting");
    Require(controller.RetryPending() && controller.FlushPending().Succeeded() && observer.commits == 2,
        "explicit retry applies and persists the pending value once");
    Require(store->Load(disk).Succeeded() && disk.general.doubleClickHideDesktop == draft.doubleClickHideDesktop, "commit reaches real JSON store");
    controller.UpdateGeneral(before.general, SettingsUpdateMode::Preview);
    Require(controller.FlushPending().Succeeded() && observer.previews == 1 && observer.commits == 2,
        "preview notifies once without a new commit");
    auto external = before.general;
    external.contextMenuHidePageManagement = !before.general.contextMenuHidePageManagement;
    Require(store->SaveGeneral(external).Succeeded(), "install independent external replacement");
    controller.PrepareForExternalDataReplacement();
    Require(controller.CloseSession().Succeeded() && store->Load(disk).Succeeded() &&
        disk.general.doubleClickHideDesktop == external.doubleClickHideDesktop &&
        disk.general.contextMenuHidePageManagement == external.contextMenuHidePageManagement,
        "closing after external replacement cannot flush stale draft over replacement data");
}
void Recovery()
{
    Profile p;
    namespace storage = snowdesktop::layout_storage;
    const auto path = p.root / L"recovery/layout.json";
    fs::create_directories(path.parent_path());
    const std::string first = R"({"schemaVersion":1,"items":[{"key":"owned-A","page":"page","x":1,"y":2}],"widgets":[{"id":"collection","type":"collection","items":["owned-A"]}]})";
    const std::string second = R"({"schemaVersion":1,"items":[{"key":"owned-B","page":"page","x":3,"y":0}]})";
    std::string error;
    const bool firstSaved = storage::SaveDocument(path, first, &error);
    Require(firstSaved, "save first generation: " + error);
    const bool secondSaved = storage::SaveDocument(path, second, &error);
    Require(secondSaved, "save second generation: " + error);
    const auto backup = Read(storage::BackupPath(path));
    Require(!storage::SaveDocument(path, "{broken", &error) && Read(path) == second && Read(storage::BackupPath(path)) == backup,
        "invalid replacement cannot damage primary or recovery data");
    Write(path, "{truncated");
    storage::Document restored;
    const auto result = storage::LoadDocument(path, restored);
    Require(result.status == storage::LoadStatus::RecoveredBackup && restored.items.size() == 1 && restored.items[0].key == "owned-A" &&
        restored.widgets.size() == 1 && restored.widgets[0].items == std::vector<std::string>{"owned-A"},
        "corrupt primary recovers the preceding valid generation with ownership intact");
    std::string cleared;
    Require(storage::BuildClearedDocument(first, cleared, &error) && storage::SaveClearedDocument(path, cleared, &error), "clear both stored generations");
    Write(path, "{truncated");
    Require(storage::LoadDocument(path, restored).status == storage::LoadStatus::RecoveredBackup && restored.items.empty() && restored.widgets.empty(),
        "recovery after clear cannot resurrect old placements");
    Write(storage::BackupPath(path),"{damaged-backup");
    const auto corruptPrimary = Read(path), corruptBackup = Read(storage::BackupPath(path));
    const auto invalid = storage::LoadDocument(path,restored);
    Require(invalid.status == storage::LoadStatus::Invalid && restored.items.empty() && restored.widgets.empty() &&
        Read(path) == corruptPrimary && Read(storage::BackupPath(path)) == corruptBackup,
        "two corrupt generations report invalid without overwriting either piece of recovery evidence");
}
void Search()
{
    Profile profile;
    std::vector<fs::path> files;
    for (const auto* name : {L"z-editor", L"editor notes", L"editor", L"unrelated"})
    {
        files.push_back(profile.desktop / name);
        Write(files.back(), "search-entry");
    }
    Require(Access::QuickNavigationNames(*profile.app, files, L"editor") ==
        std::vector<std::wstring>{L"editor", L"editor notes", L"z-editor"},
        "production search model filters misses and orders exact, prefix and interior matches");
    Require(NameMatchesQuery(L"文件管理器", L"wjglq") && !NameMatchesQuery(L"editor", L"unrelated"),
        "name-matching contract accepts pinyin initials and rejects an unrelated query");
    struct Gate { std::mutex mutex; std::condition_variable ready; bool entered = false, release = false; };
    auto gate = std::make_shared<Gate>();
    snowdesktop::QuickNavigationEverythingSearchAsync search([gate](const std::wstring& query, DWORD) {
        if (query == L"old")
        {
            std::unique_lock lock(gate->mutex); gate->entered = true; gate->ready.notify_all();
            if (!gate->ready.wait_for(lock, std::chrono::seconds(5), [&] { return gate->release; }))
                return snowdesktop::QuickNavigationEverythingSearchResponse{{},ERROR_TIMEOUT};
        }
        EverythingSearchResult entry; entry.name = query; entry.path = L"C:\\isolated\\" + query;
        return snowdesktop::QuickNavigationEverythingSearchResponse{{entry},ERROR_SUCCESS};
    });
    Require(search.Submit(nullptr, 0, {1,L"old",20}), "submit first query");
    { std::unique_lock lock(gate->mutex); Require(gate->ready.wait_for(lock,std::chrono::seconds(3),[&] { return gate->entered; }), "provider entered"); }
    Require(search.Submit(nullptr,0,{2,L"discarded",20}) && search.Submit(nullptr,0,{3,L"latest",20}), "typing remains responsive during blocked query");
    { std::lock_guard lock(gate->mutex); gate->release = true; gate->ready.notify_all(); }
    std::optional<snowdesktop::QuickNavigationEverythingSearchResult> result;
    Wait([&] { auto next=search.TakeCompleted(); if(next) result=std::move(next); return result && result->generation == 3; });
    Require(result->query == L"latest" && result->results.size() == 1 && result->results[0].name == L"latest", "only newest pending query reaches published results");
    search.Stop(); Require(!search.Submit(nullptr,0,{4,L"after-stop",20}), "disposed search rejects further work");
}
void WidgetContracts()
{
    using namespace snowdesktop::widget;
    const std::vector<std::string> required{"network.http"}, optional{"system.read"}, domains{"example.com"}, empty;
    const auto grant = WidgetPermissionBroker::Evaluate(PermissionDecisionState::Granted, required, optional, domains, empty, empty);
    Require(grant.runtimeBlock == PermissionRuntimeBlock::MissingRequired && grant.permissions.empty(), "explicit empty grant cannot inherit declared rights");
    const auto pending = WidgetPermissionBroker::Evaluate(PermissionDecisionState::Pending, required, domains, required, domains);
    Require(pending.runtimeBlock == PermissionRuntimeBlock::PendingConsent && pending.permissions.empty(), "pending consent cannot activate old stored grants");
    Require(WidgetPermissionBroker::AllowsNetworkDomain(domains,"example.com") && !WidgetPermissionBroker::AllowsNetworkDomain(domains,"example.com.evil"), "domain grant rejects suffix impersonation");
    using namespace snowdesktop::widget_runtime;
    WidgetTaskBroker broker; std::string error;
    Require(broker.RegisterTask({"test.write","filesystem.write",true,1},error), "register task policy");
    TaskStartOptions options; options.ownerToken=42; options.permissionGranted=true;
    Require(!broker.Start("widget","test.write",options), "privileged task requires actual trusted gesture");
    options.trustedGesture=true;
    const auto task=broker.Start("widget","test.write",options);
    Require(static_cast<bool>(task) && !broker.Start("widget","test.write",options), "per-instance task limit");
    Require(broker.SetPermission("widget","filesystem.write",false)==1, "revocation cancels live task");
    auto actions=broker.DrainActions();
    Require(actions.size()==2 && actions.back().type==TaskBrokerActionType::Cancel && actions.back().cancelReason==TaskBrokerCancelReason::PermissionRevoked,
        "executor receives revocation cancellation for the actual task");
    Require(broker.Complete(task.id,true), "retire cancelled task once");
    const auto completions=broker.DrainCompletions();
    Require(completions.size()==1 && !completions[0].ok && completions[0].ownerToken==42 && !broker.Complete(task.id,true),
        "late successful completion cannot become authorized success or deliver twice");
    // Execute the public API in Lua, including unsupported feature behavior.
    lua_State* lua=luaL_newstate(); Require(lua!=nullptr,"create Lua state");
    luaL_openlibs(lua);
    const snowdesktop::widget_api::FunctionDescriptor functions[]{
        {"hasFeature",snowdesktop::widget_api::LuaHasFeature,1,nullptr,0},
        {"apiInfo",snowdesktop::widget_api::LuaApiInfo,1,nullptr,0}};
    snowdesktop::widget_api::RegisterLibrary(lua,"snow",functions);
    const auto code=luaL_dostring(lua,"assert(snow.hasFeature('unknown.future.feature') == false); local info=snow.apiInfo(); assert(type(info)=='table' and type(info.current)=='number' and info.current>=1); assert(type(info.supported)=='table' and info.supported[1]==info.current); assert(type(info.features)=='table'); assert(not pcall(snow.hasFeature, {}))");
    const std::string failure=code && lua_tostring(lua,-1) ? lua_tostring(lua,-1) : "";
    lua_close(lua); Require(code==LUA_OK,"public API execution: "+failure);
}
void DockLifecycle()
{
    Profile p;
    HWND first=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Owned pin fixture",WS_POPUP,
        -12000,-12000,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Require(first!=nullptr,"create exclusively owned window");
    HWND unrelated=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Pre-existing topmost fixture",WS_POPUP,
        -12100,-12000,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Require(unrelated!=nullptr,"create independent pre-existing topmost window");
    {
        DockWindowPin pins;
        Require(!DockWindowPin::IsPinned(first) && pins.Toggle(first),"pin owned window");
        pins.Refresh(); Require(DockWindowPin::IsPinned(first),"pin remains live after refresh");
        Require(pins.Toggle(first),"explicit unpin"); pins.Refresh();
        Require(!DockWindowPin::IsPinned(first),"explicit unpin restores native state");
        Require(pins.Toggle(first),"repin owned window");
    }
    Require(!DockWindowPin::IsPinned(first) &&
        (GetWindowLongPtrW(unrelated,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0,
        "owner destruction releases its pin while preserving unrelated native TOPMOST");
    DestroyWindow(unrelated);
    DestroyWindow(first);
    DockWindowPin pins; pins.Refresh(); Require(!pins.Toggle(first),"destroyed target cannot acquire a pin");
}
}

namespace behavior
{
// Diagnostic drafts immediately select the diagnostic sink for inspection.
// The real host adapter mirror and JSON persistence change on commit.
// No Windows setting or hotkey changes.
void SettingsHostCommit()
{
    Profile profile;
    using namespace snowdesktop;
    auto& controller = Access::HostSettings(*profile.app);
    const auto before = controller.Snapshot()->values.general;
    auto draft = before;
    draft.diagnosticLogModules ^= diagnostics::ModuleBit(DiagnosticLogModule::Settings);
    controller.UpdateGeneral(draft,SettingsUpdateMode::Draft);
    Require(controller.FlushPending().Succeeded() &&
        Access::AppliedLogModules(*profile.app) == before.diagnosticLogModules &&
        diagnostics::IsLogEnabled(DiagnosticLogModule::Settings) ==
            ((draft.diagnosticLogModules & diagnostics::ModuleBit(DiagnosticLogModule::Settings)) != 0),
        "diagnostic draft selects its sink immediately without committing the host mirror");
    controller.UpdateGeneral(draft,SettingsUpdateMode::Preview);
    Require(controller.FlushPending().Succeeded() &&
        Access::AppliedLogModules(*profile.app) == before.diagnosticLogModules,
        "appearance preview cannot commit the logging preference");
    controller.RequestCommit(SettingsDomain::General);
    Require(controller.FlushPending().Succeeded() &&
        Access::AppliedLogModules(*profile.app) == draft.diagnosticLogModules &&
        diagnostics::IsLogEnabled(DiagnosticLogModule::Settings) ==
            ((draft.diagnosticLogModules & diagnostics::ModuleBit(DiagnosticLogModule::Settings)) != 0),
        "real host commit reconciles its logging mirror with the selected diagnostic sink");
    SettingsValues disk;
    Require(CreateNativeSettingsStore()->Load(disk).Succeeded() &&
        disk.general.diagnosticLogModules == draft.diagnosticLogModules,
        "real host commit persists the applied preference");
}
void GroupTypeContract()
{
    Profile profile;
    const auto desktopFile = profile.desktop / L"A.needle.txt";
    const auto desktopFolder = profile.desktop / L"Z.needle";
    const auto mapped = profile.root / L"mapped";
    const auto mappedFile = mapped / L"A.needle.txt";
    const auto mappedFolder = mapped / L"Z.needle";
    Write(desktopFile,"desktop-source-bytes"); Write(mappedFile,"mapped-source-bytes");
    fs::create_directories(desktopFolder); fs::create_directories(mappedFolder);
    // Each source starts file-before-folder, so both must actually reorder.
    Access::SeedGroupTypeSources(*profile.app,{desktopFile,desktopFolder},mapped);
    const std::vector<fs::path> expected{desktopFolder,desktopFile,mappedFolder,mappedFile};
    const auto check = [&] {
        const auto ordered = Access::GroupTypeOrder(*profile.app);
        Require(ordered.size() == expected.size() &&
            std::equal(ordered.begin(),ordered.end(),expected.begin(),expected.end(),
                [](const auto& a,const auto& b) { return fs::equivalent(a,b); }),
            "user contract: retain source sections and place folders before files inside each section");
        const auto& widgets = Access::Widgets(*profile.app);
        const auto group = std::find_if(widgets.begin(),widgets.end(),[](const auto& value) { return value.id==L"type-group"; });
        Require(group!=widgets.end() && group->activeCategoryId==L"type-desktop" &&
            group->childWidgetIds==std::vector<std::wstring>{L"type-desktop",L"type-mapped"},
            "sorting preserves original source order and active source");
        Require(Read(desktopFile)=="desktop-source-bytes" && Read(mappedFile)=="mapped-source-bytes" &&
            fs::is_empty(desktopFolder) && fs::is_empty(mappedFolder),
            "sorting and reload cannot change either source's files");
    };
    (void)Access::GroupTypeOrder(*profile.app,true); check();
    Require(Access::Save(*profile.app),"persist accepted grouped Type sort");
    const auto refresh = [&](bool disk) {
        const auto delivered = Access::asyncReadMessages;
        Access::RequestAsync(*profile.app,disk);
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::asyncReadMessages>delivered; });
    };
    refresh(false); check();
    Access::DiscardGroupTypeRuntimeState(*profile.app);
    refresh(true); check();
}
std::string ManifestString(const JsonValue& object, std::string_view key)
{
    const auto* value = object.Find(key);
    Require(value && value->IsString() && !value->string.empty(), "manifest has a nonempty " + std::string(key));
    return value->string;
}
void BuildManifest()
{
    // Test target lives in Release/tests/native-host. Inspect the actual
    // arranged product deployment inventory, not source strings or a fixture.
    std::array<wchar_t,32768> buffer{};
    const auto length = GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    Require(length && length < buffer.size(),"resolve test executable location");
    const auto output = fs::path(std::wstring(buffer.data(),length)).parent_path().parent_path().parent_path();
    JsonValue manifest; std::string error;
    const bool parsed = ParseJson(Read(output/L"SnowDesktop.deployment.json"),manifest,&error);
    Require(parsed,"parse actual deployment inventory: "+error);
    const auto* schema = manifest.Find("schemaVersion");
    Require(schema && schema->IsNumber() && schema->number == 1,"supported deployment inventory schema");
    std::set<std::string> paths;
    for (const auto* collection : {"files","appxFragments","notices"})
    {
        const auto* rows = manifest.Find(collection);
        Require(rows && rows->IsArray() && !rows->array.empty(),"actual deployment collection: "+std::string(collection));
        std::set<std::string> unique;
        for (const auto& row : rows->array)
        {
            const auto path = ManifestString(row,"path");
            Require(unique.insert(path).second,"duplicate inventory path: "+path);
            const auto* alternate = row.Find("buildPath");
            const auto physical = alternate ? ManifestString(row,"buildPath") : path;
            const fs::path relative = fs::path(Utf8ToWide(physical));
            Require(!relative.is_absolute() && !relative.has_root_name() &&
                std::none_of(relative.begin(),relative.end(),[](const auto& part) { return part == L".."; }),
                "inventory path must stay inside its build output");
            const auto file = output / relative;
            Require(fs::is_regular_file(file),"missing inventory file: "+physical);
            auto actual = snowdesktop::widget::WidgetPackageManager::Sha256File(file);
            auto expected = ManifestString(row,"sha256");
            const auto upper=[](unsigned char c) { return static_cast<char>(c >= 'a' && c <= 'f' ? c-'a'+'A' : c); };
            std::transform(actual.begin(),actual.end(),actual.begin(),upper);
            std::transform(expected.begin(),expected.end(),expected.begin(),upper);
            Require(actual.size()==64 && actual==expected,"actual inventory hash mismatch: "+physical);
            if (std::string_view(collection)=="files") paths.insert(path);
        }
    }
    for (const auto* required : {"App.xbf","SettingsShell.xbf","SnowDesktop.pri","SnowDesktop.winmd",
        "Microsoft.ui.xaml.dll","Microsoft.WindowsAppRuntime.dll","Microsoft.UI.Xaml.winmd",
        "Assets/Fonts/MiSans/MiSans-Regular.otf","Assets/Fonts/HarmonyOS-Sans/HarmonyOS_Sans_SC_Regular.ttf"})
        Require(paths.contains(required),"mandatory deployed UI/runtime asset missing from inventory: "+std::string(required));
}
}
