#include "widget/runtime/widget_engine.h"

#include "common/utils.h"
#include "common/l10n.h"
#include "data/atomic_file.h"
#include "data/data_paths.h"
#include "diagnostics/performance_trace.h"
#include "layout/logical_slot_picker_rules.h"
#include "navigation/name_pinyin.h"
#include "navigation/search_match.h"
#include "widget/api/widget_api_registry.h"
#include "widget/data/widget_system_control_data.h"
#include "widget/data/widget_resource_lua.h"
#include "widget/packages/widget_package_paths.h"
#include "widget/permissions/widget_permission_broker.h"
#include "widget/preview/widget_preview_context.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget/runtime/widget_runtime_image.h"
#include "widget/settings/widget_system_settings.h"
#include "widget/tasks/widget_audio_output_task_executor.h"
#include "widget/tasks/widget_audio_playback.h"
#include "widget/tasks/widget_audio_task_dispatch.h"
#include "widget/tasks/widget_filesystem_task_dispatch.h"
#include "widget/tasks/widget_location_task_executor.h"
#include "widget/tasks/widget_media_task_executor.h"
#include "widget/tasks/widget_native_task_executor.h"
#include "widget/tasks/widget_task_start_lua.h"
#include "widget/view/widget_render_host_context.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <iterator>
#include <span>
#include <utility>

using snowdesktop::widget::ResolvePackageAssetWithinRoot;
using snowdesktop::widget_runtime::PushSystemControlJson;
using snowdesktop::widget_runtime::IsFilesystemHandleTask;
using snowdesktop::widget_runtime::IsFilesystemPickerTask;
using snowdesktop::widget_runtime::IsPreviewFilesystemHandle;
using snowdesktop::widget_runtime::kFilesystemReadPermission;
using snowdesktop::widget_runtime::kFilesystemWritePermission;

namespace
{
constexpr char kNotificationPostPermission[] = "notification.post";

static const char* ResolveClipboardWriteFile(const LuaWidget& owner,
    const snowdesktop::widget_runtime::TaskBrokerAction& action,
    const snowdesktop::widget_runtime::WidgetFilesystemHandleStore* handleStore,
    const std::string& reference, std::filesystem::path& path)
{
    using snowdesktop::widget::WidgetPermissionBroker;
    using snowdesktop::widget_runtime::WidgetFilesystemHandleAccess;
    if (reference.starts_with("item:"))
    {
        if (!WidgetPermissionBroker::AllowsPermission(owner.permissions, "clipboard.read"))
            return "permissionDenied";
        const auto found = owner.itemReferences.find(reference);
        if (found == owner.itemReferences.end() || found->second.sourceTask != "clipboard.read")
            return "invalidReference";
        path = Utf8ToWide(found->second.target);
        return nullptr;
    }
    if (!WidgetPermissionBroker::AllowsPermission(owner.permissions, kFilesystemReadPermission))
        return "permissionDenied";
    if (action.preview && IsPreviewFilesystemHandle(reference))
    {
        path = L"C:\\Preview\\Preview.txt";
        return nullptr;
    }
    const auto entry = handleStore ? handleStore->Resolve(
        { action.instanceId, owner.packageId }, reference) : std::nullopt;
    if (!entry) return "invalidReference";
    if (entry->access == WidgetFilesystemHandleAccess::Write)
        return "handleAccessDenied";
    path = entry->path;
    return nullptr;
}

}

void WidgetEngine::BeginTaskShutdown()
{
    StopWidgetMultiplayer("canceled");
    audioPlayback_.reset();
    if (taskBroker_) taskBroker_->Shutdown();
    nativeTaskExecutor_.reset();
    systemControlTasks_.reset();
}

void WidgetEngine::InitializeWidgetTaskBroker()
{
    audioPlayback_.reset();
    systemControlTasks_.reset();
    using snowdesktop::widget_runtime::TaskDescriptor;
    taskBroker_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetTaskBroker>();
    notificationCenter_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetNotificationCenter>();
    std::string error;
    for (const auto& contract :
        snowdesktop::widget_api::SystemTaskContracts())
    {
        (void)taskBroker_->RegisterTask(TaskDescriptor{
            contract.name,
            contract.requiredPermission ? contract.requiredPermission : "",
            contract.requiresTrustedGesture,
            contract.maximumPerInstance }, error);
        error.clear();
    }
    if (previewOnly_)
    {
        nativeTaskExecutor_.reset();
        locationTaskExecutor_.reset();
        mediaTaskExecutor_.reset();
        audioOutputTaskExecutor_.reset();
        clipboardTaskExecutor_.reset();
        filesystemTaskExecutor_.reset();
        appTaskExecutor_.reset();
        desktopTaskExecutor_.reset();
        externalItemTaskExecutor_.reset();
    }
    else
    {
        audioPlayback_ = std::make_unique<snowdesktop::widget_runtime::WidgetAudioPlayback>();
        audioPlayback_->SetWakeCallback(taskWakeCallback_);
        nativeTaskExecutor_ = std::make_unique<snowdesktop::widget_runtime::WidgetNativeTaskExecutor>();
        nativeTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
        locationTaskExecutor_ = std::make_unique<snowdesktop::widget_runtime::WidgetLocationTaskExecutor>();
        locationTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
        mediaTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetMediaTaskExecutor>();
        audioOutputTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAudioOutputTaskExecutor>();
        clipboardTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetClipboardTaskExecutor>();
        filesystemTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor>();
        filesystemTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
        appTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAppTaskExecutor>();
        desktopTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAppTaskExecutor>();
        externalItemTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetExternalSearchTaskExecutor>();
    }
}

void WidgetEngine::LoadNotificationSchedules()
{
    notificationScheduleStore_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetNotificationScheduleStore>();
    notificationSchedulePath_ = GetDataFilePath(
        L"SnowDesktop.widget-notifications.json");
    const auto rejectLoad = [this](const std::string& error) {
        notificationScheduleStore_->MarkLoadFailed();
        OutputDebugStringA(("SnowDesktop: notification schedule load "
            "failed: " + error + "\n").c_str());
    };
    const DWORD attributes = GetFileAttributesW(notificationSchedulePath_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            rejectLoad("cannot inspect file: " + std::to_string(error));
        return;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        rejectLoad("file is a reparse point");
        return;
    }
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(
            notificationSchedulePath_, fileError))
    {
        rejectLoad(fileError ? fileError.message() : "not a regular file");
        return;
    }
    const auto fileBytes = std::filesystem::file_size(
        notificationSchedulePath_, fileError);
    if (fileError || fileBytes > snowdesktop::widget_runtime::
            WidgetNotificationScheduleStore::MaximumFileBytes)
    {
        rejectLoad(fileError ? fileError.message() : "file exceeds size limit");
        return;
    }
    std::string text;
    std::string error;
    if (!snowdesktop::atomic_file::ReadAll(
            notificationSchedulePath_, text, &error) ||
        !notificationScheduleStore_->LoadText(text, error))
    {
        rejectLoad(error);
        return;
    }
    const std::int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            snowdesktop::widget_runtime::WidgetNotificationCenter::
                Clock::now().time_since_epoch()).count();
    constexpr std::int64_t catchUpWindowMs =
        24LL * 60 * 60 * 1000;
    bool pruned = false;
    const auto loadedEntries = notificationScheduleStore_->Entries();
    for (const auto& entry : loadedEntries)
    {
        if (entry.dueMs < nowMs - catchUpWindowMs)
        {
            pruned = notificationScheduleStore_->Remove(
                entry.instanceId, entry.notificationId) || pruned;
        }
    }
    if (pruned) (void)SaveNotificationSchedules();
}

bool WidgetEngine::SaveNotificationSchedules()
{
    if (!notificationScheduleStore_ ||
        !notificationScheduleStore_->IsWritable() ||
        notificationSchedulePath_.empty())
        return false;
    std::string error;
    const bool saved = snowdesktop::atomic_file::WriteAll(
        notificationSchedulePath_,
        notificationScheduleStore_->Serialize(),
        notificationSchedulePath_.wstring() + L".bak", &error);
    if (!saved)
    {
        OutputDebugStringA(("SnowDesktop: notification schedule save "
            "failed: " + error + "\n").c_str());
    }
    return saved;
}

void WidgetEngine::RestoreNotificationSchedules(LuaWidget& widget)
{
    if (!notificationCenter_ || !notificationScheduleStore_ ||
        !notificationScheduleStore_->IsWritable() || widget.preview ||
        snowdesktop::widget_runtime::IsDryLoad())
        return;
    const std::string instanceId = WideToUtf8(widget.widgetId);
    if (!snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(
            widget.permissions, kNotificationPostPermission))
    {
        if (notificationScheduleStore_->RemoveInstance(instanceId) > 0)
            (void)SaveNotificationSchedules();
        return;
    }
    const bool ownerMismatch = std::any_of(
        notificationScheduleStore_->Entries().begin(),
        notificationScheduleStore_->Entries().end(),
        [&instanceId, &widget](const auto& entry) {
            return entry.instanceId == instanceId &&
                entry.packageId != widget.packageId;
        });
    if (ownerMismatch)
    {
        if (notificationScheduleStore_->RemoveInstance(instanceId) > 0)
            (void)SaveNotificationSchedules();
        return;
    }

    bool changed = false;
    const auto now = snowdesktop::widget_runtime::
        WidgetNotificationCenter::Clock::now();
    for (const auto& entry : notificationScheduleStore_->ForInstance(
            instanceId, widget.packageId))
    {
        snowdesktop::widget_runtime::WidgetNotificationContent content;
        content.title = Utf8ToWide(entry.title);
        content.message = Utf8ToWide(entry.message);
        if (!entry.imageResource.empty())
        {
            const auto imagePath = RuntimeResolvePackageResource(
                widget.widgetId, entry.imageResource, "image");
            if (!imagePath)
            {
                changed = notificationScheduleStore_->Remove(
                    instanceId, entry.notificationId) || changed;
                continue;
            }
            content.imagePath = *imagePath;
        }
        content.progress = entry.progress;
        for (const auto& action : entry.actions)
        {
            content.actions.push_back(
                { action.id, Utf8ToWide(action.label) });
        }
        const auto due = snowdesktop::widget_runtime::
            WidgetNotificationCenter::Clock::time_point(
                std::chrono::milliseconds(entry.dueMs));
        const auto restored = notificationCenter_->RestoreScheduled(
            widget.runtimeToken, entry.notificationId,
            std::move(content), due, now);
        if (!restored.ok)
        {
            changed = notificationScheduleStore_->Remove(
                instanceId, entry.notificationId) || changed;
        }
    }
    if (changed) (void)SaveNotificationSchedules();
}

void WidgetEngine::RemoveNotificationSchedules(
    const std::wstring& widgetId)
{
    if (!notificationScheduleStore_) return;
    if (notificationScheduleStore_->RemoveInstance(
            WideToUtf8(widgetId)) > 0)
        (void)SaveNotificationSchedules();
}

