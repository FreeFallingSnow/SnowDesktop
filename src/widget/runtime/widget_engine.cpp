/**
 * @file widget_engine.cpp
 * @brief Coordinates widget VM lifetimes and platform services.
 */
#include "widget_engine.h"
#include "platform/application_version.h"

#include "common/l10n.h"
#include "common/utils.h"
#include "data/data_paths.h"
#include "diagnostics/performance_trace.h"
#include "system/system_snapshot.h"
#include "widget/api/widget_api_registry.h"
#include "widget/packages/widget_package_host.h"
#include "widget/packages/widget_package_manager.h"
#include "widget/packages/widget_package_paths.h"
#include "widget/permissions/widget_permission_broker.h"
#include "widget/preview/widget_preview_context.h"
#include "widget/settings/widget_settings_declarations.h"
#include "widget/settings/widget_system_settings.h"
#include "widget/storage/widget_secret_store.h"
#include "widget/storage/widget_storage_host.h"
#include "widget/storage/widget_storage_runtime.h"
#include "widget/storage/widget_storage_value.h"
#include "widget/storage/widget_storage_write_budget.h"
#include "widget/tasks/widget_audio_output_task_executor.h"
#include "widget/tasks/widget_audio_playback.h"
#include "widget/tasks/widget_filesystem_watch_service.h"
#include "widget/tasks/widget_location_task_executor.h"
#include "widget/tasks/widget_media_task_executor.h"
#include "widget/tasks/widget_native_task_executor.h"
#include "widget/view/widget_host_input_support.h"
#include "widget/view/widget_layout_lua.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_resource_host.h"
#include "widget/view/widget_surface_host.h"
#include "widget_calendar_provider.h"
#include "widget_lua_binding_context.h"
#include "widget_menu_catalogue.h"
#include "widget_module_lua.h"
#include "widget_multiplayer.h"
#include "widget_timer_dispatch.h"
#include "widgets/widget_chrome_rules.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

using snowdesktop::widget_runtime::TrimWidgetPackageHistory;
using snowdesktop::widget_runtime::GetWidgetPackageManager;
using snowdesktop::widget_runtime::ResolveWidgetPath;
using snowdesktop::widget_runtime::RecoverWidgetPackage;
using snowdesktop::widget_runtime::ResolveWidgetEntryPackage;
using snowdesktop::widget::ResolvePackageAssetWithinRoot;
using snowdesktop::widget_runtime::ReadLuaDeclaredSettings;
using snowdesktop::widget_runtime::ValidateWidgetSettings;
using snowdesktop::widget_runtime::ActiveStorage;
using snowdesktop::widget_runtime::SaveStorageFile;

/**
 * @brief 以二进制模式读取文本文件全部内容
 * @param path 文件路径（宽字符）
 * @return 文件内容的 UTF-8 字符串，读取失败时返回空串
 */
