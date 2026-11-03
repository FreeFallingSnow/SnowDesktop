#include "dock/dock_settings.h"
#include "settings/settings_snapshot.h"
#include "settings/settings_store.h"
#include "behavior_fixture.h"
#include "settings/settings_controller.h"
#include "data/layout_storage.h"
#include "widget/permissions/widget_permission_broker.h"
#include "widget/tasks/widget_task_broker.h"
#include "widget/api/widget_api_registry.h"
#include "dock/dock_window_pin.h"
#include "ui/menu/modern_menu_appearance_rules.h"
#include "settings/personalization_initialization.h"
extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}
#include <iostream>
#include <vector>

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
    namespace menu = modern_menu;
    namespace appearance = menu::appearance_rules;
    Require(appearance::DefaultForWindows(10, 19045) == menu::Appearance::Win10FollowSystem &&
        appearance::DefaultForWindows(10, 22000) == menu::Appearance::FollowSystem &&
        appearance::DefaultForWindows(10, 26100) == menu::Appearance::FollowSystem,
        "new profiles choose Win10/Win11 light-dark following by Windows version");
    const auto appearanceFile = profile.root / L"menu-appearance.json";
    PersonalizationSettings menuSettings;
    const int newProfileDefault = static_cast<int>(appearance::DefaultForCurrentWindows());
    const int legacyFollow = static_cast<int>(appearance::LegacyFollowForCurrentWindows());
    Require(appearance::LegacyFollowForWindows(10, 19045) == menu::Appearance::OpaqueFollowSystem &&
        appearance::LegacyFollowForWindows(10, 22000) == menu::Appearance::FollowSystem &&
        appearance::LegacyFollowForWindows(10, 26100) == menu::Appearance::FollowSystem,
        "legacy follow retains Win11 blur or Win10 opaque compatibility by Windows version");
    const auto initialFile = profile.root / L"initial-user/SnowDesktop.personalization.json";
    PersonalizationSettings initialSettings;
    Require(!LoadInitialPersonalization(initialFile, initialSettings) &&
        initialSettings.contextMenuStyle == newProfileDefault,
        "unused profile receives the system-specific menu default through native initialization");
    Write(initialFile.parent_path() / L"SnowDesktop.general.json", "{}");
    initialSettings = PersonalizationSettings{};
    Require(!LoadInitialPersonalization(initialFile, initialSettings) && initialSettings.contextMenuStyle == legacyFollow,
        "older profile with other settings but no appearance file retains its previous follow material");
    Require(!LoadPersonalization(appearanceFile.c_str(), menuSettings) &&
        menuSettings.contextMenuStyle == 0,
        "missing appearance file preserves the caller's legacy baseline");
    Write(appearanceFile, "{}");
    Require(LoadPersonalization(appearanceFile.c_str(), menuSettings) && menuSettings.contextMenuStyle == legacyFollow,
        "existing profiles without a menu field retain their previous follow material");
    Write(appearanceFile, "{\"contextMenuStyle\":0}");
    Require(LoadPersonalization(appearanceFile.c_str(), menuSettings) && menuSettings.contextMenuStyle == legacyFollow &&
        SavePersonalization(appearanceFile.c_str(), menuSettings),
        "unversioned follow migrates by Windows version and persists the migration");
    PersonalizationSettings migratedSettings;
    Require(LoadPersonalization(appearanceFile.c_str(), migratedSettings) && migratedSettings.contextMenuStyle == legacyFollow,
        "migrated follow remains stable after save and reload");
    for (int storedStyle = 0; storedStyle <= 8; ++storedStyle)
    {
        if (storedStyle >= 1 && storedStyle <= 6)
        {
            Write(appearanceFile, "{\"contextMenuStyle\":" + std::to_string(storedStyle) + "}");
            Require(LoadPersonalization(appearanceFile.c_str(), menuSettings) && menuSettings.contextMenuStyle == storedStyle,
                "legacy explicit light/dark choices retain their stored style");
        }
        Write(appearanceFile, "{\"contextMenuStyleVersion\":1,\"contextMenuStyle\":" + std::to_string(storedStyle) + "}");
        Require(LoadPersonalization(appearanceFile.c_str(), menuSettings) &&
            menuSettings.contextMenuStyle == storedStyle && SavePersonalization(appearanceFile.c_str(), menuSettings),
            "legacy and new menu choices keep their stable stored IDs");
        PersonalizationSettings reloaded;
        Require(LoadPersonalization(appearanceFile.c_str(), reloaded) && reloaded.contextMenuStyle == storedStyle,
            "all nine menu choices survive a physical save/reload");
    }
    Require(appearance::ResolveForCurrentWindows(static_cast<menu::Appearance>(0), true) == menu::Appearance::OpaqueLight &&
        appearance::ResolveForCurrentWindows(static_cast<menu::Appearance>(0), false) == menu::Appearance::OpaqueDark,
        "new explicit opaque-follow remains opaque in both system themes");
    Require(appearance::ResolveForCurrentWindows(menu::Appearance::FollowSystem, true) == menu::Appearance::SystemLightBlur &&
        appearance::ResolveForCurrentWindows(menu::Appearance::FollowSystem, false) == menu::Appearance::SystemDarkBlur &&
        appearance::ResolveForCurrentWindows(menu::Appearance::Win10FollowSystem, true) == menu::Appearance::Win10Light &&
        appearance::ResolveForCurrentWindows(menu::Appearance::Win10FollowSystem, false) == menu::Appearance::Win10Dark,
        "new follow choices retain their style family in both system themes");
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
    const std::string first = R"({"layoutSchemaVersion":1,"pages":[{"id":"page","columns":4,"rows":3}],"items":[{"key":"owned-A","page":"page","x":1,"y":2}],"widgets":[{"id":"collection","page":"page","x":0,"y":0,"type":"collection","items":["owned-A"]}]})";
    const std::string second = R"({"layoutSchemaVersion":1,"pages":[{"id":"page","columns":4,"rows":3}],"items":[{"key":"owned-B","page":"page","x":3,"y":0}]})";
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
    Profile profile;
    Apartment apartment;
    WindowOwner firstOwner, unrelatedOwner;
    HWND first=firstOwner.handle=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Owned pin fixture",WS_POPUP,
        -12000,-12000,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Require(first!=nullptr,"create exclusively owned window");
    HWND unrelated=unrelatedOwner.handle=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Pre-existing topmost fixture",WS_POPUP,
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
    unrelatedOwner.handle = firstOwner.handle = nullptr;
    DockWindowPin pins; pins.Refresh(); Require(!pins.Toggle(first),"destroyed target cannot acquire a pin");
}
}
int main(int argc, char** argv)
{
    try {
        behavior::Require(argc == 2, "one named behavior is required");
        behavior::scene = argv[1];
        if (behavior::scene == "settings") behavior::Settings();
        else if (behavior::scene == "recovery") behavior::Recovery();
        else if (behavior::scene == "widget") behavior::WidgetContracts();
        else if (behavior::scene == "dock") behavior::DockLifecycle();
        else throw std::runtime_error("unknown module behavior");
        std::cout << "PASS " << behavior::scene << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