void WidgetEngine::ApplyWidgetTaskBrokerActions()
{
    using snowdesktop::widget_runtime::TaskBrokerActionType;
    if (!taskBroker_) return;
    // File pickers can run a nested Windows message loop. Keep wake messages
    // from reentering the broker while its dispatch/completion data is in use.
    if (applyingTaskBrokerActions_)
    {
        taskWakePending_ = true;
        return;
    }
    applyingTaskBrokerActions_ = true;
    struct DispatchScope
    {
        bool& active;
        bool& pending;
        TaskWakeCallback& wake;
        ~DispatchScope()
        {
            active = false;
            if (std::exchange(pending, false) && wake) wake();
        }
    } dispatchScope{ applyingTaskBrokerActions_, taskWakePending_, taskWakeCallback_ };
    PumpWidgetMultiplayer();
    const auto audioEvents =
        snowdesktop::widget_runtime::ConsumeAudioPlaybackMessages(
            audioPlayback_.get(), *taskBroker_);
    if (nativeTaskExecutor_)
        nativeTaskDispatch_.Consume(nativeTaskExecutor_->DrainMessages(), *taskBroker_,
            [this](const snowdesktop::widget_runtime::NativeTaskEvent& event) {
                auto owner = std::find_if(widgets_.begin(), widgets_.end(),
                    [&](const auto& widget) { return widget.runtimeToken == event.ownerToken; });
                if (owner == widgets_.end() ||
                    !owner->permissions.contains("native.execute")) return;
                JsonValue frame;
                const auto* value = event.ReadValue(frame);
                if (!value) return;
                snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
                    trustedGestureState_, false);
                (void)InvokeLifecycleEvent(*owner, "native.event", [&](lua_State* state) {
                    lua_pushinteger(state, static_cast<lua_Integer>(event.taskId));
                    lua_setfield(state, -2, "taskId");
                    PushSystemControlJson(state, *value);
                    lua_setfield(state, -2, "value");
                });
            });
    if (locationTaskExecutor_)
        for (auto& completion : locationTaskExecutor_->DrainCompletions())
        {
            const auto snapshot = taskBroker_->Snapshot(completion.id);
            if (!snapshot || snapshot->cancelRequested) continue;
            const auto id = completion.id;
            const auto ok = completion.ok;
            const auto error = completion.error;
            if (ok) taskResults_.StoreLocation(id, std::move(completion));
            (void)taskBroker_->Complete(id, ok, error);
        }
    if (systemControlTasks_)
        for (auto& completion : systemControlTasks_->Drain())
            (void)taskBroker_->Complete(completion.id, completion.ok, std::move(completion.error));
    if (mediaTaskExecutor_)
    {
        for (auto& completion : mediaTaskExecutor_->DrainCompletions())
        {
            (void)taskBroker_->Complete(completion.id,
                completion.accepted, std::move(completion.error));
        }
    }
    if (audioOutputTaskExecutor_)
    {
        for (auto& completion :
            audioOutputTaskExecutor_->DrainCompletions())
        {
            (void)taskBroker_->Complete(completion.id,
                completion.accepted, std::move(completion.error));
        }
    }
    if (clipboardTaskExecutor_)
        clipboardTaskDispatch_.Consume(clipboardTaskExecutor_->DrainCompletions(), *taskBroker_);
    if (filesystemTaskExecutor_)
        filesystemTaskDispatch_.Consume(filesystemTaskExecutor_->DrainCompletions(),
            *taskBroker_, filesystemHandleStore_.get(),
            [this](std::uint64_t ownerToken)
                -> std::optional<snowdesktop::widget_runtime::FilesystemCompletionOwner> {
                const auto owner = std::find_if(widgets_.begin(), widgets_.end(),
                    [ownerToken](const LuaWidget& widget) {
                        return widget.runtimeToken == ownerToken;
                    });
                if (owner == widgets_.end()) return std::nullopt;
                return snowdesktop::widget_runtime::FilesystemCompletionOwner{
                    owner->packageId,
                    snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(
                        owner->permissions, kFilesystemReadPermission)};
            });
    if (appTaskExecutor_)
    {
        for (auto& completion : appTaskExecutor_->DrainCompletions())
        {
            const std::uint64_t id = completion.id;
            const bool ok = completion.ok;
            std::string error = completion.error;
            if (ok)
                taskResults_.StoreAppSearch(
                    id, std::move(completion));
            else
                taskResults_.DiscardAppSearch(id);
            (void)taskBroker_->Complete(
                id, ok, std::move(error));
        }
    }
    if (desktopTaskExecutor_)
    {
        for (auto& completion : desktopTaskExecutor_->DrainCompletions())
        {
            const std::uint64_t id = completion.id;
            const bool ok = completion.ok;
            std::string error = completion.error;
            if (ok)
                taskResults_.StoreItemSearch(
                    id, std::move(completion));
            else
                taskResults_.DiscardItemSearch(id);
            (void)taskBroker_->Complete(id, ok, std::move(error));
        }
    }
    if (externalItemTaskExecutor_)
    {
        for (auto& completion :
            externalItemTaskExecutor_->DrainCompletions())
        {
            const std::uint64_t id = completion.id;
            const bool ok = completion.ok;
            std::string error = completion.error;
            if (ok)
                taskResults_.StoreItemSearch(
                    id, std::move(completion));
            else
                taskResults_.DiscardItemSearch(id);
            (void)taskBroker_->Complete(id, ok, std::move(error));
        }
    }
    for (const auto& action : taskBroker_->DrainActions())
    {
        snowdesktop::performance::Scope performanceScope(
            "task.dispatch", action.name,
            snowdesktop::performance::Enabled()
                ? Utf8ToWide(action.instanceId) : std::wstring{}, action.id);
        if (action.type == TaskBrokerActionType::Cancel)
        {
            if (action.name.starts_with("multiplayer."))
            {
                CancelWidgetMultiplayerSend(action.id);
                (void)taskBroker_->Complete(action.id, false);
                continue;
            }
            snowdesktop::widget_runtime::CancelAudioPlaybackTask(
                audioPlayback_.get(), action.name, action.id);
            CancelDispatchedWidgetTask(action.id, action.name == "location.current");
            networkTaskDispatch_.Cancel(action, httpService_.get());
            continue;
        }
        const auto snapshot = taskBroker_->Snapshot(action.id);
        if (!snapshot) continue;
        if (snapshot->cancelRequested)
        {
            (void)taskBroker_->Complete(action.id, false);
            continue;
        }

        auto owner = std::find_if(widgets_.begin(), widgets_.end(),
            [&action](const LuaWidget& candidate) {
                return candidate.runtimeToken == action.ownerToken;
            });
        if (owner == widgets_.end())
        {
            (void)taskBroker_->Complete(
                action.id, false, "instanceDisposed");
            continue;
        }

        if (action.name.starts_with("multiplayer."))
        {
            StartWidgetMultiplayerTask(action, *owner);
            continue;
        }
        if (snowdesktop::widget_runtime::WidgetAudioPlayback::SupportsAction(action.name))
        {
            using namespace snowdesktop::widget_runtime;
            AudioPlaybackRequest request;
            request.taskId = action.id; request.owner = action.ownerToken; request.action = action.name;
            const auto get = [&](const char* key) -> std::string {
                const auto value = action.arguments.find(key); return value == action.arguments.end() ? std::string{} : value->second;
            };
            request.playbackId = get("playbackId");
            if (const auto value = get("volume"); !value.empty())
                (void)std::from_chars(value.data(), value.data() + value.size(), request.volume);
            if (const auto value = get("positionMs"); !value.empty())
                (void)std::from_chars(value.data(), value.data() + value.size(), request.positionMs);
            request.loop = get("loop") == "1";
            request.readMetadata = get("readMetadata") == "1";
            request.systemMediaControls = get("systemMediaControls") == "1";
            request.shuffle = get("shuffle") == "1";
            if (const auto value = get("repeatMode"); !value.empty()) request.repeatMode = value;
            if (const auto value = get("playbackRate"); !value.empty())
                (void)std::from_chars(value.data(), value.data() + value.size(), request.playbackRate);
            std::string error;
            if (!owner->permissions.contains("audio.playback")) error = "permissionRevoked";
            else if (action.name == "audio.play")
            {
                if (const auto resource = get("resource"); !resource.empty())
                {
                    const auto found = owner->manifest.resources.find(resource);
                    if (found == owner->manifest.resources.end() || found->second.type != "audio") error = "invalidReference";
                    else if (const auto path = ResolvePackageAssetWithinRoot(owner->packageRoot, Utf8ToWide(found->second.path)))
                        request.path = *path;
                    else error = "invalidReference";
                }
                else
                {
                    request.sourceHandle = get("handle");
                    const auto entry = filesystemHandleStore_ ? filesystemHandleStore_->Resolve(
                        {WideToUtf8(owner->widgetId), owner->packageId}, request.sourceHandle) : std::nullopt;
                    const auto name = get("name");
                    if (!owner->permissions.contains("filesystem.userSelected.read")) error = "permissionRevoked";
                    else if (!entry || entry->access == WidgetFilesystemHandleAccess::Write) error = "invalidReference";
                    else if (name.empty() ? entry->kind != WidgetFilesystemHandleKind::File :
                        entry->kind != WidgetFilesystemHandleKind::Folder || !WidgetFilesystemTaskExecutor::IsDirectChildName(name)) error = "invalidArguments";
                    else request.path = name.empty() ? entry->path : entry->path / Utf8ToWide(name);
                }
            }
            if (action.preview) { (void)taskBroker_->Complete(action.id, true); continue; }
            if (!error.empty()) { (void)taskBroker_->Complete(action.id, false, error); continue; }
            if (!audioPlayback_ || !audioPlayback_->Submit(std::move(request)))
                (void)taskBroker_->Complete(action.id, false, "audioPlaybackUnavailable");
            continue;
        }
        if (action.name == "native.request" || action.name == "native.watch")
        {
            snowdesktop::widget_runtime::NativeRequestOptions options;
            const auto package = GetWidgetPackage(Utf8ToWide(owner->packageId));
            if (!snowdesktop::widget_runtime::ParseNativeRequestOptions(action.arguments, options))
                (void)taskBroker_->Complete(action.id, false, "invalidArguments");
            else if (action.preview)
                (void)taskBroker_->Complete(action.id, false, "previewUnavailable");
            else if (!package || package->permissionState != snowdesktop::widget::PermissionDecisionState::Granted ||
                !owner->permissions.contains("native.execute") ||
                !snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(package->grantedPermissions, "native.execute"))
                (void)taskBroker_->Complete(action.id, false, "permissionRevoked");
            else
            {
                options.streaming = action.name == "native.watch";
                if (!nativeTaskExecutor_ || !nativeTaskExecutor_->Start(action.id, package->root,
                        snowdesktop::widget::PackagePermissionScopeFingerprint(package->manifest), std::move(options)))
                    (void)taskBroker_->Complete(action.id, false, "taskExecutorUnavailable");
            }
            continue;
        }
        if (action.name == "location.current")
        {
            snowdesktop::widget_runtime::LocationOptions options;
            if (!snowdesktop::widget_runtime::ParseLocationOptions(action.arguments, options))
                (void)taskBroker_->Complete(action.id, false, "invalidArguments");
            else if (action.preview)
                (void)taskBroker_->Complete(action.id, false, "previewUnavailable");
            else if (!snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(owner->permissions, "location.read"))
                (void)taskBroker_->Complete(action.id, false, "permissionRevoked");
            else if (!locationTaskExecutor_ || !locationTaskExecutor_->Start(action.id, options, false))
                (void)taskBroker_->Complete(action.id, false, "taskExecutorUnavailable");
            continue;
        }
        if (snowdesktop::widget_runtime::IsSystemControlTask(action.name))
        {
            snowdesktop::system_control::Request request;
            if (!snowdesktop::widget_runtime::MakeSystemControlRequest(action.name, action.arguments, request))
            {
                (void)taskBroker_->Complete(action.id, false, "invalidArguments"); continue;
            }
            const auto dispatched = snowdesktop::widget_runtime::DispatchSystemControlTask(request, action.preview,
                [&](snowdesktop::system_control::Request& liveRequest) -> snowdesktop::system_control::Result {
                    const auto authorized = [this, id=action.id, token=action.ownerToken, name=action.name]() {
                        if (!taskBroker_) return false;
                        const auto task = taskBroker_->Snapshot(id);
                        const auto permission = taskBroker_->RequiredPermission(name);
                        const auto live = std::find_if(widgets_.begin(), widgets_.end(), [token](const auto& candidate) { return candidate.runtimeToken == token; });
                        return task && !task->cancelRequested && task->ownerToken == token && live != widgets_.end() &&
                            permission && snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(live->permissions, *permission);
                    };
                    if (!authorized())
                        return {false, "permissionRevoked", 0};
                    if (snowdesktop::system_control::RequiresConfirmation(liveRequest.name) || snowdesktop::system_control::RequiresPasswordPrompt(liveRequest))
                    {
                        if (!systemControlPromptCallback_)
                            return {false, "confirmationUnavailable", 0};
                        // Copy identity before the modal message loop can unload/reload its owner.
                        const auto identity = Utf8ToWide(owner->packageId) + L"\n" + owner->widgetId;
                        snowdesktop::system_control::Result confirmed;
                        try { confirmed = systemControlPromptCallback_(liveRequest, identity, authorized); }
                        catch (...) { confirmed.error = "confirmationUnavailable"; }
                        if (!confirmed.ok || !authorized())
                            return {false, confirmed.error.empty() ? "canceled" : confirmed.error, 0};
                    }
                    if (!widgetSystemDataProvider_)
                        return {false, "providerUnavailable", 0};
                    if (!systemControlTasks_) systemControlTasks_ = std::make_unique<snowdesktop::widget_runtime::WidgetSystemControlTasks>(widgetSystemDataProvider_->Controls());
                    if (!systemControlTasks_->Start(action.id, action.ownerToken, std::move(liveRequest)))
                        return {false, "taskExecutorUnavailable", 0};
                    return {true, {}, 0};
            });
            if (!dispatched.ok || action.preview)
                (void)taskBroker_->Complete(action.id, dispatched.ok, dispatched.error);
            continue;
        }
        if (action.name == "app.search")
        {
            const auto query = action.arguments.find("query");
            const auto limitValue = action.arguments.find("limit");
            const auto offsetValue = action.arguments.find("offset");
            std::size_t limit = 0;
            std::size_t offset = 0;
            const auto parseNumber = [](const std::string& text,
                std::size_t& value) {
                const char* begin = text.data();
                const char* end = begin + text.size();
                const auto parsed = std::from_chars(begin, end, value);
                return parsed.ec == std::errc{} && parsed.ptr == end;
            };
            if (query == action.arguments.end() ||
                limitValue == action.arguments.end() ||
                offsetValue == action.arguments.end() ||
                !parseNumber(limitValue->second, limit) ||
                !parseNumber(offsetValue->second, offset))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }

            if (action.preview)
            {
                snowdesktop::widget_runtime::WidgetAppSearchCompletion
                    completion;
                completion.id = action.id;
                completion.catalogRevision = 1;
                completion.ok = true;
                if (offset == 0 && limit > 0)
                {
                    completion.items.push_back({ "preview-app",
                        _L("app.widget_preview.api.application"),
                        "preview-app", "Applications", "application" });
                }
                completion.nextOffset =
                    offset + completion.items.size();
                taskResults_.StoreAppSearch(
                    action.id, std::move(completion));
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }

            if (!applicationCatalogProvider_ || !appTaskExecutor_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "providerUnavailable");
                continue;
            }
            LuaApplicationCatalogSnapshot catalog;
            try
            {
                catalog = applicationCatalogProvider_();
            }
            catch (...)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "providerFailed");
                continue;
            }
            if (catalog.state != "ready")
            {
                (void)taskBroker_->Complete(action.id, false,
                    catalog.state == "indexing"
                        ? "appIndexNotReady" : "providerUnavailable");
                continue;
            }
            const std::wstring queryWide =
                Utf8ToWide(query->second);
            const std::string foldedQuery = WideToUtf8(
                ToUpperInvariant(queryWide));
            const std::string pinyinQuery =
                BuildNamePinyinFullKey(queryWide);
            if (foldedQuery.empty() ||
                !appTaskExecutor_->StartSearch(
                    action.id, foldedQuery, pinyinQuery,
                    offset, limit, appIndexRevision_,
                    std::move(catalog.entries)))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "taskExecutorUnavailable");
            }
            continue;
        }

        if (action.name == "desktop.search" ||
            action.name == "everything.search")
        {
            const auto query = action.arguments.find("query");
            const auto limitValue = action.arguments.find("limit");
            const auto offsetValue = action.arguments.find("offset");
            std::size_t limit = 0;
            std::size_t offset = 0;
            const auto parseNumber = [](const std::string& text,
                std::size_t& value) {
                const char* begin = text.data();
                const char* end = begin + text.size();
                const auto parsed = std::from_chars(begin, end, value);
                return parsed.ec == std::errc{} && parsed.ptr == end;
            };
            if (query == action.arguments.end() ||
                limitValue == action.arguments.end() ||
                offsetValue == action.arguments.end() ||
                !parseNumber(limitValue->second, limit) ||
                !parseNumber(offsetValue->second, offset))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }

            if (action.preview)
            {
                snowdesktop::widget_runtime::WidgetAppSearchCompletion
                    completion;
                completion.id = action.id;
                completion.catalogRevision = 1;
                completion.ok = true;
                if (offset == 0 && limit > 0)
                {
                    const bool desktop = action.name == "desktop.search";
                    completion.items.push_back({
                        desktop ? "preview-desktop" : "preview-everything",
                        desktop
                            ? _L("app.widget_preview.api.desktop_document")
                            : _L("app.widget_preview.api.search_result"),
                        desktop
                            ? "C:\\Users\\Maya\\Desktop\\Preview.txt"
                            : "C:\\Users\\Maya\\Documents\\Preview.txt",
                        desktop ? "Desktop" : "Everything",
                        "file" });
                }
                completion.nextOffset =
                    offset + completion.items.size();
                taskResults_.StoreItemSearch(
                    action.id, std::move(completion));
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }

            if (action.name == "desktop.search")
            {
                if (!desktopTaskExecutor_ || !desktopSnapshotProvider_)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "providerUnavailable");
                    continue;
                }
                std::vector<LuaDesktopItemInfo> desktopItems;
                try
                {
                    desktopItems = RuntimeDesktopItems();
                }
                catch (...)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "providerFailed");
                    continue;
                }
                if (desktopItems.size() > 2048) desktopItems.resize(2048);
                std::vector<snowdesktop::widget_runtime::
                    WidgetAppCatalogEntry> catalog;
                catalog.reserve(desktopItems.size());
                for (const auto& item : desktopItems)
                {
                    if (item.title.empty() || item.path.empty()) continue;
                    snowdesktop::widget_runtime::WidgetAppCatalogEntry entry;
                    entry.id = item.id.empty() ? item.path : item.id;
                    entry.title = item.title;
                    entry.launchTarget = item.path;
                    const std::wstring titleWide =
                        Utf8ToWide(item.title);
                    entry.foldedTitle = WideToUtf8(
                        ToUpperInvariant(titleWide));
                    entry.pinyinFull = BuildNamePinyinFullKey(titleWide);
                    entry.pinyinInitials =
                        BuildNamePinyinInitialKey(titleWide);
                    entry.source = item.source;
                    entry.type = item.type;
                    if (!entry.id.empty() && !entry.foldedTitle.empty())
                        catalog.push_back(std::move(entry));
                }
                const std::wstring queryWide =
                    Utf8ToWide(query->second);
                const std::string foldedQuery = WideToUtf8(
                    ToUpperInvariant(queryWide));
                const std::string pinyinQuery =
                    BuildNamePinyinFullKey(queryWide);
                if (foldedQuery.empty() ||
                    !desktopTaskExecutor_->StartSearch(
                        action.id, foldedQuery, pinyinQuery,
                        offset, limit, desktopDataRevision_,
                        std::move(catalog)))
                {
                    (void)taskBroker_->Complete(action.id, false,
                        "taskExecutorUnavailable");
                }
                continue;
            }

            if (!externalItemTaskExecutor_ || !everythingSearchProvider_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "providerUnavailable");
                continue;
            }
            const auto provider = [this](const std::string& searchQuery,
                std::size_t maximumResults) {
                std::vector<snowdesktop::widget_runtime::
                    WidgetAppSearchResult> result;
                const std::vector<LuaDesktopItemInfo> items =
                    RuntimeEverythingSearch(searchQuery,
                        static_cast<int>(maximumResults));
                result.reserve(items.size());
                for (const auto& item : items)
                {
                    if (item.path.empty() || item.title.empty()) continue;
                    result.push_back({
                        item.id.empty() ? item.path : item.id,
                        item.title, item.path, item.source, item.type });
                }
                return result;
            };
            if (!externalItemTaskExecutor_->StartSearch(
                    action.id, query->second, offset, limit, provider))
            {
                (void)taskBroker_->Complete(action.id, false,
                    "taskExecutorUnavailable");
            }
            continue;
        }

        if (action.name == "network.request")
        {
            networkTaskDispatch_.Start(action, owner->widgetId,
                owner->packageId, owner->manifest.networkDomains,
                secretStore_.get(), httpService_.get(), *taskBroker_);
            continue;
        }

        if (action.name == "calendar.create" ||
            action.name == "calendar.update" ||
            action.name == "calendar.remove" ||
            action.name == "calendar.series.create" ||
            action.name == "calendar.series.update" ||
            action.name == "calendar.series.remove")
        {
            snowdesktop::calendar::MutationResult result;
            const bool seriesTask = action.name.starts_with("calendar.series.");
            const auto parseInteger = [](const std::string& text,
                int& value) {
                const char* begin = text.data();
                const char* finish = begin + text.size();
                const auto parsed = std::from_chars(begin, finish, value);
                return parsed.ec == std::errc{} && parsed.ptr == finish;
            };
            if (action.preview)
            {
                result.error = "previewReadOnly";
            }
            else if (action.name == "calendar.remove" ||
                action.name == "calendar.series.remove")
            {
                const auto id = action.arguments.find("id");
                if (id == action.arguments.end()) result.error = "invalidArguments";
                else if (seriesTask)
                {
                    const auto revision = action.arguments.find("expectedRevision");
                    int expectedRevision = 0;
                    result = revision != action.arguments.end() &&
                        parseInteger(revision->second, expectedRevision)
                        ? RuntimeCalendarSeriesRemove(id->second, expectedRevision)
                        : snowdesktop::calendar::MutationResult{false, id->second, 0, "invalidArguments"};
                }
                else result = RuntimeCalendarRemove(id->second);
            }
            else
            {
                const auto title = action.arguments.find("title");
                const auto date = action.arguments.find("date");
                const auto notes = action.arguments.find("notes");
                const auto allDay = action.arguments.find("allDay");
                const auto start = action.arguments.find("startMinutes");
                const auto end = action.arguments.find("endMinutes");
                const auto reminder =
                    action.arguments.find("reminderMinutes");
                snowdesktop::calendar::CalendarEvent event;
                bool valid = title != action.arguments.end() &&
                    date != action.arguments.end() &&
                    notes != action.arguments.end() &&
                    allDay != action.arguments.end() &&
                    start != action.arguments.end() &&
                    end != action.arguments.end() &&
                    reminder != action.arguments.end();
                if (valid)
                {
                    event.title = title->second;
                    event.date = date->second;
                    event.notes = notes->second;
                    event.allDay = allDay->second == "1";
                    valid = parseInteger(
                        start->second, event.startMinutes) &&
                        parseInteger(end->second, event.endMinutes) &&
                        parseInteger(reminder->second,
                            event.reminderMinutes);
                }
                if (!valid)
                {
                    result.error = "invalidArguments";
                }
                else if (seriesTask)
                {
                    snowdesktop::calendar::CalendarSeries series;
                    series.event = std::move(event);
                    const auto get = [&](const char* key) -> const std::string* {
                        const auto found = action.arguments.find(key);
                        return found == action.arguments.end() ? nullptr : &found->second;
                    };
                    const auto* kind = get("kind");
                    const auto* startDate = get("startDate");
                    const auto* endDate = get("endDate");
                    const auto* interval = get("interval");
                    const auto* monthDay = get("monthDay");
                    const auto* dates = get("dates");
                    const auto* weekdays = get("weekdays");
                    valid = kind && startDate && endDate && interval &&
                        monthDay && dates && weekdays &&
                        parseInteger(*interval, series.rule.interval) &&
                        parseInteger(*monthDay, series.rule.monthDay);
                    if (valid)
                    {
                        series.rule.kind = *kind;
                        series.rule.startDate = *startDate;
                        series.rule.endDate = *endDate;
                        const auto decode = [&](const std::string& encoded,
                            bool dateValues) {
                            std::size_t offset = 0;
                            while (offset < encoded.size())
                            {
                                const auto separator = encoded.find(',', offset);
                                const auto value = encoded.substr(offset,
                                    separator == std::string::npos ? std::string::npos : separator - offset);
                                if (dateValues) series.rule.dates.push_back(value);
                                else
                                {
                                    int weekday = 0;
                                    if (!parseInteger(value, weekday)) return false;
                                    series.rule.weekdays.push_back(weekday);
                                }
                                if (separator == std::string::npos) break;
                                offset = separator + 1;
                            }
                            return true;
                        };
                        valid = decode(*dates, true) && decode(*weekdays, false);
                    }
                    if (!valid) result.error = "invalidArguments";
                    else if (action.name == "calendar.series.create")
                        result = RuntimeCalendarSeriesCreate(std::move(series));
                    else
                    {
                        const auto* id = get("id");
                        const auto* revision = get("expectedRevision");
                        int expectedRevision = 0;
                        result = id && revision &&
                            parseInteger(*revision, expectedRevision)
                            ? RuntimeCalendarSeriesUpdate(*id, expectedRevision, std::move(series))
                            : snowdesktop::calendar::MutationResult{false, {}, 0, "invalidArguments"};
                    }
                }
                else if (action.name == "calendar.create")
                {
                    result = RuntimeCalendarCreate(std::move(event));
                }
                else
                {
                    const auto id = action.arguments.find("id");
                    const auto revision =
                        action.arguments.find("expectedRevision");
                    int expectedRevision = 0;
                    if (id == action.arguments.end() ||
                        revision == action.arguments.end() ||
                        !parseInteger(revision->second, expectedRevision))
                    {
                        result.error = "invalidArguments";
                    }
                    else
                    {
                        result = RuntimeCalendarUpdate(id->second,
                            expectedRevision, std::move(event));
                    }
                }
            }
            taskResults_.StoreCalendar(
                action.id, result);
            (void)taskBroker_->Complete(
                action.id, result.ok, result.error);
            continue;
        }

        const bool notificationTask =
            action.name == "notification.show" ||
            action.name == "notification.update" ||
            action.name == "notification.dismiss" ||
            action.name == "notification.schedule" ||
            action.name == "notification.cancel";
        if (notificationTask)
        {
            const std::string notificationInstanceId =
                WideToUtf8(owner->widgetId);
            if (action.preview)
            {
                if (action.name == "notification.show" ||
                    action.name == "notification.schedule")
                {
                    taskResults_.StoreNotification(
                        action.id, "notification:preview:" +
                            std::to_string(action.id));
                }
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!notificationCenter_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "providerUnavailable");
                continue;
            }

            using Result = snowdesktop::widget_runtime::
                WidgetNotificationOperationResult;
            Result result;
            const auto host = [this](const snowdesktop::widget_runtime::
                WidgetNotificationHostRequest& request) {
                return notifyCallback_ && notifyCallback_(request);
            };
            const auto parseProgress = [](const std::string& value,
                double& output) {
                if (value.empty()) return false;
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), output,
                    std::chars_format::general);
                return parsed.ec == std::errc{} &&
                    parsed.ptr == value.data() + value.size() &&
                    std::isfinite(output) && output >= 0.0 && output <= 1.0;
            };
            const auto readActions = [&action](
                std::vector<snowdesktop::widget_runtime::
                    WidgetNotificationAction>& actions) {
                const auto count = action.arguments.find("actionCount");
                if (count == action.arguments.end()) return true;
                int actionCount = -1;
                const auto parsed = std::from_chars(
                    count->second.data(),
                    count->second.data() + count->second.size(),
                    actionCount);
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != count->second.data() +
                        count->second.size() ||
                    actionCount < 0 || actionCount > 2)
                    return false;
                actions.clear();
                actions.reserve(static_cast<std::size_t>(actionCount));
                for (int index = 0; index < actionCount; ++index)
                {
                    const auto id = action.arguments.find(
                        "action" + std::to_string(index) + "Id");
                    const auto label = action.arguments.find(
                        "action" + std::to_string(index) + "Label");
                    if (id == action.arguments.end() ||
                        label == action.arguments.end())
                        return false;
                    actions.push_back({
                        id->second, Utf8ToWide(label->second) });
                }
                return true;
            };
            const auto applyStructuredContent =
                [this, owner, &action, &parseProgress, &readActions](
                    snowdesktop::widget_runtime::
                        WidgetNotificationContent& content) {
                    const auto image =
                        action.arguments.find("imageResource");
                    if (image != action.arguments.end() &&
                        !image->second.empty())
                    {
                        const auto path = RuntimeResolvePackageResource(
                            owner->widgetId, image->second, "image");
                        if (!path) return false;
                        content.imagePath = *path;
                    }
                    const auto progress =
                        action.arguments.find("progress");
                    if (progress != action.arguments.end())
                    {
                        double value = 0.0;
                        if (!parseProgress(progress->second, value))
                            return false;
                        content.progress = value;
                    }
                    return readActions(content.actions);
                };
            if (action.name == "notification.show")
            {
                const auto title = action.arguments.find("title");
                const auto message = action.arguments.find("message");
                if (title == action.arguments.end() ||
                    message == action.arguments.end())
                {
                    result.error = "invalidArguments";
                }
                else
                {
                    snowdesktop::widget_runtime::
                        WidgetNotificationContent content;
                    content.title = Utf8ToWide(title->second);
                    content.message = Utf8ToWide(message->second);
                    if (!applyStructuredContent(content))
                    {
                        result.error = "invalidArguments";
                    }
                    std::string admissionError;
                    if (result.error.empty())
                    {
                        result = notificationCenter_->Show(
                            owner->runtimeToken, std::move(content),
                            snowdesktop::widget_runtime::
                                WidgetNotificationCenter::Clock::now(),
                            [this, owner, &admissionError, &host](
                                const snowdesktop::widget_runtime::
                                    WidgetNotificationHostRequest& request) {
                                admissionError =
                                    RuntimeAdmitNotification(owner->widgetId);
                                return admissionError.empty() && host(request);
                            });
                    }
                    if (!admissionError.empty())
                        result.error = std::move(admissionError);
                }
            }
            else if (action.name == "notification.schedule")
            {
                const auto title = action.arguments.find("title");
                const auto message = action.arguments.find("message");
                const auto at = action.arguments.find("atMs");
                std::int64_t atMs = 0;
                const auto parsed = at == action.arguments.end()
                    ? std::from_chars_result{}
                    : std::from_chars(at->second.data(),
                        at->second.data() + at->second.size(), atMs);
                const std::int64_t nowMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        snowdesktop::widget_runtime::
                            WidgetNotificationCenter::Clock::now().
                                time_since_epoch()).count();
                constexpr std::int64_t maximumDelayMs =
                    366LL * 24 * 60 * 60 * 1000;
                if (title == action.arguments.end() ||
                    message == action.arguments.end() ||
                    at == action.arguments.end() ||
                    parsed.ec != std::errc{} ||
                    parsed.ptr != at->second.data() + at->second.size() ||
                    atMs <= nowMs || atMs > nowMs + maximumDelayMs)
                {
                    result.error = "invalidArguments";
                }
                else
                {
                    snowdesktop::widget_runtime::
                        WidgetNotificationContent content;
                    content.title = Utf8ToWide(title->second);
                    content.message = Utf8ToWide(message->second);
                    if (!applyStructuredContent(content))
                    {
                        result.error = "invalidArguments";
                    }
                    const auto due = snowdesktop::widget_runtime::
                        WidgetNotificationCenter::Clock::time_point(
                            std::chrono::milliseconds(atMs));
                    if (result.error.empty())
                    {
                        result = notificationCenter_->Schedule(
                            owner->runtimeToken, content, due,
                            snowdesktop::widget_runtime::
                                WidgetNotificationCenter::Clock::now());
                    }
                    if (result.ok)
                    {
                        snowdesktop::widget_runtime::
                            WidgetPersistedNotificationSchedule persisted{
                                notificationInstanceId,
                                owner->packageId, result.id,
                                title->second, message->second, atMs };
                        if (const auto image = action.arguments.find(
                                "imageResource");
                            image != action.arguments.end())
                            persisted.imageResource = image->second;
                        persisted.progress = content.progress;
                        for (const auto& notificationAction :
                            content.actions)
                        {
                            persisted.actions.push_back({
                                notificationAction.id,
                                WideToUtf8(
                                    notificationAction.label) });
                        }
                        std::string persistenceError;
                        const bool stored = notificationScheduleStore_ &&
                            notificationScheduleStore_->Upsert(
                                std::move(persisted),
                                persistenceError) &&
                            SaveNotificationSchedules();
                        if (!stored)
                        {
                            if (notificationScheduleStore_)
                            {
                                (void)notificationScheduleStore_->Remove(
                                    notificationInstanceId, result.id);
                            }
                            (void)notificationCenter_->Cancel(
                                owner->runtimeToken, result.id);
                            result = { false, {}, "persistenceFailed" };
                        }
                    }
                }
            }
            else
            {
                const auto id = action.arguments.find("notificationId");
                if (id == action.arguments.end())
                {
                    result.error = "invalidArguments";
                }
                else if (action.name == "notification.update")
                {
                    const auto title = action.arguments.find("title");
                    const auto message = action.arguments.find("message");
                    snowdesktop::widget_runtime::WidgetNotificationPatch patch;
                    if (title != action.arguments.end())
                        patch.title = Utf8ToWide(title->second);
                    if (message != action.arguments.end())
                        patch.message = Utf8ToWide(message->second);
                    const auto image = action.arguments.find("imageResource");
                    if (image != action.arguments.end())
                    {
                        if (image->second.empty())
                        {
                            patch.imagePath = std::wstring{};
                        }
                        else
                        {
                            const auto imagePath = RuntimeResolvePackageResource(
                                owner->widgetId, image->second, "image");
                            if (!imagePath) result.error = "invalidArguments";
                            else patch.imagePath = *imagePath;
                        }
                    }
                    const auto progress = action.arguments.find("progress");
                    if (progress != action.arguments.end())
                    {
                        if (progress->second.empty())
                        {
                            patch.progress.emplace(std::optional<double>{});
                        }
                        else
                        {
                            double progressValue = 0.0;
                            if (!parseProgress(progress->second, progressValue))
                                result.error = "invalidArguments";
                            else
                                patch.progress.emplace(progressValue);
                        }
                    }
                    const auto actionCount =
                        action.arguments.find("actionCount");
                    if (actionCount != action.arguments.end())
                    {
                        std::vector<snowdesktop::widget_runtime::
                            WidgetNotificationAction> actions;
                        if (!readActions(actions))
                            result.error = "invalidArguments";
                        else
                            patch.actions = std::move(actions);
                    }
                    const auto persisted = notificationScheduleStore_
                        ? notificationScheduleStore_->Find(
                            notificationInstanceId, id->second)
                        : std::nullopt;
                    std::string admissionError;
                    if (result.error.empty())
                    {
                        result = notificationCenter_->Update(
                            owner->runtimeToken, id->second,
                            std::move(patch),
                            [this, owner, &admissionError, &host](
                                const snowdesktop::widget_runtime::
                                    WidgetNotificationHostRequest& request) {
                                admissionError =
                                    RuntimeAdmitNotification(owner->widgetId);
                                return admissionError.empty() && host(request);
                            });
                    }
                    if (!admissionError.empty())
                        result.error = std::move(admissionError);
                    if (result.ok && persisted)
                    {
                        const std::optional<std::string> persistedTitle =
                            title != action.arguments.end()
                            ? std::optional(title->second) : std::nullopt;
                        const std::optional<std::string> persistedMessage =
                            message != action.arguments.end()
                            ? std::optional(message->second) : std::nullopt;
                        const std::optional<std::string> persistedImage =
                            image != action.arguments.end()
                            ? std::optional(image->second) : std::nullopt;
                        std::optional<std::optional<double>> persistedProgress;
                        if (progress != action.arguments.end())
                        {
                            if (progress->second.empty())
                                persistedProgress.emplace(
                                    std::optional<double>{});
                            else
                            {
                                double value = 0.0;
                                (void)parseProgress(progress->second, value);
                                persistedProgress.emplace(value);
                            }
                        }
                        std::optional<std::vector<
                            snowdesktop::widget_runtime::
                                WidgetPersistedNotificationAction>>
                            persistedActions;
                        if (actionCount != action.arguments.end())
                        {
                            persistedActions.emplace();
                            int count = 0;
                            (void)std::from_chars(
                                actionCount->second.data(),
                                actionCount->second.data() +
                                    actionCount->second.size(), count);
                            for (int index = 0; index < count; ++index)
                            {
                                persistedActions->push_back({
                                    action.arguments.at("action" +
                                        std::to_string(index) + "Id"),
                                    action.arguments.at("action" +
                                        std::to_string(index) + "Label") });
                            }
                        }
                        if (!notificationScheduleStore_->UpdateContent(
                                notificationInstanceId, id->second,
                                persistedTitle, persistedMessage,
                                persistedImage, persistedProgress,
                                persistedActions) ||
                            !SaveNotificationSchedules())
                        {
                            std::string ignored;
                            (void)notificationScheduleStore_->Upsert(
                                *persisted, ignored);
                            (void)SaveNotificationSchedules();
                            snowdesktop::widget_runtime::
                                WidgetNotificationPatch rollback;
                            rollback.title = Utf8ToWide(persisted->title);
                            rollback.message = Utf8ToWide(
                                persisted->message);
                            if (persisted->imageResource.empty())
                                rollback.imagePath = std::wstring{};
                            else if (const auto oldImage =
                                RuntimeResolvePackageResource(owner->widgetId,
                                    persisted->imageResource, "image"))
                                rollback.imagePath = *oldImage;
                            else
                                rollback.imagePath = std::wstring{};
                            rollback.progress.emplace(persisted->progress);
                            rollback.actions.emplace();
                            for (const auto& oldAction : persisted->actions)
                            {
                                rollback.actions->push_back({ oldAction.id,
                                    Utf8ToWide(oldAction.label) });
                            }
                            (void)notificationCenter_->Update(
                                owner->runtimeToken, id->second,
                                std::move(rollback), host);
                            result = { false, {}, "persistenceFailed" };
                        }
                    }
                }
                else if (action.name == "notification.dismiss")
                {
                    result = notificationCenter_->Dismiss(
                        owner->runtimeToken, id->second, host);
                }
                else
                {
                    const auto persisted = notificationScheduleStore_
                        ? notificationScheduleStore_->Find(
                            notificationInstanceId, id->second)
                        : std::nullopt;
                    result = notificationCenter_->Cancel(
                        owner->runtimeToken, id->second);
                    if (result.ok)
                    {
                        const bool removed = persisted &&
                            notificationScheduleStore_ &&
                            notificationScheduleStore_->Remove(
                                notificationInstanceId, id->second);
                        if (!removed || !SaveNotificationSchedules())
                        {
                            if (persisted && notificationScheduleStore_)
                            {
                                std::string ignored;
                                (void)notificationScheduleStore_->Upsert(
                                    *persisted, ignored);
                                (void)SaveNotificationSchedules();
                                const auto due = snowdesktop::widget_runtime::
                                    WidgetNotificationCenter::Clock::
                                        time_point(std::chrono::milliseconds(
                                            persisted->dueMs));
                                snowdesktop::widget_runtime::
                                    WidgetNotificationContent content;
                                content.title = Utf8ToWide(
                                    persisted->title);
                                content.message = Utf8ToWide(
                                    persisted->message);
                                if (!persisted->imageResource.empty())
                                {
                                    if (const auto imagePath =
                                        RuntimeResolvePackageResource(
                                            owner->widgetId,
                                            persisted->imageResource,
                                            "image"))
                                        content.imagePath = *imagePath;
                                }
                                content.progress = persisted->progress;
                                for (const auto& oldAction :
                                    persisted->actions)
                                {
                                    content.actions.push_back({ oldAction.id,
                                        Utf8ToWide(oldAction.label) });
                                }
                                (void)notificationCenter_->RestoreScheduled(
                                    owner->runtimeToken,
                                    persisted->notificationId,
                                    std::move(content), due,
                                    snowdesktop::widget_runtime::
                                        WidgetNotificationCenter::Clock::
                                            now());
                            }
                            result = { false, {}, "persistenceFailed" };
                        }
                    }
                }
            }
            if (result.ok && (action.name == "notification.show" ||
                    action.name == "notification.schedule"))
            {
                taskResults_.StoreNotification(
                    action.id, result.id);
            }
            (void)taskBroker_->Complete(action.id,
                result.ok, std::move(result.error));
            continue;
        }

        if (action.name == "app.launch")
        {
            const auto refValue = action.arguments.find("ref");
            if (refValue == action.arguments.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            const auto reference = owner->applicationReferences.find(
                refValue->second);
            if (reference == owner->applicationReferences.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidReference");
                continue;
            }
            if (action.preview)
            {
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!reference->second.available)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "unavailableReference");
                continue;
            }
            if (!reference->second.persistent &&
                reference->second.catalogRevision != appIndexRevision_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "staleReference");
                continue;
            }
            const std::wstring launchTarget = snowdesktop::
                logical_slot_picker_rules::NormalizeApplicationLaunchTarget(
                    Utf8ToWide(reference->second.launchTarget));
            const bool accepted = !launchTarget.empty() &&
                applicationLaunchCallback_ &&
                applicationLaunchCallback_(launchTarget);
            (void)taskBroker_->Complete(action.id, accepted,
                accepted ? std::string{} : "launchRejected");
            continue;
        }

        if (action.name == "shell.openItem" ||
            action.name == "shell.revealItem")
        {
            const auto refValue = action.arguments.find("ref");
            if (refValue == action.arguments.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            const auto reference = owner->itemReferences.find(
                refValue->second);
            if (reference == owner->itemReferences.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidReference");
                continue;
            }
            if (action.preview)
            {
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!reference->second.available)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "unavailableReference");
                continue;
            }
            if (reference->second.sourceTask == "desktop.search" &&
                reference->second.revision != desktopDataRevision_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "staleReference");
                continue;
            }
            const std::wstring target = Utf8ToWide(
                reference->second.target);
            const bool reveal = action.name == "shell.revealItem";
            const bool accepted = !target.empty() &&
                (reveal ? RuntimeRevealDesktopPath(target)
                        : RuntimeOpenDesktopPath(target));
            (void)taskBroker_->Complete(action.id, accepted,
                accepted ? std::string{} :
                    (reveal ? "revealRejected" : "openRejected"));
            continue;
        }

        if (action.name == "desktop.refresh")
        {
            if (!action.preview) RuntimeRefreshDesktop();
            (void)taskBroker_->Complete(action.id, true);
            continue;
        }

        if (action.name == "system.openSettings")
        {
            const auto page = action.arguments.find("page");
            const auto hostPage = page == action.arguments.end()
                ? std::nullopt
                : snowdesktop::widget_runtime::HostSettingsPage(page->second);
            const auto uri = page == action.arguments.end() || hostPage
                ? std::nullopt
                : snowdesktop::widget_runtime::SystemSettingsUri(
                    page->second);
            if (!uri && !hostPage)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (action.preview)
            {
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (hostPage)
            {
                const bool accepted = hostSettingsPageCallback_ &&
                    hostSettingsPageCallback_(*hostPage);
                (void)taskBroker_->Complete(action.id, accepted,
                    accepted ? std::string{} : "openRejected");
                continue;
            }
            const bool accepted = RuntimeOpenDesktopPath(
                std::wstring(*uri));
            (void)taskBroker_->Complete(action.id, accepted,
                accepted ? std::string{} : "openRejected");
            continue;
        }

        if (action.name == "shell.openUri")
        {
            const auto url = action.arguments.find("url");
            if (url == action.arguments.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (action.preview)
            {
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            const std::wstring target = Utf8ToWide(url->second);
            const bool valid = snowdesktop::http_security::
                IsAllowedPublicHttpsUrl(target);
            const bool accepted = valid && RuntimeOpenDesktopPath(target);
            (void)taskBroker_->Complete(action.id, accepted,
                accepted ? std::string{} :
                    (valid ? "openRejected" : "invalidUrl"));
            continue;
        }

        if (snowdesktop::widget_runtime::WidgetClipboardTaskExecutor::
                SupportsAction(action.name))
        {
            using namespace snowdesktop::widget_runtime;
            WidgetClipboardTaskRequest clipboardRequest;
            const ClipboardWriteSources sources{
                [this, &owner](const std::string& name, WidgetClipboardTaskRequest& request) {
                    if (IsWidgetRuntimeImageToken(name))
                    {
                        request.image = ResolveTaskImage(owner->widgetId, name);
                    }
                    else if (const auto path = RuntimeResolvePackageResource(
                            owner->widgetId, name, "image"))
                    {
                        request.imagePath = *path;
                    }
                },
                [this, &owner, &action](const std::string& reference, std::filesystem::path& path) {
                    return ResolveClipboardWriteFile(*owner, action,
                        filesystemHandleStore_.get(), reference, path);
                },
            };
            if (const char* error = PrepareClipboardTaskRequest(action, sources, clipboardRequest))
            {
                (void)taskBroker_->Complete(action.id, false, error);
                continue;
            }
            if (action.preview)
            {
                if (action.name == "clipboard.read")
                    clipboardTaskDispatch_.StorePreviewRead(action.id, clipboardRequest.format);
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!clipboardTaskExecutor_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "taskExecutorUnavailable");
                continue;
            }
            auto start = clipboardTaskExecutor_->Start(
                action.id, action.instanceId,
                std::move(clipboardRequest));
            if (!start)
            {
                (void)taskBroker_->Complete(action.id, false,
                    start.error.empty()
                        ? "taskExecutorUnavailable" : start.error);
            }
            continue;
        }

        if (IsFilesystemPickerTask(action.name))
        {
            LuaWidgetFilePickerRequest request;
            request.multiple = action.arguments.contains("multiple") &&
                action.arguments.at("multiple") == "true";
            snowdesktop::widget_runtime::WidgetFilesystemHandleKind kind =
                snowdesktop::widget_runtime::WidgetFilesystemHandleKind::File;
            if (action.name == "filesystem.pickOpen")
            {
                request.kind = LuaWidgetFilePickerKind::OpenFile;
                request.access = snowdesktop::widget_runtime::
                    WidgetFilesystemHandleAccess::Read;
            }
            else if (action.name == "filesystem.pickSave")
            {
                request.kind = LuaWidgetFilePickerKind::SaveFile;
                request.access = snowdesktop::widget_runtime::
                    WidgetFilesystemHandleAccess::Write;
            }
            else
            {
                request.kind = LuaWidgetFilePickerKind::Folder;
                kind = snowdesktop::widget_runtime::
                    WidgetFilesystemHandleKind::Folder;
                const auto access = action.arguments.find("access");
                if (access == action.arguments.end())
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "invalidArguments");
                    continue;
                }
                if (access->second == "read")
                    request.access = snowdesktop::widget_runtime::
                        WidgetFilesystemHandleAccess::Read;
                else if (access->second == "write")
                    request.access = snowdesktop::widget_runtime::
                        WidgetFilesystemHandleAccess::Write;
                else if (access->second == "readWrite")
                    request.access = snowdesktop::widget_runtime::
                        WidgetFilesystemHandleAccess::ReadWrite;
                else
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "invalidArguments");
                    continue;
                }
            }
            if (const auto extensions = action.arguments.find("extensions");
                extensions != action.arguments.end())
            {
                std::size_t begin = 0;
                while (begin <= extensions->second.size())
                {
                    const std::size_t end = extensions->second.find(
                        ';', begin);
                    const std::string value = extensions->second.substr(
                        begin, end == std::string::npos
                            ? std::string::npos : end - begin);
                    if (!value.empty())
                        request.extensions.push_back(Utf8ToWide(value));
                    if (end == std::string::npos) break;
                    begin = end + 1;
                }
            }
            if (const auto suggestedName =
                    action.arguments.find("suggestedName");
                suggestedName != action.arguments.end())
                request.suggestedName =
                    Utf8ToWide(suggestedName->second);

            if (action.preview)
            {
                filesystemTaskDispatch_.StorePreviewSelection(action.id,
                    { action.instanceId, owner->packageId }, kind, request.access);
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!filePickerCallback_ || !filesystemHandleStore_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "providerUnavailable");
                continue;
            }

            LuaWidgetFilePickerResult selected;
            try
            {
                selected = filePickerCallback_(request);
            }
            catch (...)
            {
                selected.error = "pickerFailed";
            }
            if (!selected)
            {
                (void)taskBroker_->Complete(action.id, false,
                    selected.canceled ? "userCanceled" :
                        (selected.error.empty()
                            ? "pickerFailed" : selected.error));
                continue;
            }
            // The modal picker pumps messages; resolve the owner again before granting.
            owner = std::find_if(widgets_.begin(), widgets_.end(), [&action](const LuaWidget& candidate) {
                return candidate.runtimeToken == action.ownerToken;
            });
            const auto currentTask = taskBroker_->Snapshot(action.id);
            if (owner == widgets_.end() || !currentTask || currentTask->cancelRequested)
            {
                (void)taskBroker_->Complete(action.id, false, "canceled");
                continue;
            }
            const bool needsRead = request.access != snowdesktop::widget_runtime::WidgetFilesystemHandleAccess::Write;
            const bool needsWrite = request.access != snowdesktop::widget_runtime::WidgetFilesystemHandleAccess::Read;
            if ((needsRead && !snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(owner->permissions, kFilesystemReadPermission)) ||
                (needsWrite && !snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(owner->permissions, kFilesystemWritePermission)))
            {
                (void)taskBroker_->Complete(action.id, false, "permissionRevoked");
                continue;
            }
            if (selected.paths.empty()) selected.paths.push_back(selected.path);
            if (selected.paths.size() > (request.multiple ? 128u : 1u))
            {
                (void)taskBroker_->Complete(action.id, false, "tooManySelections");
                continue;
            }
            std::string grantError = filesystemTaskDispatch_.GrantSelection(action.id,
                *filesystemHandleStore_, { action.instanceId, owner->packageId },
                selected.paths, kind, request.access);
            if (!grantError.empty())
            {
                (void)taskBroker_->Complete(action.id, false, std::move(grantError));
                continue;
            }
            (void)taskBroker_->Complete(action.id, true);
            continue;
        }

        if (IsFilesystemHandleTask(action.name))
        {
            const auto handle = action.arguments.find("handle");
            if (handle == action.arguments.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            const bool previewHandle = action.preview &&
                IsPreviewFilesystemHandle(handle->second);
            std::optional<snowdesktop::widget_runtime::
                WidgetFilesystemHandleEntry> entry;
            if (!previewHandle)
            {
                if (!filesystemHandleStore_)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "providerUnavailable");
                    continue;
                }
                entry = filesystemHandleStore_->Resolve(
                    { action.instanceId, owner->packageId },
                    handle->second);
                if (!entry)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "invalidReference");
                    continue;
                }
            }

            if (action.name == "filesystem.release")
            {
                if (action.preview)
                {
                    (void)taskBroker_->Complete(action.id, true);
                    continue;
                }
                if (RuntimeIsFilesystemSettingHandleValue(
                        Utf8ToWide(action.instanceId),
                        handle->second))
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "hostManagedReference");
                    continue;
                }
                const bool busy = filesystemTaskDispatch_.HasActiveHandle(handle->second) ||
                    std::any_of(
                        filesystemWatchBindings_.begin(),
                        filesystemWatchBindings_.end(),
                        [&handle](const auto& active) {
                            return active.second.sourceHandle ==
                                handle->second;
                        });
                if (busy)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "handleBusy");
                    continue;
                }
                std::string revokeError;
                const bool revoked = filesystemHandleStore_->Revoke(
                    { action.instanceId, owner->packageId },
                    handle->second, revokeError);
                if (revoked && audioPlayback_) audioPlayback_->RevokeFile(action.ownerToken, handle->second);
                (void)taskBroker_->Complete(action.id, revoked,
                    revoked ? std::string{} :
                        (revokeError.empty()
                            ? "invalidReference" : revokeError));
                continue;
            }

            snowdesktop::widget_runtime::WidgetFilesystemTaskRequest request;
            request.action = action.name;
            request.handle = handle->second;
            if (entry) request.path = entry->path;
            if (const auto name = action.arguments.find("name"); name != action.arguments.end())
                request.name = name->second;
            if (const auto grant = action.arguments.find("grantHandles"); grant != action.arguments.end())
                request.grantHandles = grant->second != "false";
            if (const auto encoding = action.arguments.find("encoding");
                encoding != action.arguments.end())
                request.encoding = encoding->second;
            const auto parseSize = [&action](const char* field,
                std::size_t fallback, std::size_t& output) {
                const auto value = action.arguments.find(field);
                if (value == action.arguments.end())
                {
                    output = fallback;
                    return true;
                }
                const char* begin = value->second.data();
                const char* end = begin + value->second.size();
                const auto parsed = std::from_chars(begin, end, output);
                return parsed.ec == std::errc{} && parsed.ptr == end;
            };
            if (action.name == "filesystem.list" &&
                (!parseSize("offset", 0, request.offset) ||
                    !parseSize("limit", 50, request.limit)))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (action.name == "filesystem.image" &&
                !parseSize("maxDimension", 2048, request.maxDimension))
            {
                (void)taskBroker_->Complete(action.id, false, "invalidArguments");
                continue;
            }
            if (action.name == "filesystem.read" &&
                !parseSize("maxBytes", 512 * 1024, request.maxBytes))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (action.name == "filesystem.write")
            {
                const auto text = action.arguments.find(
                    request.encoding == "binary" ? "data" : "text");
                if (text == action.arguments.end())
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "invalidArguments");
                    continue;
                }
                request.text = text->second;
                if (const auto revision =
                        action.arguments.find("expectedRevision");
                    revision != action.arguments.end())
                    request.expectedRevision = revision->second;
            }

            if (action.preview)
            {
                filesystemTaskDispatch_.StorePreviewResult(action.id, request);
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!filesystemTaskExecutor_ ||
                !snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor::
                    ValidateRequest(request))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "taskExecutorUnavailable");
                continue;
            }
            auto start = filesystemTaskExecutor_->Start(
                action.id, action.instanceId, std::move(request));
            if (start)
                filesystemTaskDispatch_.TrackHandle(action.id, handle->second);
            else
                (void)taskBroker_->Complete(action.id, false,
                    start.error.empty()
                        ? "taskExecutorUnavailable" : start.error);
            continue;
        }

        if (action.preview)
        {
            (void)taskBroker_->Complete(action.id, true);
            continue;
        }
        if (snowdesktop::widget_runtime::WidgetAudioOutputTaskExecutor::
                SupportsAction(action.name))
        {
            snowdesktop::widget_runtime::WidgetAudioOutputTaskRequest
                audioRequest;
            audioRequest.action = action.name;
            audioRequest.coalesce = action.arguments.contains("coalesce") &&
                action.arguments.at("coalesce") == "1";
            bool validAudioRequest = true;
            if (action.name == "audio.output.setVolume")
            {
                const auto value = action.arguments.find("volume");
                double volume = 0.0;
                if (value == action.arguments.end())
                    validAudioRequest = false;
                else
                {
                    const char* begin = value->second.data();
                    const char* end = begin + value->second.size();
                    const auto parsed = std::from_chars(
                        begin, end, volume, std::chars_format::general);
                    validAudioRequest = parsed.ec == std::errc{} &&
                        parsed.ptr == end;
                    if (validAudioRequest) audioRequest.volume = volume;
                }
            }
            else
            {
                const auto value = action.arguments.find("muted");
                validAudioRequest = value != action.arguments.end() &&
                    (value->second == "0" || value->second == "1");
                if (validAudioRequest)
                    audioRequest.muted = value->second == "1";
            }
            validAudioRequest = validAudioRequest &&
                snowdesktop::widget_runtime::
                    WidgetAudioOutputTaskExecutor::ValidateRequest(
                        audioRequest);
            if (!validAudioRequest)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (!audioOutputTaskExecutor_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "taskExecutorUnavailable");
                continue;
            }
            auto start = audioOutputTaskExecutor_->Start(
                action.id, action.instanceId, std::move(audioRequest));
            if (!start)
            {
                (void)taskBroker_->Complete(action.id, false,
                    start.error.empty()
                        ? "taskExecutorUnavailable" : start.error);
            }
            continue;
        }
        snowdesktop::widget_runtime::WidgetMediaTaskRequest mediaRequest;
        mediaRequest.action = action.name;
        if (const auto value = action.arguments.find("sessionId");
            value != action.arguments.end())
            mediaRequest.sessionId = value->second;
        bool validMediaRequest = true;
        if (action.name == "media.seek")
        {
            const auto value = action.arguments.find("positionMs");
            std::int64_t position = 0;
            if (value == action.arguments.end())
                validMediaRequest = false;
            else
            {
                const char* begin = value->second.data();
                const char* end = begin + value->second.size();
                const auto parsed = std::from_chars(begin, end, position);
                validMediaRequest = parsed.ec == std::errc{} &&
                    parsed.ptr == end;
                if (validMediaRequest) mediaRequest.positionMs = position;
            }
        }
        else if (action.name == "media.setRate")
        {
            const auto value = action.arguments.find("rate");
            double rate = 0.0;
            if (value == action.arguments.end())
                validMediaRequest = false;
            else
            {
                const char* begin = value->second.data();
                const char* end = begin + value->second.size();
                const auto parsed = std::from_chars(begin, end, rate,
                    std::chars_format::general);
                validMediaRequest = parsed.ec == std::errc{} &&
                    parsed.ptr == end;
                if (validMediaRequest) mediaRequest.rate = rate;
            }
        }
        else if (action.name == "media.setShuffle")
        {
            const auto value = action.arguments.find("shuffle");
            validMediaRequest = value != action.arguments.end() &&
                (value->second == "0" || value->second == "1");
            if (validMediaRequest)
                mediaRequest.shuffle = value->second == "1";
        }
        else if (action.name == "media.setRepeat")
        {
            const auto value = action.arguments.find("mode");
            validMediaRequest = value != action.arguments.end();
            if (validMediaRequest) mediaRequest.repeatMode = value->second;
        }
        validMediaRequest = validMediaRequest &&
            snowdesktop::widget_runtime::WidgetMediaTaskExecutor::
                ValidateRequest(mediaRequest);
        if (!validMediaRequest)
        {
            (void)taskBroker_->Complete(
                action.id, false, "invalidArguments");
            continue;
        }
        if (!mediaTaskExecutor_ ||
            !mediaTaskExecutor_->Start(
                action.id, std::move(mediaRequest)))
        {
            (void)taskBroker_->Complete(
                action.id, false, "taskExecutorUnavailable");
        }
    }

    for (auto& completion : taskBroker_->DrainCompletions())
    {
        snowdesktop::performance::Scope performanceScope(
            "task.completion", completion.name,
            snowdesktop::performance::Enabled()
                ? Utf8ToWide(completion.instanceId) : std::wstring{}, completion.id);
        auto widget = std::find_if(widgets_.begin(), widgets_.end(),
            [&completion](const LuaWidget& candidate) {
                return candidate.runtimeToken == completion.ownerToken;
            });
        const auto nativePayload = nativeTaskDispatch_.TakeCompletion(completion, [&] {
            return widget != widgets_.end() && widget->permissions.contains("native.execute");
        });
        auto payloads = taskResults_.Take(completion.id);
        const auto& searchCompletion = payloads.appSearch;
        const auto& itemSearchCompletion = payloads.itemSearch;
        const auto& calendarCompletion = payloads.calendar;
        const auto& notificationCompletion = payloads.notification;
        const auto& locationCompletion = payloads.location;
        if (widget == widgets_.end() ||
            widget->taskIds.erase(completion.id) == 0)
        {
            networkTaskDispatch_.DiscardCompletion(completion.id);
            clipboardTaskDispatch_.DiscardCompletion(completion.id);
            filesystemTaskDispatch_.DiscardCompletion(completion.id);
            continue;
        }
        if (completion.ok && completion.name == "app.search" &&
            !searchCompletion)
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool calendarTask =
            completion.name == "calendar.create" ||
            completion.name == "calendar.update" ||
            completion.name == "calendar.remove" ||
            completion.name == "calendar.series.create" ||
            completion.name == "calendar.series.update" ||
            completion.name == "calendar.series.remove";
        if (calendarTask &&
            !calendarCompletion)
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool locationTask = completion.name == "location.current";
        const auto* locationResult = locationCompletion.has_value() ? &*locationCompletion : nullptr;
        if (completion.ok && locationTask && (!locationResult ||
            !snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(widget->permissions, "location.read")))
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool networkTask = completion.name == "network.request";
        const bool notificationIdTask =
            completion.name == "notification.show" ||
            completion.name == "notification.schedule";
        if (completion.ok && notificationIdTask &&
            !notificationCompletion)
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        auto clipboardPayload = clipboardTaskDispatch_.TakeCompletion(completion);
        auto filesystemPayload = filesystemTaskDispatch_.TakeCompletion(completion);
        const bool itemSearchTask =
            completion.name == "desktop.search" ||
            completion.name == "everything.search";
        if (completion.ok && itemSearchTask &&
            !itemSearchCompletion)
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }

        struct PublicAppSearchItem
        {
            std::string reference;
            std::string title;
            std::string source;
            std::string type;
        };
        std::vector<PublicAppSearchItem> publicItems;
        std::size_t nextOffset = 0;
        bool hasMore = false;
        std::uint64_t catalogRevision = 0;
        if (completion.ok && completion.name == "app.search")
        {
            constexpr std::size_t MaximumReferences = 2048;
            catalogRevision = searchCompletion->catalogRevision;
            std::erase_if(widget->applicationReferences,
                [catalogRevision](const auto& entry) {
                    return entry.second.catalogRevision != catalogRevision;
                });
            if (widget->applicationReferences.size() +
                    searchCompletion->items.size() >
                MaximumReferences)
            {
                widget->applicationReferences.clear();
            }
            publicItems.reserve(searchCompletion->items.size());
            for (const auto& item : searchCompletion->items)
            {
                const std::string baseReference =
                    snowdesktop::widget_runtime::MakeWidgetAppReference(
                        item.id);
                if (baseReference.empty()) continue;
                std::string reference = baseReference;
                for (std::size_t suffix = 1;; ++suffix)
                {
                    const auto collision =
                        widget->applicationReferences.find(reference);
                    if (collision == widget->applicationReferences.end() ||
                        collision->second.catalogId == item.id)
                        break;
                    reference = baseReference + ":" +
                        std::to_string(suffix);
                }
                widget->applicationReferences.insert_or_assign(reference,
                    LuaWidget::ApplicationReference{ item.id,
                        item.launchTarget, catalogRevision, item.title,
                        item.source, item.type, false });
                publicItems.push_back({ std::move(reference), item.title,
                    item.source, item.type });
            }
            nextOffset = searchCompletion->nextOffset;
            hasMore = searchCompletion->hasMore;
        }
        else if (completion.ok && itemSearchTask)
        {
            constexpr std::size_t MaximumReferences = 4096;
            catalogRevision =
                itemSearchCompletion->catalogRevision;
            if (completion.name == "desktop.search")
            {
                std::erase_if(widget->itemReferences,
                    [&completion, catalogRevision](const auto& entry) {
                        return entry.second.sourceTask == completion.name &&
                            entry.second.revision != catalogRevision;
                    });
            }
            if (widget->itemReferences.size() +
                    itemSearchCompletion->items.size() >
                MaximumReferences)
            {
                widget->itemReferences.clear();
            }
            publicItems.reserve(
                itemSearchCompletion->items.size());
            for (const auto& item :
                itemSearchCompletion->items)
            {
                const std::string baseReference =
                    snowdesktop::widget_runtime::MakeWidgetItemReference(
                        completion.name + ":" +
                            std::to_string(widget->runtimeToken),
                        item.id);
                if (baseReference.empty()) continue;
                std::string reference = baseReference;
                for (std::size_t suffix = 1;; ++suffix)
                {
                    const auto collision =
                        widget->itemReferences.find(reference);
                    if (collision == widget->itemReferences.end() ||
                        (collision->second.sourceTask == completion.name &&
                            collision->second.target == item.launchTarget))
                        break;
                    reference = baseReference + ":" +
                        std::to_string(suffix);
                }
                widget->itemReferences.insert_or_assign(reference,
                    LuaWidget::ItemReference{ item.launchTarget,
                        completion.name, catalogRevision, item.title,
                        item.source, item.type,
                        completion.name == "desktop.search"
                            ? "desktop.item" : "filesystem.reference",
                        false });
                publicItems.push_back({ std::move(reference), item.title,
                    item.source, item.type });
            }
            nextOffset = itemSearchCompletion->nextOffset;
            hasMore = itemSearchCompletion->hasMore &&
                nextOffset <= 100;
        }
        const snowdesktop::calendar::MutationResult* calendarResult =
            calendarCompletion.has_value()
            ? &*calendarCompletion : nullptr;
        const auto networkResult = networkTaskDispatch_.TakeCompletion(completion);
        if (clipboardPayload.HasValue())
        {
            clipboardPayload.PrepareValue(completion,
                [this, &widget](const std::string& token,
                    const std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels>& pixels) {
                    return RegisterTaskImage(widget->widgetId, token, pixels, "clipboard");
                },
                [&widget, &completion](std::size_t count,
                    std::span<const snowdesktop::widget_runtime::ClipboardFileItem> files) {
                    constexpr std::size_t MaximumReferences = 4096;
                    std::erase_if(widget->itemReferences, [](const auto& entry) {
                        return entry.second.sourceTask == "clipboard.read";
                    });
                    if (widget->itemReferences.size() + count > MaximumReferences)
                        widget->itemReferences.clear();
                    for (const auto& file : files)
                    {
                        widget->itemReferences.insert_or_assign(file.reference,
                            LuaWidget::ItemReference{file.target, "clipboard.read",
                                completion.id, file.name, "clipboard",
                                file.folder ? "folder" : "file", "filesystem.reference", false});
                    }
                });
        }
        filesystemPayload.PrepareValue(completion, [this, &widget](const std::string& token,
            const std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels>& pixels) {
            return RegisterTaskImage(widget->widgetId, token, pixels, "filesystem");
        });
        snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
            trustedGestureState_, false);
        (void)InvokeLifecycleEvent(*widget, "task.complete",
            [&completion, &publicItems, &clipboardPayload,
                nextOffset, hasMore,
                catalogRevision, calendarTask, itemSearchTask,
                calendarResult, notificationIdTask,
                notificationCompletion, locationTask, locationResult, networkTask,
                &networkResult, &filesystemPayload, &nativePayload](lua_State* eventState) {
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(completion.id));
                lua_setfield(eventState, -2, "taskId");
                lua_pushlstring(eventState, completion.name.data(),
                    completion.name.size());
                lua_setfield(eventState, -2, "task");
                lua_pushboolean(eventState, completion.ok ? 1 : 0);
                lua_setfield(eventState, -2, "ok");
                if (const auto* diagnostic = nativePayload.Diagnostic())
                {
                    PushSystemControlJson(eventState, *diagnostic);
                    lua_setfield(eventState, -2, "diagnostic");
                }
                if (completion.ok)
                {
                    if (const auto* value = nativePayload.Value())
                        PushSystemControlJson(eventState, *value);
                    else if (completion.name == "audio.play")
                        snowdesktop::widget_runtime::PushAudioPlaybackTaskValue(
                            eventState, completion.id);
                    else if (completion.name == "app.search" || itemSearchTask)
                    {
                        lua_createtable(eventState, 0, 4);
                        lua_createtable(eventState,
                            static_cast<int>(publicItems.size()), 0);
                        int index = 1;
                        for (const auto& item : publicItems)
                        {
                            lua_createtable(eventState, 0, 4);
                            lua_pushlstring(eventState,
                                item.reference.data(), item.reference.size());
                            lua_setfield(eventState, -2, "ref");
                            lua_pushlstring(eventState,
                                item.title.data(), item.title.size());
                            lua_setfield(eventState, -2, "title");
                            lua_pushlstring(eventState,
                                item.source.data(), item.source.size());
                            lua_setfield(eventState, -2, "source");
                            lua_pushlstring(eventState,
                                item.type.data(), item.type.size());
                            lua_setfield(eventState, -2, "type");
                            lua_rawseti(eventState, -2, index++);
                        }
                        lua_setfield(eventState, -2, "items");
                        lua_pushinteger(eventState,
                            static_cast<lua_Integer>(nextOffset));
                        lua_setfield(eventState, -2, "nextOffset");
                        lua_pushboolean(eventState, hasMore ? 1 : 0);
                        lua_setfield(eventState, -2, "hasMore");
                        lua_pushinteger(eventState,
                            static_cast<lua_Integer>(catalogRevision));
                        lua_setfield(eventState, -2,
                            itemSearchTask ? "revision" :
                                "catalogRevision");
                    }
                    else if (calendarTask)
                    {
                        lua_createtable(eventState, 0, 2);
                        lua_pushlstring(eventState,
                            calendarResult->id.data(),
                            calendarResult->id.size());
                        lua_setfield(eventState, -2, "id");
                        lua_pushinteger(eventState,
                            calendarResult->revision);
                        lua_setfield(eventState, -2, "revision");
                    }
                    else if (notificationIdTask)
                    {
                        lua_createtable(eventState, 0, 1);
                        lua_pushlstring(eventState,
                            notificationCompletion->data(),
                            notificationCompletion->size());
                        lua_setfield(eventState, -2,
                            "notificationId");
                    }
                    else if (locationTask)
                    {
                        lua_createtable(eventState, 0, 6);
                        lua_pushnumber(eventState, locationResult->latitude); lua_setfield(eventState, -2, "latitude");
                        lua_pushnumber(eventState, locationResult->longitude); lua_setfield(eventState, -2, "longitude");
                        lua_pushnumber(eventState, locationResult->accuracyMeters); lua_setfield(eventState, -2, "accuracyMeters");
                        lua_pushinteger(eventState, locationResult->timestampMs); lua_setfield(eventState, -2, "timestampMs");
                        lua_pushlstring(eventState, locationResult->source.data(), locationResult->source.size());
                        lua_setfield(eventState, -2, "source");
                    }
                    else if (networkTask)
                        snowdesktop::widget_runtime::PushNetworkTaskValue(eventState, *networkResult);
                    else if (clipboardPayload.HasValue())
                        clipboardPayload.PushValue(eventState);
                    else if (filesystemPayload.HasValue())
                        filesystemPayload.PushValue(eventState, completion.name);
                    else
                    {
                        lua_createtable(eventState, 0, 1);
                        lua_pushboolean(eventState, 1);
                        lua_setfield(eventState, -2, "accepted");
                    }
                    lua_setfield(eventState, -2, "value");
                }
                else
                {
                    lua_pushlstring(eventState, completion.error.data(),
                        completion.error.size());
                    lua_setfield(eventState, -2, "error");
                    if (calendarTask && calendarResult &&
                        calendarResult->revision > 0)
                    {
                        lua_pushinteger(eventState,
                            calendarResult->revision);
                        lua_setfield(eventState, -2, "currentRevision");
                    }
                    if (networkTask && networkResult)
                        snowdesktop::widget_runtime::PushNetworkTaskStatus(eventState, *networkResult);
                }
            });
    }
    for (const auto& message : audioEvents)
    {
        auto owner = std::find_if(widgets_.begin(), widgets_.end(),
            [&message](const auto& candidate) {
                return candidate.runtimeToken == message.owner;
            });
        if (owner == widgets_.end() || !owner->permissions.contains("audio.playback"))
            continue;
        snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
            trustedGestureState_, false);
        (void)InvokeLifecycleEvent(*owner,
            message.command.command.empty() ? "audio.state" : "audio.command",
            [this, &owner, &message](lua_State* state) {
                snowdesktop::widget_runtime::PushAudioPlaybackEvent(state, message,
                    [this, &owner](const std::string& token, const auto& pixels) {
                        return RegisterTaskImage(owner->widgetId, token, pixels, "audio");
                    });
            });
    }

}