static std::string ReadTextFile(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

using snowdesktop::widget_runtime::PreviewExecutionScope;

// ── Drawing API ──────────────────────────────────────────────────

// ── WidgetEngine ──────────────────────────────────────────────────
WidgetEngine::WidgetEngine() = default;

WidgetEngine::~WidgetEngine()
{
    Shutdown();
}

bool WidgetEngine::Init(ID2D1DeviceContext* d2dContext, IDWriteFactory* dwriteFactory)
{
    previewOnly_ = false;
    d2dContext_ = d2dContext;
    dwriteFactory_ = dwriteFactory;

    // Allocate D2D state
    d2dState_ = new D2DState{};
    d2dState_->packageImageCache.SetReadyCallback(taskWakeCallback_);
    d2dState_->dwrite = dwriteFactory_.Get();
    d2dState_->engine = this;
    d2dState_->shellIconLoader =
        std::make_unique<AsyncShellIconLoader>(
            [this](const std::wstring& widgetId) {
                RuntimeInvalidateHost(widgetId);
            });

    // Warm the validated package catalogue and this UI thread's menu metadata
    // before loading layouts, so the first Add Widget popup only probes changes.
    TrimWidgetPackageHistory(GetWidgetPackageManager());
    (void)ListAvailableMenuEntries();
    PrewarmWidgetPackageResources(d2dState_);

    InitializeWidgetStorage();
    filesystemHandleStore_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetFilesystemHandleStore>(
            GetDataFilePath(L"SnowDesktop.widget-file-handles.json"));
    std::string filesystemHandleError;
    if (!filesystemHandleStore_->Load(filesystemHandleError))
    {
        const std::string diagnostic =
            "SnowDesktop filesystem handle registry load failed: " +
            filesystemHandleError + "\n";
        OutputDebugStringA(diagnostic.c_str());
    }
    filesystemWatchService_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetFilesystemWatchService>();
    calendarService_ =
        std::make_unique<
            snowdesktop::calendar::CalendarService>(
            GetDataFilePath(
                L"SnowDesktop.calendar.json"));
    calendarService_->SetChangedCallback(
        [this](const std::string& reason) {
            if (reason == "selection")
                pendingCalendarSelectionChange_ = true;
            else if (reason == "events")
                pendingCalendarEventsChange_ = true;
        });
    calendarService_->SetNotificationCallback(
        [this](
            const snowdesktop::calendar::CalendarEvent& event) {
            if (!notifyCallback_)
                return;
            (void)notifyCallback_({
                snowdesktop::widget_runtime::
                    WidgetNotificationHostOperation::Show,
                {}, _LW("calendar.notification_title"),
                Utf8ToWide(event.title) });
        });
    calendarServiceLoaded_ = snowdesktop::widget_runtime::LoadWidgetCalendarProvider(
        *calendarService_, GetDataFilePath(L"SnowDesktop.calendar.json"));
    systemSnapshotService_ = std::make_unique<SystemSnapshotService>();
    httpService_ = std::make_unique<AsyncHttpService>();
    InitializeWidgetDataBroker();
    InitializeWidgetTaskBroker();
    LoadNotificationSchedules();
    (void)d2dState_->packageImageCache.PrewarmPending();
    return true;
}

bool WidgetEngine::InitPreview(
    ID2D1DeviceContext* d2dContext, IDWriteFactory* dwriteFactory)
{
    previewOnly_ = true;
    d2dContext_ = d2dContext;
    dwriteFactory_ = dwriteFactory;
    d2dState_ = new D2DState{};
    d2dState_->dwrite = dwriteFactory_.Get();
    d2dState_->engine = this;
    // Package metadata and Lua files remain readable.  Shell icon loading,
    // storage, calendar, system snapshots and HTTP workers are deliberately
    // absent in this render-only engine.
    (void)GetWidgetPackageManager();
    InitializeWidgetDataBroker();
    InitializeWidgetTaskBroker();
    return d2dState_ != nullptr;
}

void WidgetEngine::ResetGraphicsResources(ID2D1DeviceContext* context)
{
    d2dContext_ = context;
    if (!d2dState_) return;
    d2dState_->ctx = nullptr;
    d2dState_->bitmapDevice.Reset();
    d2dState_->immediateCommandContext.Reset();
    d2dState_->backgroundCache.Clear();
    d2dState_->imageSamplingCache.Clear();
    d2dState_->ResetBrushes();
    d2dState_->imageCache.clear();
    d2dState_->imageCacheRevisions.clear();
    d2dState_->runtimeImageCache.ClearBitmaps();
    ClearShellIconBitmaps(d2dState_);
    d2dState_->shellIconFailures.clear();
    for (auto& widget : widgets_)
    {
        widget.desktopMarquee = {};
        widget.panelMarquee = {};
    }
}

void WidgetEngine::Shutdown()
{
    if (d2dState_) d2dState_->scriptReadAhead.Stop();
    ResetFocusedHostInput();
    if (taskBroker_)
        taskBroker_->Shutdown();
    if (notificationCenter_)
        notificationCenter_->Clear(notifyCallback_);
    if (d2dState_)
        d2dState_->shellIconLoader.reset();
    for (auto& widget : widgets_)
    {
        ClearNativeMarqueeComposition(widget);
        if (widget.valid && widget.hostVisible)
            InvokeSimpleCallback(widget, "onHidden");
        DisposeWidgetLifecycle(widget, "shutdown");
        ReleaseWidgetDataSubscriptions(widget);
        ReleaseWidgetTasks(widget,
            snowdesktop::widget_runtime::TaskBrokerCancelReason::Shutdown);
        if (widget.refreshTimerId && widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.refreshTimerId);
        if (widget.namedTimerId && widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.namedTimerId);
        StopAnimationFrames(widget);
    }
    if (systemSnapshotService_)
    {
        systemSnapshotService_->Stop();
        systemSnapshotService_.reset();
        systemSnapshotServiceStarted_ = false;
    }
    if (httpService_)
    {
        httpService_->Stop();
        httpService_.reset();
    }
    calendarService_.reset();
    calendarServiceLoaded_ = false;
    pendingCalendarSelectionChange_ = false;
    pendingCalendarEventsChange_ = false;
    for (auto& widget : widgets_)
    {
        if (widget.state)
        {
            widget.lifecycle.Release(widget.state);
            luaL_unref(widget.state, LUA_REGISTRYINDEX, widget.ref);
            ReleasePackageImageResources(d2dState_, widget.state);
            lua_close(widget.state);
            widget.state = nullptr;
        }
    }
    if (dataBroker_)
    {
        dataBroker_->Shutdown(
            snowdesktop::widget_runtime::WidgetDataBroker::Clock::now());
        ApplyWidgetDataBrokerActions();
    }
    if (widgetSystemDataProvider_)
        widgetSystemDataProvider_->RemoveConsumer("widgets");
    if (widgetAudioAnalysisProvider_)
        widgetAudioAnalysisProvider_->Stop();
    widgets_.clear();
    systemControlTasks_.reset();
    widgetSystemDataProvider_.reset();
    widgetAudioAnalysisProvider_.reset();
    dataBroker_.reset();
    nativeTaskExecutor_.reset();
    nativeTaskDispatch_.Clear();
    locationTaskExecutor_.reset();
    taskResults_.Clear();
    audioPlayback_.reset();
    mediaTaskExecutor_.reset();
    audioOutputTaskExecutor_.reset();
    clipboardTaskExecutor_.reset();
    filesystemWatchService_.reset();
    filesystemHandleStore_.reset();
    secretStore_.reset();
    filesystemTaskExecutor_.reset();
    appTaskExecutor_.reset();
    desktopTaskExecutor_.reset();
    externalItemTaskExecutor_.reset();
    networkTaskDispatch_.Clear();
    clipboardTaskDispatch_.Clear();
    filesystemTaskDispatch_.Clear();
    filesystemWatchBindings_.clear();
    taskBroker_.reset();
    notificationCenter_.reset();
    notificationScheduleStore_.reset();
    notificationSchedulePath_.clear();
    widgetHostFailures_.clear();
    delete d2dState_; d2dState_ = nullptr;
}

void WidgetEngine::UnloadWidget(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    if (d2dState_) d2dState_->scriptReadAhead.Forget(widgetId);
    RemoveNotificationSchedules(widgetId);
    PreviewExecutionScope previewScope(
        widgets_[idx].preview ? &widgets_[idx].previewStorage : nullptr);
    ResetHostInputForWidget(widgetId);
    if (widgets_[idx].hostVisible)
        InvokeSimpleCallback(widgets_[idx], "onHidden");
    ClearNativeMarqueeComposition(widgets_[idx]);
    DisposeWidgetLifecycle(widgets_[idx], "unload");
    ReleaseWidgetDataSubscriptions(widgets_[idx]);
    ReleaseWidgetTasks(widgets_[idx],
        snowdesktop::widget_runtime::TaskBrokerCancelReason::InstanceDisposed);
    if (widgets_[idx].refreshTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widgets_[idx].refreshTimerId);
    if (widgets_[idx].namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widgets_[idx].namedTimerId);
    StopAnimationFrames(widgets_[idx]);
    if (httpService_) httpService_->CancelWidget(widgetId);
    lua_State* state = widgets_[idx].state;
    if (state)
    {
        widgets_[idx].lifecycle.Release(state);
        luaL_unref(state, LUA_REGISTRYINDEX, widgets_[idx].ref);
        ReleasePackageImageResources(d2dState_, state);
        lua_close(state);
        widgets_[idx].state = nullptr;
    }
    if (d2dState_)
    {
        ClearRuntimeImagesForWidget(d2dState_, widgetId);
        d2dState_->ClearPrivateTextFormats();
    }
    widgets_.erase(widgets_.begin() + idx);
    std::erase_if(widgets_, [&widgetId](const LuaWidget& widget) {
        return widget.widgetId == widgetId;
    });

}

bool WidgetEngine::RequiresRemovalConfirmation(const std::wstring& widgetId,
    const std::wstring& packageId)
{
    const int index = FindWidget(widgetId);
    if (index >= 0 && widgets_[index].manifest.confirmRemoval)
        return true;
    // Resolve metadata even if the script failed to load or was disabled.
    if (const auto package = GetWidgetPackage(packageId))
        return package->manifest.confirmRemoval;
    // A missing package must not silently discard an orphaned instance's data.
    return index < 0;
}

void WidgetEngine::DeleteWidgetInstance(const std::wstring& widgetId)
{
    RemoveNotificationSchedules(widgetId);
    if (filesystemHandleStore_)
    {
        std::string error;
        (void)filesystemHandleStore_->RevokeInstance(
            WideToUtf8(widgetId), error);
    }
    UnloadWidget(widgetId);
    if (secretStore_)
    {
        bool removed = false;
        std::string error;
        if (!secretStore_->RemoveInstance(
                WideToUtf8(widgetId), removed, error))
        {
            OutputDebugStringA((
                "SnowDesktop widget secret cleanup failed: " + error +
                "\n").c_str());
        }
    }
    widgetHostFailures_.erase(widgetId);
    std::string prefix = WideToUtf8(widgetId) + ".";
    auto& storage = ActiveStorage();
    auto it = storage.begin();
    while (it != storage.end())
    {
        if (it->first.compare(0, prefix.size(), prefix) == 0)
            it = storage.erase(it);
        else
            ++it;
    }
    if (!snowdesktop::widget_runtime::HasStorageOverlay()) SaveStorageFile();
}

int WidgetEngine::FindWidget(const std::wstring& widgetId) const
{
    for (int i = static_cast<int>(widgets_.size()) - 1; i >= 0; --i)
    {
        if (widgets_[i].widgetId == widgetId)
            return i;
    }
    return -1;
}

void WidgetEngine::ActivateWidgetState(const std::wstring& widgetId)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || !widgets_[index].state)
        return;
    const auto& widget = widgets_[index];
    if (d2dState_)
    {
        snowdesktop::widget_runtime::ApplyLayoutSpan(
            *d2dState_, widget.lastColumns, widget.lastRows);
        snowdesktop::widget_runtime::ApplyLayoutMetrics(
            *d2dState_, widget.layoutMetrics);
    }
}

