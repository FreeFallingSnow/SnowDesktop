#pragma once
#include "data/layout_storage.h"
#include "navigation/search_match.h"
#include "navigation/quick_navigation_search_async.h"
#include "widget/permissions/widget_permission_broker.h"
#include "widget/tasks/widget_task_broker.h"
#include "widget/api/widget_api_registry.h"
#include "dock/dock_window_pin.h"
#include <condition_variable>
#include <mutex>

namespace behavior
{
// Real native JSON persistence; only host notifications are observed here.
// This protects controller draft/commit semantics, not desktop side effects.
struct SettingsObserver final : snowdesktop::SettingsHostActions
{
    int previews = 0, commits = 0;
    snowdesktop::SettingsActionResult OnSettingsPreview(const snowdesktop::SettingsSnapshot&, snowdesktop::SettingsDomain) override
    { ++previews; return snowdesktop::SettingsActionResult::Success(); }
    snowdesktop::SettingsActionResult OnSettingsCommitted(const snowdesktop::SettingsSnapshot&, snowdesktop::SettingsDomain) override
    { ++commits; return snowdesktop::SettingsActionResult::Success(); }
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
    Require(controller.Reload().Succeeded() && controller.Snapshot()->values.general.doubleClickHideDesktop == draft.doubleClickHideDesktop,
        "ordinary reload preserves pending draft");
    controller.RequestCommit(SettingsDomain::General);
    Require(controller.FlushPending().Succeeded() && observer.commits == 1, "explicit commit dispatches once");
    Require(store->Load(disk).Succeeded() && disk.general.doubleClickHideDesktop == draft.doubleClickHideDesktop, "commit reaches real JSON store");
    controller.UpdateGeneral(before.general, SettingsUpdateMode::Preview);
    Require(controller.FlushPending().Succeeded() && observer.previews == 1 && observer.commits == 1,
        "preview notifies once without a new commit");
    controller.PrepareForExternalDataReplacement();
    Require(controller.CloseSession().Succeeded() && store->Load(disk).Succeeded() && disk.general.doubleClickHideDesktop == draft.doubleClickHideDesktop,
        "closing after external replacement cannot flush stale draft over replacement data");
}
void Recovery()
{
    Profile p;
    namespace storage = snowdesktop::layout_storage;
    const auto path = p.root / L"recovery/layout.json";
    const std::string first = R"({"schemaVersion":1,"items":[{"key":"owned-A","page":"page","x":1,"y":2}],"widgets":[{"id":"collection","type":"collection","items":["owned-A"]}]})";
    const std::string second = R"({"schemaVersion":1,"items":[{"key":"owned-B","page":"page","x":3,"y":0}]})";
    std::string error;
    Require(storage::SaveDocument(path, first, &error) && storage::SaveDocument(path, second, &error), "save two actual layout generations: " + error);
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
}
void Search()
{
    // Ranking is asserted relationally: exact, prefix, then interior match.
    std::vector<std::wstring> names{L"z-editor", L"editor notes", L"editor"};
    std::stable_sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return NameSearchMatchRank(a,L"editor") < NameSearchMatchRank(b,L"editor"); });
    Require(names == std::vector<std::wstring>{L"editor", L"editor notes", L"z-editor"}, "exact and prefix results precede substring matches");
    Require(NameMatchesQuery(L"文件管理器", L"wjglq") && !NameMatchesQuery(L"editor", L"unrelated"), "pinyin query and no-match boundary");
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
    const auto code=luaL_dostring(lua,"assert(snow.hasFeature('unknown.future.feature') == false); local info=snow.apiInfo(); assert(type(info)=='table')");
    const std::string failure=code && lua_tostring(lua,-1) ? lua_tostring(lua,-1) : "";
    lua_close(lua); Require(code==LUA_OK,"public API execution: "+failure);
}
void DockLifecycle()
{
    Profile p;
    HWND first=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Owned pin fixture",WS_POPUP,
        -12000,-12000,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Require(first!=nullptr,"create exclusively owned window");
    {
        DockWindowPin pins;
        Require(!DockWindowPin::IsPinned(first) && pins.Toggle(first),"pin owned window");
        pins.Refresh(); Require(DockWindowPin::IsPinned(first),"pin remains live after refresh");
        Require(pins.Toggle(first),"explicit unpin"); pins.Refresh();
        Require(!DockWindowPin::IsPinned(first),"explicit unpin restores native state");
        Require(pins.Toggle(first),"repin owned window");
    }
    Require(!DockWindowPin::IsPinned(first),"owner destruction releases only owned pin");
    DestroyWindow(first);
    DockWindowPin pins; pins.Refresh(); Require(!pins.Toggle(first),"destroyed target cannot acquire a pin");
}
}