void WidgetEngine::CancelDispatchedWidgetTask(
    std::uint64_t taskId, bool completeLocation)
{
    if (nativeTaskExecutor_) (void)nativeTaskExecutor_->Cancel(taskId);
    nativeTaskDispatch_.DiscardCompletion(taskId);
    CancelWidgetTaskWork(taskId, completeLocation);
}

void WidgetEngine::CancelWidgetTaskWork(
    std::uint64_t taskId, bool completeLocation)
{
    if (locationTaskExecutor_) (void)locationTaskExecutor_->Cancel(taskId);
    taskResults_.DiscardLocation(taskId);
    if (completeLocation) (void)taskBroker_->Complete(taskId, false);
    if (systemControlTasks_) (void)systemControlTasks_->Cancel(taskId);
    if (mediaTaskExecutor_)
        (void)mediaTaskExecutor_->Cancel(taskId);
    if (audioOutputTaskExecutor_)
        (void)audioOutputTaskExecutor_->Cancel(taskId);
    if (clipboardTaskExecutor_)
        (void)clipboardTaskExecutor_->Cancel(taskId);
    if (filesystemTaskExecutor_)
        (void)filesystemTaskExecutor_->Cancel(taskId);
    if (appTaskExecutor_)
        (void)appTaskExecutor_->Cancel(taskId);
    if (desktopTaskExecutor_)
        (void)desktopTaskExecutor_->Cancel(taskId);
    if (externalItemTaskExecutor_)
        (void)externalItemTaskExecutor_->Cancel(taskId);
    taskResults_.Discard(taskId);
    clipboardTaskDispatch_.DiscardCompletion(taskId);
    filesystemTaskDispatch_.DiscardCompletion(taskId);
}