void WidgetEngine::InvokeSimpleCallback(LuaWidget& widget, const char* callbackName)
{
    if (std::strcmp(callbackName, "onVisible") == 0 ||
        std::strcmp(callbackName, "onHidden") == 0)
    {
        const bool visible = std::strcmp(
            callbackName, "onVisible") == 0;
        (void)InvokeLifecycleEvent(widget, "visibility",
            [visible](lua_State* state) {
                lua_pushboolean(state, visible ? 1 : 0);
                lua_setfield(state, -2, "visible");
            });
    }
    else if (std::strcmp(callbackName, "onOpen") == 0)
    {
        (void)InvokeLifecycleEvent(widget, "action",
            [](lua_State* state) {
                lua_pushliteral(state, "open");
                lua_setfield(state, -2, "id");
            });
    }
    else if (std::strcmp(callbackName, "onSelected") == 0)
    {
        (void)InvokeLifecycleEvent(widget, "selection",
            [](lua_State* state) {
                lua_pushboolean(state, 1);
                lua_setfield(state, -2, "selected");
            });
    }
    else if (std::strcmp(callbackName, "onLanguageChanged") == 0)
    {
        (void)InvokeLifecycleEvent(widget, "environment",
            [](lua_State* state) {
                lua_pushliteral(state, "language");
                lua_setfield(state, -2, "reason");
            });
    }
}

bool WidgetEngine::InitializeWidgetLifecycle(LuaWidget& widget)
{
    snowdesktop::performance::Scope performanceScope(
        "widget.lifecycle", "setup", widget.widgetId);
    lua_State* state = widget.state;
    if (!state) return false;
    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widget.widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    SetWidgetRectContext(d2dState_, BoundsForSurface(
        widget, CurrentWidgetSurface(d2dState_)));
    std::string error;
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    if (widget.lifecycle.Setup(
            state, widget.ref, pushContext, error))
    {
        (void)snowdesktop::widget_api::ConsumeTransientStateDirty(state);
        return true;
    }
    RuntimeRecordError(widget.widgetId,
        error.empty() ? "Widget setup failed" : error);
    RecordWidgetHostFailure(widget.widgetId,
        error.empty() ? "Widget setup failed" : error,
        widget.quota && (widget.quota->memoryExceeded ||
            widget.quota->executionExceeded));
    return false;
}

bool WidgetEngine::InvokeLifecycleEvent(LuaWidget& widget,
    const char* kind,
    const std::function<void(lua_State*)>& pushFields)
{
    snowdesktop::performance::Scope performanceScope(
        "widget.event", kind ? kind : "unknown", widget.widgetId);
    if (!widget.state || !kind || !*kind)
        return false;
    snowdesktop::widget_runtime::WidgetInvalidationBatch::Scope
        invalidationScope(invalidationBatch_, invalidateCallback_);
    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widget.widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(widget.state);
    SetWidgetRectContext(d2dState_, BoundsForSurface(
        widget, CurrentWidgetSurface(d2dState_)));
    bool invoked = false;
    std::string error;
    struct EventArguments
    {
        const char* kind;
        const std::function<void(lua_State*)>* fields;
    } arguments{kind, &pushFields};
    if (!snowdesktop::lua_runtime::ProtectedHostCall(widget.state,
            +[](lua_State* inner) {
                const auto* args = static_cast<EventArguments*>(lua_touserdata(inner, 1));
                lua_createtable(inner, 0, 10);
                lua_pushstring(inner, args->kind);
                lua_setfield(inner, -2, "kind");
                if (*args->fields) (*args->fields)(inner);
                return 1;
            }, &arguments, 1, error))
    {
        RuntimeRecordError(widget.widgetId, error);
        return false;
    }
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    const bool succeeded = widget.lifecycle.Event(
        widget.state, widget.ref, pushContext, -1, invoked, error);
    if (!succeeded && !error.empty())
        RuntimeRecordError(widget.widgetId, error);
    else if (succeeded && invoked)
        RuntimeRecordSuccess(widget.widgetId);
    const bool stateChanged = snowdesktop::widget_api::
        ConsumeTransientStateDirty(widget.state);
    if ((succeeded && invoked) || stateChanged)
        RuntimeInvalidateHost(widget.widgetId);
    return succeeded && invoked;
}

void WidgetEngine::DisposeWidgetLifecycle(
    LuaWidget& widget, const char* reason)
{
    snowdesktop::performance::Scope performanceScope(
        "widget.lifecycle", "dispose", widget.widgetId);
    if (!widget.state) return;
    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widget.widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(widget.state);
    SetWidgetRectContext(d2dState_, BoundsForSurface(
        widget, CurrentWidgetSurface(d2dState_)));
    std::string error;
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    if (!widget.lifecycle.Dispose(widget.state, widget.ref,
            pushContext, reason, error) && !error.empty())
    {
        RuntimeRecordError(widget.widgetId, error);
    }
    (void)snowdesktop::widget_api::ConsumeTransientStateDirty(widget.state);
}

bool WidgetEngine::EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& packageId)
{
    if (const int index = FindWidget(widgetId); index >= 0)
    {
        using snowdesktop::widget_runtime::WidgetHostStateKind;
        const auto state = GetWidgetHostState(widgetId, packageId).kind;
        if (state == WidgetHostStateKind::PermissionPending ||
            state == WidgetHostStateKind::PermissionDenied)
        {
            UnloadWidget(widgetId);
            return false;
        }
        return widgets_[index].valid;
    }
    if (const auto failure = widgetHostFailures_.find(widgetId);
        failure != widgetHostFailures_.end())
    {
        // A retained desktop instance may outlive its unsubscribed package.
        // Missing content is recoverable as soon as it is installed again;
        // actual script/quota failures still require the normal retry action.
        if (failure->second.kind !=
                snowdesktop::widget_runtime::WidgetHostStateKind::PackageMissing ||
            !IsWidgetPackageInstalled(packageId))
            return false;
        widgetHostFailures_.erase(failure);
    }
    if (const auto package = GetWidgetPackage(packageId))
    {
        const auto grant = snowdesktop::widget::WidgetPermissionBroker::
            Evaluate(package->permissionState,
                package->manifest.permissions,
                package->manifest.optionalPermissions,
                package->manifest.networkDomains,
                package->grantedPermissions,
                package->grantedNetworkDomains);
        if (grant.runtimeBlock !=
            snowdesktop::widget::PermissionRuntimeBlock::None)
        {
            return false;
        }
    }
    const std::wstring path = ResolveWidgetPath(packageId);
    if (path.empty())
    {
        RuntimeRecordError(widgetId,
            "Widget package is missing, disabled, or has not been migrated");
        if (!IsWidgetPackageInstalled(packageId))
            widgetHostFailures_[widgetId].kind =
                snowdesktop::widget_runtime::WidgetHostStateKind::PackageMissing;
        return false;
    }
    if (LoadWidget(path, widgetId))
        return true;
    const std::string id = WideToUtf8(packageId);
    if (!RecoverWidgetPackage(id))
        return false;
    const std::wstring fallback = ResolveWidgetPath(packageId);
    return !fallback.empty() && fallback != path &&
        LoadWidget(fallback, widgetId);
}

bool WidgetEngine::EnsureWidgetPreviewLoaded(
    const std::wstring& widgetId, const std::wstring& packageId,
    const std::unordered_map<std::string, std::string>& storageOverrides)
{
    if (int index = FindWidget(widgetId); index >= 0)
        return widgets_[index].preview &&
            widgets_[index].packageId == WideToUtf8(packageId);
    const std::wstring path = ResolveWidgetPath(packageId);
    return !path.empty() && LoadWidget(
        path, widgetId, true, &storageOverrides);
}