void WidgetEngine::ReleaseWidgetTasks(LuaWidget& widget,
    snowdesktop::widget_runtime::TaskBrokerCancelReason reason)
{
    ForgetWidgetMultiplayer(widget.runtimeToken);
    if (audioPlayback_) audioPlayback_->ForgetOwner(widget.runtimeToken);
    if (notificationCenter_)
        notificationCenter_->RemoveOwner(
            widget.runtimeToken, notifyCallback_);
    if (!taskBroker_)
    {
        widget.taskIds.clear();
        return;
    }
    for (const std::uint64_t taskId : widget.taskIds)
    {
        (void)taskBroker_->Cancel(taskId, reason);
        CancelDispatchedWidgetTask(taskId, false);
        networkTaskDispatch_.DiscardCompletion(taskId);
    }
    if (audioOutputTaskExecutor_)
        audioOutputTaskExecutor_->ForgetInstance(
            WideToUtf8(widget.widgetId));
    if (clipboardTaskExecutor_)
        clipboardTaskExecutor_->ForgetInstance(
            WideToUtf8(widget.widgetId));
    if (filesystemTaskExecutor_)
        filesystemTaskExecutor_->ForgetInstance(
            WideToUtf8(widget.widgetId));
    widget.taskIds.clear();
}

namespace snowdesktop::widget_runtime
{
static bool IsValidUtf8Local(const std::string& value)
{
    if (value.empty()) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0) > 0;
}