bool WidgetEngine::EnsureWidgetDirectoryPreviewLoaded(
    const std::wstring& widgetId,
    const std::filesystem::path& packageRoot,
    const std::unordered_map<std::string, std::string>& storageOverrides,
    const LuaWidgetAuthorPreviewConfiguration* previewConfiguration)
{
    if (!previewOnly_)
    {
        RuntimeRecordError(widgetId,
            "development packages may only load in a preview engine");
        return false;
    }
    snowdesktop::widget::PackageManifest manifest;
    const auto report = GetWidgetPackageManager().ValidateDirectory(
        packageRoot, &manifest);
    if (!report.Ok())
    {
        std::string message = "component package validation failed";
        for (const auto& issue : report.issues)
        {
            if (issue.severity !=
                snowdesktop::widget::ValidationSeverity::Error)
                continue;
            message = issue.code + ": " + issue.message;
            break;
        }
        RuntimeRecordError(widgetId, message);
        return false;
    }
    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(
        packageRoot, error);
    if (error)
    {
        RuntimeRecordError(widgetId,
            "component package root cannot be canonicalized");
        return false;
    }
    const auto entry = std::filesystem::weakly_canonical(
        canonicalRoot / Utf8ToWide(manifest.entry), error);
    if (error)
    {
        RuntimeRecordError(widgetId,
            "component entry cannot be canonicalized");
        return false;
    }
    if (const int index = FindWidget(widgetId); index >= 0)
    {
        std::error_code equivalentError;
        return widgets_[index].preview &&
            widgets_[index].packageId == manifest.id &&
            std::filesystem::equivalent(widgets_[index].packageRoot,
                canonicalRoot, equivalentError) && !equivalentError;
    }
    return LoadWidget(entry.wstring(), widgetId, true,
        &storageOverrides, &canonicalRoot, previewConfiguration);
}

bool WidgetEngine::IsPreviewWidget(const std::wstring& widgetId) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 && widgets_[index].preview;
}