int lua_TaskStart(lua_State* state)
{
    using snowdesktop::widget_runtime::LuaTaskStartHost;
    static constexpr LuaTaskStartHost host{
        [](lua_State* taskState, const std::string& name,
            LuaTaskStartHost::Arguments&& arguments)
            -> std::optional<snowdesktop::widget_runtime::TaskStartResult> {
            auto* engine = BoundWidgetEngine(taskState);
            if (!engine) return std::nullopt;
            return engine->RuntimeStartTask(
                BoundWidgetId(taskState), BoundWidgetRuntimeToken(taskState),
                name, std::move(arguments));
        },
        [](lua_State* taskState, int index) {
            return ResolveResourceHandlePath(
                taskState, index, LuaResourceType::Image).has_value();
        },
        IsValidUtf8Local,
    };
    return snowdesktop::widget_runtime::StartTaskFromLua(state, host);
}

int lua_TaskCancel(lua_State* state)
{
    if (lua_gettop(state) != 1)
        return luaL_error(state, "task.cancel: expected one task ID");
    const lua_Integer id = luaL_checkinteger(state, 1);
    if (id <= 0)
        return luaL_error(state, "task.cancel: task ID must be positive");
    auto* host = BoundWidgetEngine(state);
    lua_pushboolean(state, host &&
        host->RuntimeCancelTask(
            BoundWidgetId(state), BoundWidgetRuntimeToken(state),
            static_cast<std::uint64_t>(id)));
    return 1;
}