bool WidgetEngine::LoadWidget(const std::wstring& path,
    const std::wstring& widgetId, bool preview,
    const std::unordered_map<std::string, std::string>*
        previewStorageOverrides,
    const std::filesystem::path* packageRootOverride,
    const LuaWidgetAuthorPreviewConfiguration* previewConfiguration,
    bool* deterministicFailure)
{
    const auto markDeterministicFailure = [deterministicFailure] {
        if (deterministicFailure) *deterministicFailure = true;
    };
    LuaWidget pending;
    pending.widgetId = widgetId;
    pending.filePath = path;
    pending.manifest = GetWidgetManifest(path);
    pending.packageId = pending.manifest.packageId;
    pending.preview = preview;
    if (!snowdesktop::widget::IsExecutablePackageContract(
            pending.manifest.schemaVersion,
            pending.manifest.apiVersion))
    {
        RuntimeRecordError(widgetId,
            "Widget schema/API contract is not supported by this host");
        return false;
    }
    std::string source = ReadTextFile(path);
    if (source.empty())
    {
        RuntimeRecordError(widgetId,
            "Widget entry script is empty or cannot be read");
        return false;
    }
    if (preview)
    {
        const std::string prefix = WideToUtf8(widgetId) + ".";
        for (const auto& [key, value] : pending.manifest.previewStorage)
            pending.previewStorage[prefix + key] = value;
        if (previewStorageOverrides)
        {
            for (const auto& [key, value] : *previewStorageOverrides)
                pending.previewStorage[prefix + key] = value;
        }
    }
    PreviewExecutionScope previewScope(
        preview ? &pending.previewStorage : nullptr);
    {
        std::string slotError;
        if (!pending.logicalSlots.Configure(
                pending.manifest.logicalSlots, slotError) ||
            !pending.logicalSlots.Restore(ActiveStorage(),
                WideToUtf8(widgetId), slotError))
        {
            RuntimeRecordError(widgetId,
                "Widget logical slot state is invalid: " + slotError);
            return false;
        }
    }
    if (packageRootOverride)
        pending.packageRoot = *packageRootOverride;
    else if (const auto package = ResolveWidgetEntryPackage(path))
        pending.packageRoot = package->root;
    else
        pending.packageRoot = std::filesystem::path(path).parent_path();
    if (pending.packageId.empty())
    {
        RuntimeRecordError(widgetId, "Legacy loose Lua scripts cannot run directly");
        return false;
    }
    if (!pending.manifest.signatureValid)
    {
        RuntimeRecordError(widgetId, "Widget signature validation failed");
        return false;
    }
    if (!snowdesktop::widget::MeetsWidgetMinimumHostVersion(
            snowdesktop::ApplicationVersion(), pending.manifest.minHostVersion))
    {
        markDeterministicFailure();
        RuntimeRecordError(widgetId, "Widget requires SnowDesktop " +
            pending.manifest.minHostVersion + " or newer");
        return false;
    }
    auto grant = snowdesktop::widget::WidgetPermissionBroker::
        GrantForIsolatedExecution(
            pending.manifest.permissions,
            pending.manifest.optionalPermissions,
            pending.manifest.networkDomains);
    if (!preview && !snowdesktop::widget_runtime::IsDryLoad())
    {
        if (const auto package =
            GetWidgetPackageManager().Resolve(pending.packageId))
        {
            grant = snowdesktop::widget::WidgetPermissionBroker::Evaluate(
                package->permissionState,
                pending.manifest.permissions,
                pending.manifest.optionalPermissions,
                pending.manifest.networkDomains,
                package->grantedPermissions,
                package->grantedNetworkDomains);
        }
    }
    switch (grant.runtimeBlock)
    {
    case snowdesktop::widget::PermissionRuntimeBlock::PendingConsent:
        RuntimeRecordError(widgetId,
            "Widget permission consent is pending");
        return false;
    case snowdesktop::widget::PermissionRuntimeBlock::Denied:
        RuntimeRecordError(widgetId,
            "Widget permission consent was denied");
        return false;
    case snowdesktop::widget::PermissionRuntimeBlock::MissingRequired:
        RuntimeRecordError(widgetId,
            "Widget required permissions are not granted");
        return false;
    case snowdesktop::widget::PermissionRuntimeBlock::None:
        break;
    }
    pending.permissions.insert(
        grant.permissions.begin(), grant.permissions.end());

    if (preview && previewConfiguration && d2dState_)
    {
        d2dState_->gridColumns = std::max(
            1, previewConfiguration->columns);
        d2dState_->gridRows = std::max(1, previewConfiguration->rows);
        SetWidgetRectContext(d2dState_, previewConfiguration->bounds);
    }
    WidgetExecutionContextGuard loadContext(d2dState_, widgetId);
    auto quota = std::make_unique<LuaRuntimeQuota>();
    auto storageWriteBudget = std::make_unique<
        snowdesktop::widget_runtime::WidgetStorageWriteBudget>();
    lua_State* state = lua_newstate(snowdesktop::lua_runtime::QuotaAllocator, quota.get());
    if (!state)
    {
        RuntimeRecordError(widgetId, "Cannot allocate isolated Lua state");
        RecordWidgetHostFailure(widgetId,
            "Cannot allocate isolated Lua state", quota->memoryExceeded);
        return false;
    }
    const auto closePendingState = [d2dState = d2dState_](lua_State* value) {
        if (!value) return;
        ReleasePackageImageResources(d2dState, value);
        lua_close(value);
    };
    std::unique_ptr<lua_State, decltype(closePendingState)> stateGuard(
        state, closePendingState);
    const auto missing = snowdesktop::widget_api::MissingFeatures(
        pending.manifest.requiredFeatures);
    if (!missing.empty())
    {
        markDeterministicFailure();
        std::string message = "Widget requires unsupported host feature";
        if (missing.size() > 1) message += "s";
        message += ": ";
        for (std::size_t index = 0; index < missing.size(); ++index)
        {
            if (index) message += ", ";
            message += missing[index];
        }
        RuntimeRecordError(widgetId, message);
        return false;
    }
    luaL_requiref(state, "_G", luaopen_base, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(state, 1);
    RegisterDrawAPI(state);
    lua_pushlightuserdata(state, d2dState_);
    lua_setfield(state, LUA_REGISTRYINDEX, "__d2d_ptr");
    snowdesktop::widget_runtime::BindWidgetLuaEngine(state,
        d2dState_ ? d2dState_->engine : nullptr);
    BindWidgetDrawHost(state, d2dState_);
    snowdesktop::widget_runtime::BindWidgetLayoutContext(state, d2dState_);
    BindWidgetHostStorage(state, d2dState_ != nullptr);
    lua_pushlightuserdata(state, quota.get());
    lua_setfield(state, LUA_REGISTRYINDEX, "__quota_ptr");
    lua_pushlightuserdata(state, storageWriteBudget.get());
    lua_setfield(state, LUA_REGISTRYINDEX,
        "__storage_write_budget_ptr");
    lua_pushstring(state, WideToUtf8(widgetId).c_str());
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_id");
    lua_createtable(state, 0,
        static_cast<int>(pending.permissions.size()));
    for (const auto& permission : pending.permissions)
    {
        lua_pushboolean(state, true);
        lua_setfield(state, -2, permission.c_str());
    }
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_permissions");
    lua_pushstring(state,
        WideToUtf8(pending.packageRoot.wstring()).c_str());
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_package_root");
    lua_createtable(state, 0,
        static_cast<int>(pending.manifest.resources.size()));
    for (const auto& [name, resource] : pending.manifest.resources)
    {
        lua_createtable(state, 0, 3);
        lua_pushlstring(state, resource.type.data(), resource.type.size());
        lua_setfield(state, -2, "type");
        lua_pushlstring(state, resource.path.data(), resource.path.size());
        lua_setfield(state, -2, "path");
        lua_pushlstring(state, resource.license.data(),
            resource.license.size());
        lua_setfield(state, -2, "license");
        lua_setfield(state, -2, name.c_str());
    }
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_resources");
    lua_createtable(state, 0,
        static_cast<int>(pending.manifest.resources.size()));
    for (const auto& [name, resource] : pending.manifest.resources)
    {
        const auto resolved = ResolvePackageAssetWithinRoot(
            pending.packageRoot, Utf8ToWide(resource.path));
        if (!resolved) continue;
        const std::string utf8Path = WideToUtf8(*resolved);
        lua_pushlstring(state, utf8Path.data(), utf8Path.size());
        lua_setfield(state, -2, name.c_str());
    }
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_resource_paths");
    lua_createtable(state, 0,
        static_cast<int>(pending.manifest.resources.size()));
    lua_setfield(state, LUA_REGISTRYINDEX,
        "__widget_resource_content_keys");
    lua_pushboolean(state, preview ? 1 : 0);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_preview");
    lua_pushinteger(state, std::max(1, pending.manifest.defaultColumns));
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_default_columns");
    lua_pushinteger(state, std::max(1, pending.manifest.defaultRows));
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_default_rows");
    lua_pushboolean(state, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_loading");
    if (d2dState_)
    {
        d2dState_->currentWidgetId = widgetId;
        d2dState_->storagePrefix = WideToUtf8(widgetId);
    }
    // Create a sandbox table with only the registered safe API surface.
    PushSafeEnvironment(state, pending);
    lua_pushvalue(state, -1);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_environment");

    // Load the chunk
    if (const int status = luaL_loadstring(state, source.c_str()); status != LUA_OK)
    {
        if (status == LUA_ERRSYNTAX) markDeterministicFailure();
        const char* err = lua_tostring(state, -1);
        RuntimeRecordError(widgetId, err ? err : "(load error)");
        RecordWidgetHostFailure(widgetId, err ? err : "(load error)",
            quota->memoryExceeded || quota->executionExceeded);
        lua_pop(state, 2);
        return false;
    }

    // Try to set the chunk's first upvalue (_ENV) to sandbox.
    lua_pushvalue(state, -2);
    const char* envName = lua_setupvalue(state, -2, 1);
    if (envName == nullptr)
    {
        // Chunk has no _ENV upvalue - run directly in sandbox via alternative method
        // Pop the chunk, reload with explicit environment
        lua_pop(state, 2);  // pop sandbox copy and chunk, keep sandbox
        // Wrap the source to use the sandbox explicitly
        std::string wrapped = "local _ENV = ...;\n" + source;
        if (const int status = luaL_loadstring(state, wrapped.c_str()); status != LUA_OK)
        {
            if (status == LUA_ERRSYNTAX) markDeterministicFailure();
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(load error)");
            RecordWidgetHostFailure(widgetId,
                err ? err : "(load error)",
                quota->memoryExceeded || quota->executionExceeded);
            lua_pop(state, 2);
            return false;
        }
        lua_pushvalue(state, -2);  // push sandbox as argument
        if (const int status = snowdesktop::lua_runtime::ProtectedCall(
                state, 1, 1, 1000000,
                std::chrono::milliseconds(100)); status != LUA_OK)
        {
            if (status == LUA_ERRRUN && snowdesktop::widget_runtime::IsDryLoad() &&
                !quota->memoryExceeded && !quota->executionExceeded)
                markDeterministicFailure();
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(pcall error)");
            RecordWidgetHostFailure(widgetId,
                err ? err : "(pcall error)",
                quota->memoryExceeded || quota->executionExceeded);
            lua_pop(state, 2);
            return false;
        }
    }
    else
    {
        // Execute the chunk
        if (const int status = snowdesktop::lua_runtime::ProtectedCall(
                state, 0, 1, 1000000,
                std::chrono::milliseconds(100)); status != LUA_OK)
        {
            if (status == LUA_ERRRUN && snowdesktop::widget_runtime::IsDryLoad() &&
                !quota->memoryExceeded && !quota->executionExceeded)
                markDeterministicFailure();
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(pcall error)");
            RecordWidgetHostFailure(widgetId,
                err ? err : "(pcall error)",
                quota->memoryExceeded || quota->executionExceeded);
            lua_pop(state, 2);
            return false;
        }
    }

    lua_pushboolean(state, 0);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_loading");

    int ref = LUA_NOREF;
    if (!snowdesktop::widget_api::IsDefinedWidget(state, -1))
    {
        markDeterministicFailure();
        RuntimeRecordError(widgetId,
            "API v2 entry must return widget.define({...})");
        lua_pop(state, 2);
        return false;
    }
    ref = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_pop(state, 1); // sandbox
    (void)snowdesktop::widget_api::ConsumeTransientStateDirty(state);

    std::string name = pending.manifest.name.empty()
        ? "Unnamed" : pending.manifest.name;
    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
    lua_getfield(state, -1, "name");
    if (lua_isstring(state, -1))
        name = lua_tostring(state, -1);
    lua_pop(state, 1);

    // Read customStyle flag
    bool customStyle = false;
    lua_getfield(state, -1, "useCustomStyle");
    if (!lua_isnil(state, -1))
        customStyle = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    bool followPersonalizationDefault = false;
    lua_getfield(state, -1, "followPersonalizationDefault");
    if (!lua_isnil(state, -1))
        followPersonalizationDefault = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    lua_getfield(state, -1, "backgroundLayer");
    const bool hasBackgroundLayer = lua_istable(state, -1);
    lua_pop(state, 1);

    lua_getfield(state, -1, "showTitle");
    const bool showTitle = !lua_isnil(state, -1) &&
        lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_pushboolean(state, showTitle ? 1 : 0);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_show_title");

    lua_getfield(state, -1, "bottomBarHover");
    const bool bottomBarHover = lua_isnil(state, -1) ||
        lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_pushboolean(state, bottomBarHover ? 1 : 0);
    lua_setfield(state, LUA_REGISTRYINDEX,
        "__widget_bottom_bar_hover");

    std::vector<LuaWidgetManifest::Setting> scriptSettings;
    std::vector<LuaWidgetManifest::SettingGroup> scriptSettingGroups;
    std::vector<LuaWidgetManifest::SettingPreset> scriptPresets;
    ReadLuaDeclaredSettings(state, -1, scriptSettings,
        scriptSettingGroups, scriptPresets);
    std::string settingsError;
    if (!ValidateWidgetSettings(pending.manifest, scriptSettings,
            scriptSettingGroups, scriptPresets, settingsError))
    {
        RuntimeRecordError(widgetId, settingsError);
        RecordWidgetHostFailure(widgetId, settingsError, false);
        lua_pop(state, 1);
        return false;
    }

    if (!MigrateWidgetStorage(state, pending, *quota))
    {
        lua_pop(state, 1);
        return false;
    }

    lua_pop(state, 1);  // pop table

    LuaWidget w;
    w.widgetId = widgetId;
    w.runtimeToken = ++nextWidgetRuntimeToken_;
    if (w.runtimeToken == 0)
        w.runtimeToken = ++nextWidgetRuntimeToken_;
    w.packageId = pending.packageId;
    w.packageRoot = pending.packageRoot;
    w.state = stateGuard.release();
    lua_pushinteger(w.state,
        static_cast<lua_Integer>(w.runtimeToken));
    lua_setfield(w.state, LUA_REGISTRYINDEX,
        "__widget_runtime_token");
    w.quota = std::move(quota);
    w.storageWriteBudget = std::move(storageWriteBudget);
    w.name = name;
    w.filePath = path;
    w.manifest = pending.manifest;
    CapturePackageImageResources(w);
    w.permissions = pending.permissions;
    w.ref = ref;
    w.valid = true;
    w.customStyle = customStyle;
    w.followPersonalizationDefault = followPersonalizationDefault;
    w.hasBackgroundLayer = hasBackgroundLayer;
    if (preview && previewConfiguration)
    {
        w.theme = previewConfiguration->theme;
        w.surfaceContext = previewConfiguration->surface;
        w.lastBounds = previewConfiguration->bounds;
        w.lastColumns = std::max(1, previewConfiguration->columns);
        w.lastRows = std::max(1, previewConfiguration->rows);
        w.layoutMetrics = snowdesktop::widget_runtime::
            NormalizeLayoutMetrics(
                previewConfiguration->cellWidth,
                previewConfiguration->cellHeight,
                previewConfiguration->gap,
                previewConfiguration->barHeight,
                previewConfiguration->fontWeight,
                CalculateWidgetCellScale(
                    previewConfiguration->cellWidth,
                    previewConfiguration->cellHeight));
        w.previewDataState = previewConfiguration->dataState;
    }
    else if (d2dState_)
    {
        w.layoutMetrics = snowdesktop::widget_runtime::
            CaptureLayoutMetrics(*d2dState_);
    }
    w.scriptSettings = std::move(scriptSettings);
    w.scriptSettingGroups = std::move(scriptSettingGroups);
    w.scriptPresets = std::move(scriptPresets);
    w.preview = preview;
    w.previewStorage = std::move(pending.previewStorage);
    PurgeOpaqueWidgetSettings(w);
    w.logicalSlots = std::move(pending.logicalSlots);
    RestoreLogicalSlotReferences(w);
    (void)w.namedTimers.SetVisible(false,
        snowdesktop::widget_runtime::NamedTimerSchedule::Clock::now());
    (void)w.animationFrames.SetVisible(false);
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        w.lastModified = attr.ftLastWriteTime;
    widgets_.push_back(std::move(w));
    if (!InitializeWidgetLifecycle(widgets_.back()))
    {
        LuaWidget& failed = widgets_.back();
        ReleaseWidgetDataSubscriptions(failed);
        ReleaseWidgetTasks(failed,
            snowdesktop::widget_runtime::TaskBrokerCancelReason::InstanceDisposed);
        if (failed.state)
        {
            failed.lifecycle.Release(failed.state);
            luaL_unref(failed.state, LUA_REGISTRYINDEX, failed.ref);
            ReleasePackageImageResources(d2dState_, failed.state);
            lua_close(failed.state);
            failed.state = nullptr;
        }
        widgets_.pop_back();
        return false;
    }
    widgetHostFailures_.erase(widgetId);

    if (!preview && !snowdesktop::widget_runtime::IsDryLoad() && d2dState_)
    {
        // Only warm the filesystem cache. RenderWidget still checks the current
        // script synchronously, including deletion and failed-reload recovery.
        try
        {
            (void)d2dState_->scriptReadAhead.Track(widgetId, path,
                (widgets_.back().packageRoot / L"widget.json").wstring(),
                GetWidgetPackageManager().Paths().installed.wstring());
        }
        catch (...) { /* Optional path preparation must not fail the VM load. */ }
    }

    auto& loadedStorage = ActiveStorage();
    const std::string staleErrorKey =
        WideToUtf8(widgetId) + ".lastError";
    if (loadedStorage.erase(staleErrorKey) > 0 &&
        !snowdesktop::widget_runtime::HasStorageOverlay())
    {
        SaveStorageFile();
    }

    if (!preview && !snowdesktop::widget_runtime::IsDryLoad() &&
        widgets_.back().manifest.refreshIntervalMs > 0 &&
        widgetTimerRequestCallback_)
    {
        UINT_PTR tid = widgetTimerRequestCallback_(widgetId,
            static_cast<UINT>(widgets_.back().manifest.refreshIntervalMs));
        if (tid && !widgets_.empty())
        {
            auto& stored = widgets_.back();
            stored.refreshTimerId = tid;
        }
    }
    RestoreNotificationSchedules(widgets_.back());
    RefreshLogicalSlotAvailability();
    return true;
}

void WidgetEngine::TickRuntime()
{
    DrainPackageImageChanges();
    if (d2dState_ && d2dState_->shellIconLoader)
        d2dState_->shellIconLoader->Pump();
    const auto healthNow = snowdesktop::widget_runtime::
        RuntimeHealth::Clock::now();
    std::vector<std::wstring> recoveryTargets;
    for (auto& widget : widgets_)
    {
        const bool memoryQuotaExceeded = widget.quota &&
            widget.quota->memoryExceeded;
        if (widget.preview || widget.valid || memoryQuotaExceeded ||
            !widget.health.BeginRecovery(healthNow))
        {
            continue;
        }
        widget.valid = true;
        widgetHostFailures_.erase(widget.widgetId);
        recoveryTargets.push_back(widget.widgetId);
        RuntimeAddLog(widget.widgetId, "info",
            "Automatic runtime recovery probe started");
    }
    for (const auto& widgetId : recoveryTargets)
        RuntimeInvalidateHost(widgetId);

    ApplyWidgetTaskBrokerActions();
    TickWidgetNotifications();
    DrainWidgetProviderChanges();
    TickWidgetCalendar();
    DrainWidgetSnapshotChanges();
    DrainWidgetHttpResponses();

}

void WidgetEngine::ApplyWidgetHostVisibility(
    LuaWidget& widget, bool visible)
{
    if (!widget.valid || widget.hostVisible == visible)
        return;

    widget.hostVisible = visible;
    if (!visible && d2dState_)
    {
        d2dState_->backgroundCache.Erase(widget.widgetId);
        d2dState_->textLayoutCache.Erase(widget.widgetId);
    }
    const auto timerNow = widget.preview && (!previewOnly_ || !widgetTimerRequestCallback_)
        ? snowdesktop::widget_runtime::NamedTimerSchedule::TimePoint{
            std::chrono::milliseconds(snowdesktop::widget_runtime::
                PreviewMonotonicMilliseconds) }
        : snowdesktop::widget_runtime::NamedTimerSchedule::Clock::now();
    if (widget.namedTimers.SetVisible(visible, timerNow))
        RescheduleNamedTimer(widget);

    if (visible)
        (void)widget.animationFrames.SetVisible(true);
    else
        StopAnimationFrames(widget);

    if (dataBroker_)
    {
        (void)dataBroker_->SetInstanceVisible(
            WideToUtf8(widget.widgetId), visible,
            snowdesktop::widget_runtime::WidgetDataBroker::Clock::now());
        ApplyWidgetDataBrokerActions();
    }
    InvokeSimpleCallback(widget, visible ? "onVisible" : "onHidden");
    if (visible)
        (void)ScheduleAnimationFrame(widget);
}

void WidgetEngine::SetWidgetDesktopVisible(
    const std::wstring& widgetId, bool visible,
    bool keepRuntimeActive)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& widget = widgets_[index];
    if (!widget.valid || (widget.preview && (!previewOnly_ || !widgetTimerRequestCallback_)))
        return;
    widget.keepRuntimeActiveForHiddenPage =
        !visible && keepRuntimeActive;
    widget.desktopVisible = visible;
    if (!visible)
        widget.desktopOccluded = widget.desktopOcclusionResumePending = false;
    if (!visible && d2dState_)
        d2dState_->packageImageCache.ForgetOwner(WideToUtf8(widgetId) + "|desktop");
    if (!visible)
        ClearNativeMarqueeComposition(widget);
    ApplyWidgetHostVisibility(
        widget, widget.desktopVisible || widget.panelActive ||
            widget.keepRuntimeActiveForHiddenPage);
}

void WidgetEngine::SetAllWidgetDesktopVisible(bool visible)
{
    for (auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview)
            continue;
        widget.keepRuntimeActiveForHiddenPage = false;
        widget.desktopVisible = visible;
        if (!visible)
            widget.desktopOccluded = widget.desktopOcclusionResumePending = false;
        if (!visible && d2dState_)
            d2dState_->packageImageCache.ForgetOwner(
                WideToUtf8(widget.widgetId) + "|desktop");
        if (!visible)
            ClearNativeMarqueeComposition(widget);
        ApplyWidgetHostVisibility(
            widget, widget.desktopVisible || widget.panelActive);
    }
}