int lua_HttpRequest(lua_State* L)
{
    if (!RequirePermission(L, "network.http")) return 0;
    luaL_checktype(L, 1, LUA_TTABLE);
    auto* host = BoundWidgetEngine(L);
    if (!host) { lua_pushnil(L); return 1; }

    HttpRequestOptions options;
    options.widgetId = BoundWidgetId(L);
    lua_getfield(L, 1, "url");
    options.url = Utf8ToWide(luaL_checkstring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "method");
    if (lua_isstring(L, -1)) options.method = Utf8ToWide(lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "body");
    if (lua_isstring(L, -1)) options.body = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "timeoutMs");
    if (lua_isinteger(L, -1)) options.timeoutMs = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "cacheSeconds");
    if (lua_isinteger(L, -1)) options.cacheSeconds = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1))
    {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0)
        {
            if (lua_isstring(L, -2) && lua_isstring(L, -1))
            {
                options.headers += Utf8ToWide(lua_tostring(L, -2));
                options.headers += L": ";
                options.headers += Utf8ToWide(lua_tostring(L, -1));
                options.headers += L"\r\n";
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    int id = host->RuntimeHttpRequest(BoundWidgetId(L), std::move(options));
    if (id > 0) lua_pushinteger(L, id); else lua_pushnil(L);
    return 1;
}

int lua_HttpCancel(lua_State* L)
{
    if (!RequirePermission(L, "network.http")) return 0;
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    auto* host = BoundWidgetEngine(L);
    lua_pushboolean(L, host &&
        host->RuntimeHttpCancel(BoundWidgetId(L), id));
    return 1;
}
}