void WidgetEngine::SetWidgetDesktopOccluded(const std::wstring& widgetId, bool occluded)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& widget = widgets_[index];
    if (widget.desktopOccluded == occluded) return;
    widget.desktopOccluded = occluded;
    widget.desktopOcclusionResumePending = !occluded && widget.desktopVisible;
    if (!occluded) return;
    PauseNativeMarqueeClock(widget.desktopMarquee);
    widget.viewTransitions.Clear();
    widget.viewTransitionFramePending = false;
    if (widget.panelActive) return;
    if (widget.animationTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.animationTimerId);
    widget.animationTimerId = 0;
    (void)widget.animationFrames.ConsumeDataRefresh();
    widget.animationFrames.ResetFrameTiming();
}

bool WidgetEngine::ReloadWidget(const std::wstring& widgetId,
    bool* deterministicFailure)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    WidgetExecutionContextGuard reloadContext(d2dState_, widgetId);
    const std::string packageId = widgets_[idx].packageId;
    if (!snowdesktop::widget_runtime::HasStorageOverlay())
    {
        auto& manager = GetWidgetPackageManager();
        bool permissionScopeChanged = false;
        std::string permissionRefreshError;
        if (!manager.RefreshChangedPermissionScope(packageId,
                permissionScopeChanged, permissionRefreshError))
        {
            RuntimeRecordError(widgetId,
                "Widget permission scope refresh failed: " +
                    permissionRefreshError);
            return false;
        }
        if (permissionScopeChanged)
        {
            const auto package = manager.Resolve(packageId);
            if (!package)
            {
                UnloadWidget(widgetId);
                return false;
            }
            const auto grant =
                snowdesktop::widget::WidgetPermissionBroker::Evaluate(
                    package->permissionState,
                    package->manifest.permissions,
                    package->manifest.optionalPermissions,
                    package->manifest.networkDomains,
                    package->grantedPermissions,
                    package->grantedNetworkDomains);
            if (grant.runtimeBlock !=
                snowdesktop::widget::PermissionRuntimeBlock::None)
            {
                // Retire the VM that still owns the previous grant. The host
                // placeholder now exposes the normal reauthorization action.
                UnloadWidget(widgetId);
                return false;
            }
        }
    }
    const auto layoutMetrics = widgets_[idx].layoutMetrics;
    std::wstring path = ResolveWidgetPath(
        Utf8ToWide(packageId));
    if (path.empty())
        path = widgets_[idx].filePath;
    const size_t oldIndex = static_cast<size_t>(idx);
    if (!LoadWidget(path, widgetId, false, nullptr, nullptr, nullptr, deterministicFailure))
    {
        // The enclosing package verification owns rollback for all instances.
        if (!snowdesktop::widget_runtime::HasStorageOverlay())
            RecoverWidgetPackage(packageId, widgets_[idx].manifest.version);
        return false;
    }

    // Loading appends the replacement VM. Preserve the host metrics that were
    // already calculated for this instance before retiring the old VM.
    widgets_.back().layoutMetrics = layoutMetrics;

    // The new VM is now fully loaded. Only then retire the last-known-good VM.
    LuaWidget& old = widgets_[oldIndex];
    ClearNativeMarqueeComposition(old);
    if (old.hostVisible)
        InvokeSimpleCallback(old, "onHidden");
    DisposeWidgetLifecycle(old, "hotReload");
    ReleaseWidgetDataSubscriptions(old);
    ReleaseWidgetTasks(old,
        snowdesktop::widget_runtime::TaskBrokerCancelReason::InstanceDisposed);
    ClearRuntimeImagesForWidget(d2dState_, widgetId);
    if (old.refreshTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(old.refreshTimerId);
    if (old.namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(old.namedTimerId);
    StopAnimationFrames(old);
    if (httpService_) httpService_->CancelWidget(widgetId);
    if (old.state)
    {
        old.lifecycle.Release(old.state);
        luaL_unref(old.state, LUA_REGISTRYINDEX, old.ref);
        ReleasePackageImageResources(d2dState_, old.state);
        lua_close(old.state);
        old.state = nullptr;
    }
    widgets_.erase(widgets_.begin() + static_cast<std::ptrdiff_t>(oldIndex));
    return true;
}

bool WidgetEngine::RetryWidget(const std::wstring& widgetId,
    const std::wstring& packageId)
{
    widgetHostFailures_.erase(widgetId);
    if (FindWidget(widgetId) >= 0)
        return ReloadWidget(widgetId);
    return EnsureWidgetLoaded(widgetId, packageId);
}

snowdesktop::widget_runtime::WidgetHostState
WidgetEngine::GetWidgetHostState(const std::wstring& widgetId,
    const std::wstring& packageId) const
{
    using snowdesktop::widget_runtime::WidgetHostStateKind;

    if (!IsWidgetPackageInstalled(packageId))
        return { WidgetHostStateKind::PackageMissing, {} };
    // This synchronous check performs no catalog mutation or callbacks. Keep
    // permission evaluation fresh without copying the manifest and locales
    // on every animated frame. The borrow ends before leaving this call.
    const auto* package = GetWidgetPackageManager().FindResolved(WideToUtf8(packageId));
    if (!package)
        return { WidgetHostStateKind::LoadFailed, {} };
    const auto grant = snowdesktop::widget::WidgetPermissionBroker::Evaluate(
        package->permissionState,
        package->manifest.permissions,
        package->manifest.optionalPermissions,
        package->manifest.networkDomains,
        package->grantedPermissions,
        package->grantedNetworkDomains);
    switch (grant.runtimeBlock)
    {
    case snowdesktop::widget::PermissionRuntimeBlock::PendingConsent:
        return { WidgetHostStateKind::PermissionPending, {} };
    case snowdesktop::widget::PermissionRuntimeBlock::Denied:
    case snowdesktop::widget::PermissionRuntimeBlock::MissingRequired:
        return { WidgetHostStateKind::PermissionDenied, {} };
    case snowdesktop::widget::PermissionRuntimeBlock::None:
        break;
    }

    const int index = FindWidget(widgetId);
    if (index >= 0)
    {
        const LuaWidget& widget = widgets_[index];
        if (widget.valid)
            return { WidgetHostStateKind::Ready, {} };
        const bool quotaExceeded = widget.quota &&
            (widget.quota->memoryExceeded ||
                widget.quota->executionExceeded);
        if (quotaExceeded)
            return { WidgetHostStateKind::QuotaExceeded, {} };
        if (widget.health.CircuitOpen())
            return { WidgetHostStateKind::RuntimeSuspended, {} };
    }

    if (const auto failure = widgetHostFailures_.find(widgetId);
        failure != widgetHostFailures_.end())
        return failure->second;
    return { WidgetHostStateKind::LoadFailed, {} };
}

std::optional<snowdesktop::widget_runtime::WidgetHostState>
WidgetEngine::GetWidgetRuntimeFailure(
    const std::wstring& widgetId) const
{
    if (const auto failure = widgetHostFailures_.find(widgetId);
        failure != widgetHostFailures_.end())
        return failure->second;
    return std::nullopt;
}

void WidgetEngine::NotifyLanguageChanged(const std::wstring& widgetId)
{
    int index = FindWidget(widgetId);
    if (index < 0)
        return;
    InvokeSimpleCallback(widgets_[index], "onLanguageChanged");
    RuntimeInvalidateHost(widgetId);
}

bool WidgetEngine::RuntimeHasPermission(const std::wstring& widgetId, const char* permission) const
{
    if (!permission || !*permission) return true;
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    const auto& perms = widgets_[idx].permissions;
    return snowdesktop::widget::WidgetPermissionBroker::
        AllowsPermission(perms, permission);
}

void WidgetEngine::RuntimeSetWidgetTitle(const std::wstring& widgetId, const std::wstring& title)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    if (setWidgetTitleCallback_)
        setWidgetTitleCallback_(widgetId, title);
}