snowdesktop::widget_runtime::TaskStartResult
WidgetEngine::RuntimeStartTask(
    const std::wstring& widgetId, std::uint64_t ownerToken,
    std::string name,
    std::unordered_map<std::string, std::string> arguments)
{
    using snowdesktop::widget_runtime::TaskStartOptions;
    if (name.starts_with("multiplayer.") && snowdesktop::widget_runtime::IsDryLoad())
        return {0, "previewUnavailable"};
    if ((name == "native.request" || name == "native.watch") && snowdesktop::widget_runtime::IsDryLoad())
        return { 0, "previewUnavailable" };
    if (!taskBroker_)
        return { 0, "task broker is not initialized" };
    const auto loaded = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (loaded == widgets_.end())
        return { 0, "widget instance is not loaded" };

    LuaWidget& widget = *loaded;
    if (name == "filesystem.pickFolder")
    {
        const auto access = arguments.find("access");
        if (access == arguments.end() ||
            (access->second != "read" && access->second != "write" &&
                access->second != "readWrite"))
            return { 0, "invalidArguments" };
        const bool needsRead = access->second != "write";
        const bool needsWrite = access->second != "read";
        if ((needsRead && !snowdesktop::widget::WidgetPermissionBroker::
                AllowsPermission(widget.permissions,
                    kFilesystemReadPermission)) ||
            (needsWrite && !snowdesktop::widget::WidgetPermissionBroker::
                AllowsPermission(widget.permissions,
                    kFilesystemWritePermission)))
            return { 0, "permissionDenied" };
    }
    else if (IsFilesystemHandleTask(name))
    {
        const auto handle = arguments.find("handle");
        if (handle == arguments.end()) return { 0, "invalidArguments" };
        const bool previewHandle = widget.preview &&
            IsPreviewFilesystemHandle(handle->second);
        std::optional<snowdesktop::widget_runtime::
            WidgetFilesystemHandleEntry> entry;
        if (!previewHandle)
        {
            if (!filesystemHandleStore_)
                return { 0, "providerUnavailable" };
            entry = filesystemHandleStore_->Resolve(
                { WideToUtf8(widget.widgetId), widget.packageId },
                handle->second);
            if (!entry) return { 0, "invalidReference" };
        }
        if (entry)
        {
            const bool readable = entry->access ==
                    snowdesktop::widget_runtime::
                        WidgetFilesystemHandleAccess::Read ||
                entry->access == snowdesktop::widget_runtime::
                    WidgetFilesystemHandleAccess::ReadWrite;
            const bool writable = entry->access ==
                    snowdesktop::widget_runtime::
                        WidgetFilesystemHandleAccess::Write ||
                entry->access == snowdesktop::widget_runtime::
                    WidgetFilesystemHandleAccess::ReadWrite;
            if ((name == "filesystem.stat" ||
                    name == "filesystem.list" ||
                    name == "filesystem.read" ||
                    name == "filesystem.image") && !readable)
                return { 0, "handleAccessDenied" };
            if (name == "filesystem.write" && !writable)
                return { 0, "handleAccessDenied" };
            if (name == "filesystem.list" && entry->kind !=
                    snowdesktop::widget_runtime::
                        WidgetFilesystemHandleKind::Folder)
                return { 0, "notFolder" };
            if (name == "filesystem.image")
            {
                const auto child = arguments.find("name");
                const bool hasChild = child != arguments.end();
                if (hasChild && !snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor::IsDirectChildName(child->second))
                    return { 0, "invalidArguments" };
                const auto expected = hasChild ? snowdesktop::widget_runtime::WidgetFilesystemHandleKind::Folder :
                    snowdesktop::widget_runtime::WidgetFilesystemHandleKind::File;
                if (entry->kind != expected) return { 0, hasChild ? "notFolder" : "notFile" };
            }
            if ((name == "filesystem.read" ||
                    name == "filesystem.write") && entry->kind !=
                    snowdesktop::widget_runtime::
                        WidgetFilesystemHandleKind::File)
                return { 0, "notFile" };
        }
    }
    TaskStartOptions options;
    options.ownerToken = widget.runtimeToken;
    const auto requiredPermission =
        taskBroker_->RequiredPermission(name);
    options.permissionGranted = requiredPermission &&
        (requiredPermission->empty() ||
            snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(
                widget.permissions, *requiredPermission));
    options.trustedGesture = trustedGestureState_.Active();
    options.preview = widget.preview;
    options.arguments = std::move(arguments);
    if (name == "audio.output.setVolume" && options.arguments.contains("coalesce") &&
        options.arguments.at("coalesce") == "1" && options.permissionGranted &&
        options.trustedGesture)
    {
        if (widget.taskIds.size() >= snowdesktop::widget_runtime::WidgetTaskBroker::MaximumTasks)
            return { 0, "per-instance task limit exceeded" };
        // Retire superseded tasks before quota checks. Keep unique task IDs,
        // cancellation notifications, instance ownership and gesture checks.
        for (const auto id : widget.taskIds)
        {
            const auto previous = taskBroker_->Snapshot(id);
            if (previous && previous->ownerToken == ownerToken && previous->name == name)
            {
                (void)RuntimeCancelTask(widgetId, ownerToken, id);
                (void)taskBroker_->Complete(id, false, "canceled");
            }
        }
    }
    auto result = taskBroker_->Start(
        WideToUtf8(widgetId), name, options);
    if (result)
    {
        snowdesktop::performance::Value("task.start", name, widgetId, 1, result.id);
        widget.taskIds.insert(result.id);
        if (!widget.preview && taskWakeCallback_) taskWakeCallback_();
    }
    return result;
}