void WidgetEngine::RuntimeInvalidateHost(const std::wstring& widgetId,
    std::optional<RECT> dirtyRect, std::string_view surface)
{
    snowdesktop::performance::Scope performanceScope(
        "widget.invalidate", surface.empty() ? "current" : surface, widgetId);
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    // Calls made while evaluating or interacting with an auxiliary surface
    // belong to that surface unless the caller explicitly names another one.
    // Keeping an empty surface here used to invalidate the panel and the
    // desktop copy together, which could attempt a second DComp BeginDraw
    // while the shared popup surface was still being rendered.
    if (surface.empty())
    {
        const std::string_view currentSurface =
            CurrentWidgetSurface(d2dState_);
        if (IsPanelSurface(currentSurface))
            surface = currentSurface;
    }
    snowdesktop::performance::DrawLink(
        surface.empty() ? "desktop" : surface, widgetId, false);
    const int index = widgetId.empty() ? -1 : FindWidget(widgetId);
    if (index >= 0)
    {
        auto& widget = widgets_[index];
        if (surface.empty() || surface == "desktop")
            widget.desktopMarquee.requiresLuaRender = true;
        if (surface.empty() || IsPanelSurface(surface))
            widget.panelMarquee.requiresLuaRender = true;
    }
    invalidationBatch_.Invalidate(
        invalidateCallback_, widgetId, dirtyRect, surface);
}

bool WidgetEngine::RuntimeNotifySettingsChanged(
    const std::wstring& widgetId, std::vector<std::string> keys,
    bool preview)
{
    const int index = FindWidget(widgetId);
    if (index < 0 ||
        !WidgetDeclaresFeature(widgets_[index], "settings.changeEvent"))
        return false;
    keys.erase(std::remove_if(keys.begin(), keys.end(),
        [](const std::string& key) { return key.empty(); }), keys.end());
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return InvokeLifecycleEvent(widgets_[index], "settings.changed",
        [keys = std::move(keys), preview](lua_State* eventState) {
            lua_createtable(eventState, static_cast<int>(keys.size()), 0);
            for (std::size_t index = 0; index < keys.size(); ++index)
            {
                lua_pushlstring(eventState,
                    keys[index].data(), keys[index].size());
                lua_rawseti(eventState, -2,
                    static_cast<lua_Integer>(index + 1));
            }
            lua_setfield(eventState, -2, "keys");
            lua_pushboolean(eventState, preview ? 1 : 0);
            lua_setfield(eventState, -2, "preview");
        });
}

LuaWidgetManifest WidgetEngine::GetWidgetManifest(const std::wstring& filename)
{
    const bool byPackageId = PathIsRelativeW(filename.c_str()) != FALSE;
    const auto path = byPackageId ? ResolveWidgetPath(filename) : filename;
    if (path.empty()) return {};
    if (byPackageId && !snowdesktop::widget_runtime::IsDryLoad())
    {
        // ResolveWidgetPath has already validated this package's entry. A
        // layout query by ID need not resolve every other installed entry to
        // rediscover its root. Preserve legacy lookup for missing entries.
        const auto* package = GetWidgetPackageManager().FindResolved(WideToUtf8(filename));
        std::error_code error;
        if (package && package->active && std::filesystem::exists(path, error))
            return snowdesktop::widget_runtime::ReadWidgetScriptManifest(path, package->root);
    }
    const auto package = ResolveWidgetEntryPackage(path);
    return snowdesktop::widget_runtime::ReadWidgetScriptManifest(path,
        package ? package->root : std::filesystem::path{});
}

bool WidgetEngine::SetWidgetPermissionDecision(
    const std::wstring& packageId,
    snowdesktop::widget::PermissionDecisionState state,
    const std::vector<std::string>& grantedPermissions,
    const std::vector<std::string>& grantedNetworkDomains,
    std::string& error, bool nativeAcknowledged, std::string_view reviewedNativeIdentity)
{
    const std::string requestedId = WideToUtf8(packageId);
    if (requestedId.empty())
    {
        error = "package id is empty";
        return false;
    }
    return GetWidgetPackageManager().SetPermissionDecision(
        requestedId, state, grantedPermissions,
        grantedNetworkDomains, error, nativeAcknowledged, reviewedNativeIdentity);
}

bool WidgetEngine::ApplyWidgetPermissionDecision(
    const std::wstring& packageId,
    snowdesktop::widget::PermissionDecisionState state,
    const std::vector<std::string>& grantedPermissions,
    const std::vector<std::string>& grantedNetworkDomains,
    std::string& error, bool nativeAcknowledged, std::string_view reviewedNativeIdentity)
{
    if (!SetWidgetPermissionDecision(packageId, state,
            grantedPermissions, grantedNetworkDomains, error, nativeAcknowledged, reviewedNativeIdentity))
        return false;
    const std::string requestedId = WideToUtf8(packageId);
    const std::unordered_set<std::string> effectivePermissions =
        state == snowdesktop::widget::PermissionDecisionState::Granted
        ? std::unordered_set<std::string>(
            grantedPermissions.begin(), grantedPermissions.end())
        : std::unordered_set<std::string>{};
    std::vector<std::wstring> instances;
    for (const auto& widget : widgets_)
    {
        if (widget.packageId == requestedId)
        {
            instances.push_back(widget.widgetId);
            if (taskBroker_)
            {
                const std::string instanceId =
                    WideToUtf8(widget.widgetId);
                for (const std::string& permission : widget.permissions)
                {
                    if (!effectivePermissions.contains(permission))
                    {
                        (void)taskBroker_->SetPermission(
                            instanceId, permission, false);
                    }
                }
            }
        }
    }
    for (const auto& instance : instances)
        UnloadWidget(instance);
    if (state == snowdesktop::widget::PermissionDecisionState::Granted)
    {
        for (const auto& instance : instances)
            (void)EnsureWidgetLoaded(instance, packageId);
    }
    return true;
}