bool WidgetEngine::RuntimeCancelTask(
    const std::wstring& widgetId, std::uint64_t ownerToken,
    std::uint64_t taskId)
{
    if (!taskBroker_ || taskId == 0) return false;
    const auto loaded = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (loaded == widgets_.end()) return false;
    LuaWidget& widget = *loaded;
    if (!widget.taskIds.contains(taskId)) return false;
    const auto snapshot = taskBroker_->Snapshot(taskId);
    if (!snapshot || snapshot->ownerToken != widget.runtimeToken)
        return false;
    if (!taskBroker_->Cancel(taskId)) return false;
    snowdesktop::widget_runtime::CancelAudioPlaybackTask(
        audioPlayback_.get(), snapshot->name, taskId);
    CancelWidgetTaskWork(taskId, false);
    networkTaskDispatch_.DiscardCompletion(taskId);
    if (!widget.preview && taskWakeCallback_) taskWakeCallback_();
    return true;
}

int WidgetEngine::RuntimeHttpRequest(const std::wstring& widgetId, HttpRequestOptions options)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return 0;
    int index = FindWidget(widgetId);
    if (index < 0 || !httpService_) return 0;
    options.widgetId = widgetId;
    options.allowAnyHttpOrHttpsUrl = true;
    options.timeoutMs = std::clamp(options.timeoutMs, 1000, 30000);
    options.cacheSeconds = std::clamp(options.cacheSeconds, 0, 86400);
    if (options.body.size() > 64 * 1024) return 0;
    return httpService_->Submit(std::move(options));
}

bool WidgetEngine::RuntimeHttpCancel(const std::wstring& widgetId, int requestId)
{
    return httpService_ && httpService_->Cancel(widgetId, requestId);
}

void WidgetEngine::RuntimeNotify(const std::wstring& widgetId,
    const std::wstring& title, const std::wstring& message)
{
    const std::string error = RuntimePostNotification(
        widgetId, title, message);
    if (!error.empty())
        RuntimeRecordError(widgetId,
            "Widget notification failed: " + error);
}

std::string WidgetEngine::RuntimePostNotification(
    const std::wstring& widgetId, const std::wstring& title,
    const std::wstring& message)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return {};
    if (title.empty() || message.empty()) return "invalidArguments";
    const std::string admissionError =
        RuntimeAdmitNotification(widgetId);
    if (!admissionError.empty()) return admissionError;
    try
    {
        if (!notifyCallback_({ snowdesktop::widget_runtime::
                WidgetNotificationHostOperation::Show,
                {}, title, message }))
            return "notificationFailed";
    }
    catch (...)
    {
        return "notificationFailed";
    }
    return {};
}

std::string WidgetEngine::RuntimeAdmitNotification(
    const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return {};
    const int index = FindWidget(widgetId);
    if (index < 0) return "instanceDisposed";
    auto& widget = widgets_[index];
    const auto now = std::chrono::steady_clock::now();
    if (widget.notificationWindow.time_since_epoch().count() == 0 ||
        now - widget.notificationWindow >= std::chrono::minutes(1))
    {
        widget.notificationWindow = now;
        widget.notificationsInWindow = 0;
    }
    if (widget.notificationsInWindow >= 5)
        return "quotaExceeded";
    if (!notifyCallback_) return "providerUnavailable";
    ++widget.notificationsInWindow;
    return {};
}

void WidgetEngine::OnNotificationAction(
    std::string_view notificationId, std::string_view actionId)
{
    if (!notificationCenter_) return;
    const auto activation = notificationCenter_->Activate(
        notificationId, actionId);
    if (!activation.ok) return;
    const auto owner = std::find_if(widgets_.begin(), widgets_.end(),
        [&activation](const LuaWidget& widget) {
            return widget.runtimeToken == activation.ownerToken &&
                widget.valid && !widget.preview;
        });
    if (owner == widgets_.end()) return;

    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, true);
    (void)InvokeLifecycleEvent(*owner, "notification.action",
        [&activation](lua_State* eventState) {
            lua_pushlstring(eventState,
                activation.notificationId.data(),
                activation.notificationId.size());
            lua_setfield(eventState, -2, "notificationId");
            lua_pushlstring(eventState, activation.actionId.data(),
                activation.actionId.size());
            lua_setfield(eventState, -2, "actionId");
        });
    RuntimeInvalidateHost(owner->widgetId);
}

void WidgetEngine::SetTaskWakeCallback(TaskWakeCallback callback)
{
    taskWakeCallback_ = std::move(callback);
    if (d2dState_) d2dState_->packageImageCache.SetReadyCallback(taskWakeCallback_);
    if (audioPlayback_) audioPlayback_->SetWakeCallback(taskWakeCallback_);
    if (nativeTaskExecutor_) nativeTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
    if (locationTaskExecutor_) locationTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
    if (filesystemTaskExecutor_)
        filesystemTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
}

void WidgetEngine::OnTaskWake()
{
    DrainPackageImageChanges();
    TickWidgetDataBroker();
    ApplyWidgetTaskBrokerActions();
}

void WidgetEngine::TickWidgetNotifications()
{
    if (notificationCenter_ && notificationScheduleStore_)
    {
        const auto notificationNow = snowdesktop::widget_runtime::
            WidgetNotificationCenter::Clock::now();
        const std::int64_t notificationNowMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                notificationNow.time_since_epoch()).count();
        std::vector<snowdesktop::widget_runtime::
            WidgetPersistedNotificationSchedule> durableDue;
        for (const auto& entry :
            notificationScheduleStore_->Due(notificationNowMs))
        {
            const bool loaded = std::any_of(
                widgets_.begin(), widgets_.end(),
                [&entry](const LuaWidget& widget) {
                    return widget.valid && !widget.preview &&
                        WideToUtf8(widget.widgetId) ==
                            entry.instanceId &&
                        widget.packageId == entry.packageId;
                });
            if (loaded) durableDue.push_back(entry);
        }
        bool dispatchDue = !durableDue.empty();
        if (dispatchDue)
        {
            for (const auto& entry : durableDue)
            {
                (void)notificationScheduleStore_->Remove(
                    entry.instanceId, entry.notificationId);
            }
            if (!SaveNotificationSchedules())
            {
                std::string ignored;
                for (const auto& entry : durableDue)
                    (void)notificationScheduleStore_->Upsert(entry, ignored);
                dispatchDue = false;
            }
        }
        const auto deliveries = dispatchDue
            ? notificationCenter_->DispatchDue(notificationNow,
            [this](std::uint64_t ownerToken) {
                const auto owner = std::find_if(
                    widgets_.begin(), widgets_.end(),
                    [ownerToken](const LuaWidget& widget) {
                        return widget.runtimeToken == ownerToken &&
                            widget.valid && !widget.preview;
                    });
                return owner == widgets_.end()
                    ? std::string("instanceDisposed")
                    : RuntimeAdmitNotification(owner->widgetId);
            },
            [this](const snowdesktop::widget_runtime::
                WidgetNotificationHostRequest& request) {
                return notifyCallback_ && notifyCallback_(request);
            })
            : std::vector<snowdesktop::widget_runtime::
                WidgetNotificationDelivery>{};
        for (const auto& delivery : deliveries)
        {
            const auto owner = std::find_if(
                widgets_.begin(), widgets_.end(),
                [&delivery](const LuaWidget& widget) {
                    return widget.runtimeToken == delivery.ownerToken;
                });
            if (owner == widgets_.end() || !owner->valid) continue;
            (void)InvokeLifecycleEvent(*owner, "notification.delivered",
                [&delivery](lua_State* eventState) {
                    lua_pushlstring(eventState, delivery.id.data(),
                        delivery.id.size());
                    lua_setfield(eventState, -2, "notificationId");
                    lua_pushboolean(eventState, delivery.ok ? 1 : 0);
                    lua_setfield(eventState, -2, "ok");
                    if (!delivery.ok)
                    {
                        lua_pushlstring(eventState, delivery.error.data(),
                            delivery.error.size());
                        lua_setfield(eventState, -2, "error");
                    }
                });
            RuntimeInvalidateHost(owner->widgetId);
        }
    }
}

void WidgetEngine::DrainWidgetHttpResponses()
{
    if (httpService_)
    {
        for (auto& response : httpService_->Drain())
        {
            if (networkTaskDispatch_.Consume(response, taskBroker_.get()))
                continue;
            int index = FindWidget(response.widgetId);
            if (index < 0) continue;
            auto& widget = widgets_[index];
            lua_State* state = widget.state;
            if (!state) continue;
            const std::wstring widgetId = widget.widgetId;
            WidgetExecutionContextGuard contextGuard(
                d2dState_, widgetId);
            snowdesktop::lua_runtime::StackGuard stackGuard(state);
            SetWidgetRectContext(d2dState_, widget.lastBounds);
            lua_rawgeti(state, LUA_REGISTRYINDEX, widget.ref);
            if (lua_istable(state, -1))
            {
                lua_getfield(state, -1, "onHttpResponse");
                if (lua_isfunction(state, -1))
                {
                    lua_pushinteger(state, response.id);
                    snowdesktop::widget_runtime::PushHttpResponseValue(state, response);
                    if (snowdesktop::lua_runtime::ProtectedCall(state, 2, 0) != LUA_OK)
                    {
                        const char* error = lua_tostring(state, -1);
                        RuntimeRecordError(widgetId, error ? error : "(onHttpResponse error)");
                        lua_pop(state, 1);
                    }
                    RuntimeInvalidateHost(widgetId);
                }
                else
                    lua_pop(state, 1);
            }
            lua_pop(state, 1);
        }
    }
}
