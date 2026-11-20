#include "widget/runtime/widget_engine.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/preview/widget_preview_context.h"
#include "widget/permissions/widget_permission_broker.h"
#include "common/utils.h"
#include "common/l10n.h"
#include "diagnostics/performance_trace.h"
#include "widget/tasks/widget_filesystem_task_dispatch.h"
#include "system/system_snapshot.h"
#include "widget/tasks/widget_filesystem_watch_origin.h"
#include "widget/tasks/widget_filesystem_watch_service.h"
#include "widget/api/widget_calendar_query_options.h"
#include "widget/api/widget_api_registry.h"
#include "widget/data/widget_clipboard_observer.h"
#include "widget/data/widget_resource_lua.h"
#include "widget/data/widget_data_snapshot.h"
#include "widget/data/widget_system_control_data.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

using snowdesktop::widget_runtime::IsPreviewFilesystemHandle;
using snowdesktop::widget_runtime::kClipboardChangesTopic;
using snowdesktop::widget_runtime::kClipboardObservePermission;

constexpr char kClipboardObservationImageSource[] = "clipboard.observe";
using snowdesktop::widget_runtime::kFilesystemReadPermission;

static void ClearRuntimeImagesForSource(D2DState* state, std::string_view source)
{
    if (!state || source.empty()) return;
    state->backgroundCache.Clear();
    state->runtimeImageCache.RemoveSource(source);
}

void WidgetEngine::SetSystemDataProvider(std::shared_ptr<
    snowdesktop::widget_runtime::WidgetSystemDataProvider> provider)
{
    if (!dataBroker_ && !previewOnly_)
        widgetSystemDataProvider_ = std::move(provider);
}

void WidgetEngine::SetAudioAnalysisWakeCallback(
    AudioAnalysisWakeCallback callback)
{
    audioAnalysisWakeCallback_ = std::move(callback);
    if (widgetAudioAnalysisProvider_)
    {
        widgetAudioAnalysisProvider_->SetChangedCallback(
            audioAnalysisWakeCallback_);
    }
}

void WidgetEngine::InitializeWidgetDataBroker()
{
    using snowdesktop::widget_runtime::DataProviderDescriptor;

    dataBroker_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetDataBroker>();
    desktopDataRevision_ = 0;
    desktopDataChangeReason_ = "initial";
    desktopDataTimestampMs_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    calendarEventsRevision_ = 0;
    calendarSelectionRevision_ = 0;
    appIndexRevision_ = 0;
    std::string error;
    for (const auto& contract :
        snowdesktop::widget_api::SystemDataTopicContracts())
    {
        (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
            contract.name,
            contract.requiredPermission ? contract.requiredPermission : "",
            std::chrono::milliseconds(contract.minimumIntervalMs),
            std::chrono::milliseconds(contract.hiddenIntervalMs),
            std::chrono::milliseconds(contract.idleGraceMs),
            contract.highRisk,
            contract.supportsHiddenContinue }, error);
        error.clear();
    }

    if (previewOnly_)
    {
        widgetSystemDataProvider_.reset();
        widgetAudioAnalysisProvider_.reset();
    }
    else
    {
        if (!widgetSystemDataProvider_)
            widgetSystemDataProvider_ = std::make_shared<
                snowdesktop::widget_runtime::WidgetSystemDataProvider>();
        widgetAudioAnalysisProvider_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAudioAnalysisProvider>();
        widgetAudioAnalysisProvider_->SetChangedCallback(
            audioAnalysisWakeCallback_);
    }
}

void WidgetEngine::ApplyWidgetDataBrokerActions()
{
    using snowdesktop::widget_runtime::DataBrokerActionType;
    if (!dataBroker_) return;
    const auto audioConfiguration = [this] {
        snowdesktop::widget_runtime::WidgetAudioAnalysisConfiguration
            configuration{ false, false, false, false, 16, 16 };
        for (const auto& subscription :
            dataBroker_->SubscriptionSnapshots("audio.output.analysis"))
        {
            if (!subscription.eligible) continue;
            configuration.waveform = configuration.waveform ||
                subscription.options.audioWaveform;
            configuration.spectrum = configuration.spectrum ||
                subscription.options.audioSpectrum;
            configuration.rms = configuration.rms ||
                subscription.options.audioRms;
            configuration.peak = configuration.peak ||
                subscription.options.audioPeak;
            if (subscription.options.audioWaveform)
            {
                configuration.waveformPoints = std::max(
                    configuration.waveformPoints,
                    subscription.options.audioWaveformPoints);
            }
            if (subscription.options.audioSpectrum)
            {
                configuration.spectrumBins = std::max(
                    configuration.spectrumBins,
                    subscription.options.audioSpectrumBins);
            }
        }
        return configuration;
    };
    for (const auto& action : dataBroker_->DrainActions())
    {
        if (action.topic == kClipboardChangesTopic)
        {
            if (action.type == DataBrokerActionType::Start)
            {
                const bool started = clipboardObserver_.Start(taskWakeCallback_);
                (void)dataBroker_->MarkStarted(action.topic, started,
                    started ? "" : "clipboardListenerUnavailable");
            }
            else if (action.type == DataBrokerActionType::Stop)
            {
                clipboardObserver_.Stop();
            }
            continue;
        }
        const bool audioAnalysis =
            action.topic == "audio.output.analysis";
        const bool hostEventData =
            action.topic == "desktop.items" ||
            action.topic == "desktop.selection" ||
            action.topic == "desktop.changes" ||
            action.topic == "calendar.events" ||
            action.topic == "calendar.selectedDate" ||
            action.topic == "app.indexStatus" ||
            action.topic == "filesystem.watch";
        switch (action.type)
        {
        case DataBrokerActionType::Start:
        {
            const bool started = hostEventData
                ? true
                : audioAnalysis
                ? widgetAudioAnalysisProvider_ &&
                    widgetAudioAnalysisProvider_->Start(
                        action.effectiveInterval,
                        audioConfiguration())
                : widgetSystemDataProvider_ &&
                    widgetSystemDataProvider_->StartTopic(
                        action.topic, action.effectiveInterval);
            (void)dataBroker_->MarkStarted(action.topic, started,
                started ? std::string{} : "data provider failed to start");
            break;
        }
        case DataBrokerActionType::Reconfigure:
            if (hostEventData)
            {
                // Event-backed desktop topics have no polling resource to
                // reconfigure. The broker still owns their lifecycle.
            }
            else if (audioAnalysis)
            {
                if (widgetAudioAnalysisProvider_)
                    (void)widgetAudioAnalysisProvider_->Start(
                        action.effectiveInterval,
                        audioConfiguration());
            }
            else if (widgetSystemDataProvider_)
            {
                (void)widgetSystemDataProvider_->StartTopic(
                    action.topic, action.effectiveInterval);
            }
            break;
        case DataBrokerActionType::Stop:
            if (hostEventData)
            {
                // No worker or OS handle is retained for these topics.
            }
            else if (audioAnalysis)
            {
                if (widgetAudioAnalysisProvider_)
                    widgetAudioAnalysisProvider_->Stop();
            }
            else if (widgetSystemDataProvider_)
            {
                (void)widgetSystemDataProvider_->StopTopic(action.topic);
                if (action.topic == "media.artwork" && d2dState_)
                {
                    ClearRuntimeImagesForSource(d2dState_, "media");
                }
            }
            break;
        }
    }
    ReconcileFilesystemWatches();
}

std::string WidgetEngine::RegisterClipboardObservationImage(
    const std::wstring& widgetId,
    const snowdesktop::widget_runtime::WidgetClipboardChange& change)
{
    if (!d2dState_ || !change.image || change.imageToken.empty()) return {};
    // Keep one-shot read handles independent of the observation cache.
    const auto sourceToken = change.imageToken + ":obs";
    constexpr std::size_t kMaxRetainedImages = 8;
    auto& recent = clipboardObservationImages_[widgetId];
    auto& cache = d2dState_->runtimeImageCache;
    if (recent.size() == kMaxRetainedImages &&
        std::find(recent.begin(), recent.end(), sourceToken) == recent.end())
    {
        recent.erase(recent.begin());
        std::array<std::string_view, kMaxRetainedImages - 1> keep;
        std::copy(recent.begin(), recent.end(), keep.begin());
        cache.Retain(widgetId, kClipboardObservationImageSource, keep);
    }
    auto token = cache.Register(widgetId, sourceToken, change.image,
        kClipboardObservationImageSource, true);
    if (!token.empty())
    {
        std::erase(recent, sourceToken);
        recent.push_back(sourceToken);
    }
    return token;
}

void WidgetEngine::DrainClipboardChanges()
{
    if (!dataBroker_) return;
    const auto changes = clipboardObserver_.Drain();
    if (changes.empty()) return;
    const auto subscribers = dataBroker_->SubscriptionSnapshots(kClipboardChangesTopic);
    for (const auto& change : changes)
    {
        for (const auto& subscriber : subscribers)
        {
            const auto revision = clipboardSubscriptionRevisions_.find(subscriber.id);
            if (revision == clipboardSubscriptionRevisions_.end() ||
                revision->second >= change.revision || !subscriber.eligible)
                continue;
            const std::wstring widgetId = Utf8ToWide(subscriber.instanceId);
            const int index = FindWidget(widgetId);
            if (index < 0 || !widgets_[index].valid ||
                !RuntimeHasPermission(widgetId, kClipboardObservePermission))
                continue;
            const auto state = GetWidgetHostState(widgetId,
                Utf8ToWide(widgets_[index].packageId)).kind;
            using snowdesktop::widget_runtime::WidgetHostStateKind;
            if (state == WidgetHostStateKind::PermissionPending ||
                state == WidgetHostStateKind::PermissionDenied)
            {
                UnloadWidget(widgetId);
                continue;
            }
            if (state != WidgetHostStateKind::Ready) continue;
            const auto imageToken = RegisterClipboardObservationImage(widgetId, change);
            snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
                trustedGestureState_, false);
            (void)InvokeLifecycleEvent(widgets_[index], "data.change",
                [&](lua_State* state) {
                    lua_pushliteral(state, "clipboard.changes");
                    lua_setfield(state, -2, "topic");
                    lua_pushinteger(state, static_cast<lua_Integer>(subscriber.id));
                    lua_setfield(state, -2, "subscriptionId");
                    lua_pushinteger(state, static_cast<lua_Integer>(change.revision));
                    lua_setfield(state, -2, "revision");

                    lua_createtable(state, 0, 4);
                    lua_pushstring(state,
                        change.format.empty() ? "empty" : change.format.c_str());
                    lua_setfield(state, -2, "format");
                    if (!change.text.empty())
                    {
                        lua_pushlstring(state, change.text.data(), change.text.size());
                        lua_setfield(state, -2, "text");
                    }
                    if (!change.rgb.empty())
                    {
                        lua_pushlstring(state, change.rgb.data(), change.rgb.size());
                        lua_setfield(state, -2, "rgb");
                        lua_pushinteger(state, change.width);
                        lua_setfield(state, -2, "width");
                        lua_pushinteger(state, change.height);
                        lua_setfield(state, -2, "height");
                        lua_pushlstring(state, change.imageToken.data(), change.imageToken.size());
                        lua_setfield(state, -2, "imageId");
                    }
                    if (!imageToken.empty())
                    {
                        using namespace snowdesktop::widget_runtime;
                        PushResourceHandle(state, LuaResourceType::Image, imageToken);
                        lua_setfield(state, -2, "image");
                        lua_pushinteger(state, change.image->width);
                        lua_setfield(state, -2, "imageWidth");
                        lua_pushinteger(state, change.image->height);
                        lua_setfield(state, -2, "imageHeight");
                    }
                    if (!change.names.empty())
                    {
                        lua_createtable(state, static_cast<int>(change.names.size()), 0);
                        lua_Integer nameIndex = 1;
                        for (const auto& name : change.names)
                        {
                            lua_pushlstring(state, name.data(), name.size());
                            lua_rawseti(state, -2, nameIndex++);
                        }
                        lua_setfield(state, -2, "names");
                    }
                    if (!change.error.empty())
                    {
                        lua_pushstring(state, change.error.c_str());
                        lua_setfield(state, -2, "error");
                    }
                    lua_setfield(state, -2, "clipboard");
                });
        }
    }
}

void WidgetEngine::DrainAudioAnalysisChanges()
{
    snowdesktop::performance::Scope performanceScope("shared.audio", "deliver");
    if (!widgetAudioAnalysisProvider_ ||
        !widgetAudioAnalysisProvider_->DrainChanged())
    {
        return;
    }
    if (!dataBroker_) return;
    const auto now = snowdesktop::widget_runtime::WidgetDataBroker::Clock::now();
    for (auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview) continue;
        bool due = false;
        for (const auto& [id, topic] : widget.dataSubscriptions)
        {
            if (topic == "audio.output.analysis" &&
                dataBroker_->ConsumeUpdateDue(id, now))
                due = true;
        }
        if (!due || (widget.desktopOccluded && !widget.panelActive)) continue;
        snowdesktop::performance::DrawLink("desktop", widget.widgetId, false);
        // Audio is asynchronous data, so share an existing animation deadline
        // or request one host frame. Pointer invalidation remains synchronous.
        if (widget.animationFrames.RequestDataRefresh() &&
            ScheduleAnimationFrame(widget))
        {
            snowdesktop::performance::Value("shared.audio", "refresh.queued",
                widget.widgetId, 1);
            continue;
        }
        (void)widget.animationFrames.ConsumeDataRefresh();
        RuntimeInvalidateHost(widget.widgetId);
    }
}

void WidgetEngine::ReconcileFilesystemWatches()
{
    if (!dataBroker_) return;
    for (auto& [subscriptionId, watch] : filesystemWatchBindings_)
    {
        if (watch.preview) continue;
        const auto subscription =
            dataBroker_->SubscriptionSnapshot(subscriptionId);
        const bool shouldRun = subscription && subscription->eligible;
        if (shouldRun == watch.desiredActive) continue;

        watch.desiredActive = shouldRun;
        watch.events.clear();
        watch.overflow = false;
        watch.available = false;
        watch.warmingUp = shouldRun;
        watch.error.clear();
        if (!shouldRun)
        {
            if (filesystemWatchService_)
                (void)filesystemWatchService_->Stop(subscriptionId);
            continue;
        }
        if (!filesystemWatchService_)
        {
            watch.warmingUp = false;
            watch.error = "providerUnavailable";
            continue;
        }
        auto started = filesystemWatchService_->Start(subscriptionId,
            watch.owner.instanceId, watch.directory);
        if (!started)
        {
            watch.warmingUp = false;
            watch.error = started.error.empty()
                ? "providerUnavailable" : started.error;
        }
    }
}

void WidgetEngine::DrainFilesystemWatchCompletions()
{
    if (!filesystemWatchService_) return;
    for (auto& completion :
        filesystemWatchService_->DrainCompletions())
    {
        auto binding = filesystemWatchBindings_.find(completion.id);
        if (binding == filesystemWatchBindings_.end()) continue;
        auto& watch = binding->second;
        const auto subscription = dataBroker_
            ? dataBroker_->SubscriptionSnapshot(completion.id)
            : std::nullopt;
        const bool eligible = subscription && subscription->eligible &&
            watch.desiredActive;

        if (completion.kind == snowdesktop::widget_runtime::
                WidgetFilesystemWatchCompletionKind::Started)
        {
            if (!eligible) continue;
            watch.available = true;
            watch.warmingUp = false;
            watch.error.clear();
            watch.timestampMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            continue;
        }
        if (completion.kind == snowdesktop::widget_runtime::
                WidgetFilesystemWatchCompletionKind::Stopped)
        {
            if (!watch.desiredActive)
            {
                watch.available = false;
                watch.warmingUp = false;
                watch.events.clear();
                watch.overflow = false;
            }
            continue;
        }
        if (completion.kind == snowdesktop::widget_runtime::
                WidgetFilesystemWatchCompletionKind::Error)
        {
            if (!eligible) continue;
            watch.available = false;
            watch.warmingUp = false;
            watch.events.clear();
            watch.overflow = false;
            watch.error = completion.error.empty()
                ? "watchFailed" : completion.error;
            continue;
        }
        if (!eligible) continue;

        watch.events.clear();
        watch.overflow = completion.overflow;
        for (const auto& event : completion.events)
        {
            LuaWidgetDataSnapshot::FilesystemWatchEvent publicEvent;
            publicEvent.kind = event.kind;
            publicEvent.name = WideToUtf8(event.name);
            publicEvent.oldName = WideToUtf8(event.oldName);
            if (publicEvent.name.empty()) continue;

            if (event.kind != "removed" && completion.origin && filesystemHandleStore_)
            {
                auto grant = completion.origin->GrantChild(*filesystemHandleStore_,
                    watch.owner, event.name, watch.access);
                if (grant)
                {
                    publicEvent.itemKind = std::string(
                        snowdesktop::widget_runtime::
                            WidgetFilesystemHandleStore::KindName(grant.entry->kind));
                    publicEvent.handle = grant.entry->handle;
                }
            }
            watch.events.push_back(std::move(publicEvent));
        }
        if (watch.events.empty() && !watch.overflow) continue;
        watch.available = true;
        watch.warmingUp = false;
        watch.error.clear();
        ++watch.revision;
        watch.timestampMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        const int widgetIndex = FindWidget(
            Utf8ToWide(watch.owner.instanceId));
        if (widgetIndex < 0 || !widgets_[widgetIndex].valid) continue;
        LuaWidget& widget = widgets_[widgetIndex];
        const std::uint64_t revision = watch.revision;
        const bool overflow = watch.overflow;
        (void)InvokeLifecycleEvent(widget, "data.change",
            [subscriptionId = completion.id, revision, overflow]
            (lua_State* eventState) {
                lua_pushliteral(eventState, "filesystem.watch");
                lua_setfield(eventState, -2, "topic");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(subscriptionId));
                lua_setfield(eventState, -2, "subscriptionId");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(revision));
                lua_setfield(eventState, -2, "revision");
                lua_pushboolean(eventState, overflow);
                lua_setfield(eventState, -2, "overflow");
            });
        RuntimeInvalidateHost(widget.widgetId);
    }
}

void WidgetEngine::ReleaseWidgetDataSubscriptions(LuaWidget& widget)
{
    if (clipboardObservationImages_.erase(widget.widgetId) != 0 && d2dState_)
        d2dState_->runtimeImageCache.Retain(widget.widgetId,
            kClipboardObservationImageSource, {});
    if (!dataBroker_)
    {
        for (const auto& [subscriptionId, _] : widget.dataSubscriptions)
        {
            clipboardSubscriptionRevisions_.erase(subscriptionId);
            if (filesystemWatchService_)
                (void)filesystemWatchService_->Stop(subscriptionId);
            filesystemWatchBindings_.erase(subscriptionId);
        }
        widget.dataSubscriptions.clear();
        return;
    }
    const auto now = snowdesktop::widget_runtime::WidgetDataBroker::Clock::now();
    for (const auto& [subscriptionId, _] : widget.dataSubscriptions)
    {
        clipboardSubscriptionRevisions_.erase(subscriptionId);
        if (filesystemWatchService_)
            (void)filesystemWatchService_->Stop(subscriptionId);
        filesystemWatchBindings_.erase(subscriptionId);
        (void)dataBroker_->Unsubscribe(subscriptionId, now);
    }
    widget.dataSubscriptions.clear();
    ApplyWidgetDataBrokerActions();
}

snowdesktop::widget_runtime::DataSubscriptionResult
WidgetEngine::RuntimeSubscribeData(
    const std::wstring& widgetId, std::string topic,
    std::chrono::milliseconds maxAge,
    snowdesktop::widget_runtime::DataHiddenPolicy whenHidden,
    std::string rangeStart, std::string rangeEnd,
    std::string scopeHandle,
    snowdesktop::widget_runtime::WidgetAudioAnalysisConfiguration
        audioAnalysis, std::string eventId,
    std::optional<snowdesktop::widget_runtime::CalendarEventQueryOptions> calendarQuery)
{
    using snowdesktop::widget_runtime::DataSubscriptionOptions;
    if (!dataBroker_)
        return { 0, "data broker is not initialized" };
    const int index = FindWidget(widgetId);
    if (index < 0)
        return { 0, "widget instance is not loaded" };

    LuaWidget& widget = widgets_[index];
    std::optional<FilesystemWatchBinding> filesystemWatch;
    if (topic == "filesystem.watch")
    {
        if (scopeHandle.empty()) return { 0, "folder handle is required" };
        FilesystemWatchBinding binding;
        binding.owner = {
            WideToUtf8(widget.widgetId), widget.packageId };
        binding.sourceHandle = scopeHandle;
        binding.preview = widget.preview;
        if (widget.preview && IsPreviewFilesystemHandle(scopeHandle))
        {
            binding.directory = L"Preview Folder";
            binding.access = snowdesktop::widget_runtime::
                WidgetFilesystemHandleAccess::Read;
            binding.available = true;
            binding.warmingUp = false;
        }
        else
        {
            if (!filesystemHandleStore_)
                return { 0, "filesystem handle provider is unavailable" };
            const auto entry = filesystemHandleStore_->Resolve(
                binding.owner, scopeHandle);
            if (!entry) return { 0, "invalid filesystem handle" };
            if (entry->kind != snowdesktop::widget_runtime::
                    WidgetFilesystemHandleKind::Folder)
                return { 0, "filesystem handle is not a folder" };
            binding.directory = entry->path;
            binding.access = entry->access;
        }
        filesystemWatch = std::move(binding);
    }
    else if (!scopeHandle.empty())
    {
        return { 0, "scope handle is not supported for this topic" };
    }

    DataSubscriptionOptions options;
    const bool clipboardChanges = topic == kClipboardChangesTopic;
    options.requestedInterval = clipboardChanges ? std::chrono::milliseconds(1) : maxAge;
    options.whenHidden = clipboardChanges
        ? snowdesktop::widget_runtime::DataHiddenPolicy::Continue : whenHidden;
    options.visible = widget.hostVisible;
    const auto requiredPermission =
        dataBroker_->RequiredPermission(topic);
    options.permissionGranted = requiredPermission &&
        (requiredPermission->empty() || RuntimeHasPermission(
            widgetId, requiredPermission->c_str()));
    options.preview = widget.preview;
    options.rangeStart = std::move(rangeStart);
    options.rangeEnd = std::move(rangeEnd);
    options.eventId = std::move(eventId);
    options.calendarQuery = std::move(calendarQuery);
    options.audioWaveform = audioAnalysis.waveform;
    options.audioSpectrum = audioAnalysis.spectrum;
    options.audioRms = audioAnalysis.rms;
    options.audioPeak = audioAnalysis.peak;
    options.audioWaveformPoints = audioAnalysis.waveformPoints;
    options.audioSpectrumBins = audioAnalysis.spectrumBins;
    auto result = dataBroker_->Subscribe(
        WideToUtf8(widgetId), topic, options,
        snowdesktop::widget_runtime::WidgetDataBroker::Clock::now());
    if (result)
    {
        if (clipboardChanges)
            clipboardSubscriptionRevisions_.emplace(result.id, clipboardObserver_.Revision());
        widget.dataSubscriptions.emplace(result.id, std::move(topic));
        if (filesystemWatch)
            filesystemWatchBindings_.emplace(
                result.id, std::move(*filesystemWatch));
        ApplyWidgetDataBrokerActions();
    }
    return result;
}

bool WidgetEngine::RuntimeUnsubscribeData(
    std::uint64_t subscriptionId)
{
    if (subscriptionId == 0 || !dataBroker_) return false;
    bool owned = false;
    for (auto& widget : widgets_)
    {
        if (widget.dataSubscriptions.erase(subscriptionId) > 0)
        {
            owned = true;
            break;
        }
    }
    if (!owned) return false;
    clipboardSubscriptionRevisions_.erase(subscriptionId);
    if (filesystemWatchService_)
        (void)filesystemWatchService_->Stop(subscriptionId);
    filesystemWatchBindings_.erase(subscriptionId);
    const bool removed = dataBroker_->Unsubscribe(subscriptionId,
        snowdesktop::widget_runtime::WidgetDataBroker::Clock::now());
    ApplyWidgetDataBrokerActions();
    return removed;
}

std::vector<snowdesktop::widget_runtime::WidgetResourcePoint>
WidgetEngine::RuntimeGetDataHistory(std::uint64_t subscriptionId, std::string& error,
    std::string_view adapterId, bool includeGpuDetails) const
{
    error.clear();
    const auto binding = dataBroker_ ? dataBroker_->SubscriptionSnapshot(subscriptionId) : std::nullopt;
    if (!binding) { error = "unsubscribed"; return {}; }
    if (previewOnly_ || binding->options.preview) return {};
    if (!binding->options.permissionGranted) { error = "permissionDenied"; return {}; }
    if (binding->topic != "system.cpu" && binding->topic != "system.memory" &&
        binding->topic != "system.gpu" && binding->topic != "system.network.traffic")
    {
        error = "historyUnavailable";
        return {};
    }
    if (!widgetSystemDataProvider_) { error = "providerUnavailable"; return {}; }
    if (binding->topic == "system.gpu")
    {
        if (!includeGpuDetails) { error = "detailsRequired"; return {}; }
        if (adapterId.empty()) { error = "adapterRequired"; return {}; }
        const auto snapshot = widgetSystemDataProvider_->Gpu(true);
        if (snapshot)
        {
            for (const auto& adapter : snapshot->adapters)
                if (adapter.id == adapterId ||
                    std::find(adapter.aliasIds.begin(), adapter.aliasIds.end(), adapterId) != adapter.aliasIds.end())
                    return widgetSystemDataProvider_->ResourceHistory(binding->topic, adapter.id);
        }
        error = "adapterNotFound";
        return {};
    }
    if (!adapterId.empty()) { error = "invalidHistoryOptions"; return {}; }
    return widgetSystemDataProvider_->ResourceHistory(binding->topic);
}

std::optional<LuaWidgetDataSnapshot>
WidgetEngine::RuntimeGetDataSnapshot(
    std::uint64_t subscriptionId, bool includeGpuDetails) const
{
    if (subscriptionId == 0 || !dataBroker_) return std::nullopt;
    const auto binding = dataBroker_->SubscriptionSnapshot(subscriptionId);
    if (!binding) return std::nullopt;

    LuaWidgetDataSnapshot result;
    result.topic = binding->topic;
    if (result.topic == kClipboardChangesTopic)
    {
        if (binding->options.preview)
        {
            result.error = "previewUnavailable";
            return result;
        }
        if (!binding->options.permissionGranted)
        {
            result.error = "permissionDenied";
            return result;
        }
        const auto provider = dataBroker_->Snapshot(result.topic);
        if (provider)
        {
            using snowdesktop::widget_runtime::DataProviderState;
            result.available = provider->state == DataProviderState::Active;
            result.warmingUp = provider->state == DataProviderState::Starting;
            result.stale = false;
            result.error = provider->lastError;
        }
        return result;
    }
    if (binding->options.preview)
    {
        LuaWidgetPreviewDataState previewState =
            LuaWidgetPreviewDataState::Ready;
        const int previewWidgetIndex = FindWidget(
            Utf8ToWide(binding->instanceId));
        if (previewWidgetIndex >= 0)
            previewState = widgets_[previewWidgetIndex].previewDataState;
        return snowdesktop::widget_runtime::BuildPreviewDataSnapshot(*binding, previewState,
            [this](bool selection) {
                return selection ? RuntimeDesktopSelection() : RuntimeDesktopItems();
            });
    }
    if (!binding->options.permissionGranted)
    {
        result.error = "permissionDenied";
        return result;
    }
    if (result.topic == "filesystem.watch")
    {
        const auto watch = filesystemWatchBindings_.find(subscriptionId);
        if (watch == filesystemWatchBindings_.end())
        {
            result.error = "invalidReference";
            return result;
        }
        result.available = watch->second.available;
        result.warmingUp = watch->second.warmingUp;
        result.timestampMs = watch->second.timestampMs;
        result.stale = false;
        result.error = watch->second.error;
        result.filesystemWatchEvents = watch->second.events;
        result.filesystemWatchRevision = watch->second.revision;
        result.filesystemWatchOverflow = watch->second.overflow;
        return result;
    }
    if (!widgetSystemDataProvider_)
    {
        result.error = "providerUnavailable";
        return result;
    }

    const auto timestampNow = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (!snowdesktop::widget_runtime::PopulatePlatformDataSnapshot(
            result, *binding, *widgetSystemDataProvider_, widgetAudioAnalysisProvider_.get(),
            includeGpuDetails, timestampNow, [this, &binding]()
                -> std::optional<snowdesktop::widget_runtime::WidgetDisplayPixelRectDataSnapshot> {
                const int index = FindWidget(Utf8ToWide(binding->instanceId));
                if (index < 0) return std::nullopt;
                const RECT monitor = widgets_[index].surfaceContext.monitorBounds;
                return snowdesktop::widget_runtime::WidgetDisplayPixelRectDataSnapshot{
                    monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top };
            }))
    {
        const auto setFreshness = [&](std::int64_t timestamp) {
            result.timestampMs = timestamp;
            result.stale = snowdesktop::widget_runtime::IsDataSnapshotStale(
                timestampNow, timestamp, binding->options.requestedInterval);
        };
        if (result.topic == "desktop.items" ||
            result.topic == "desktop.selection")
        {
            result.desktopItems = result.topic == "desktop.items"
                ? RuntimeDesktopItems() : RuntimeDesktopSelection();
            const std::size_t maximum = result.topic == "desktop.items"
                ? 2048u : 512u;
            if (result.desktopItems.size() > maximum)
                result.desktopItems.resize(maximum);
            result.desktopRevision = desktopDataRevision_;
            result.available = true;
            setFreshness(timestampNow);
        }
        else if (result.topic == "desktop.changes")
        {
            result.desktopRevision = desktopDataRevision_;
            result.desktopChangeReason = desktopDataChangeReason_;
            result.available = true;
            result.timestampMs = desktopDataTimestampMs_;
            result.stale = false;
        }
        else if (result.topic == "calendar.events")
        {
            result.calendarRangeStart = binding->options.rangeStart;
            result.calendarRangeEnd = binding->options.rangeEnd;
            if (!calendarService_ || !calendarServiceLoaded_)
            {
                result.error = "providerUnavailable";
            }
            else if (!binding->options.eventId.empty())
            {
                const auto event = calendarService_->EventById(binding->options.eventId, true);
                if (event) result.calendarEvents.push_back(*event);
                result.calendarRevision = calendarEventsRevision_;
                result.available = true;
                setFreshness(timestampNow);
            }
            else
            {
                if (result.calendarRangeStart.empty())
                {
                    const std::string selected = RuntimeCalendarSelectedDate();
                    const auto start = snowdesktop::calendar::CalendarService::
                        AddDays(selected, -62);
                    const auto end = snowdesktop::calendar::CalendarService::
                        AddDays(selected, 62);
                    if (start && end)
                    {
                        result.calendarRangeStart = *start;
                        result.calendarRangeEnd = *end;
                    }
                }
                if (result.calendarRangeStart.empty() ||
                    result.calendarRangeEnd.empty())
                {
                    result.error = "calendarRangeUnavailable";
                }
                else
                {
                    result.calendarEvents = RuntimeCalendarEvents(
                        result.calendarRangeStart, result.calendarRangeEnd, true);
                    constexpr std::size_t MaximumEvents = 512;
                    if (binding->options.calendarQuery)
                    {
                        auto page = snowdesktop::widget_runtime::QueryCalendarEvents(
                            result.calendarEvents, *binding->options.calendarQuery);
                        result.calendarEvents = std::move(page.events);
                        result.calendarTotal = page.total;
                        result.calendarHasMore = page.hasMore;
                        result.calendarNextOffset = page.nextOffset;
                        result.calendarTruncated = page.total > result.calendarEvents.size();
                    }
                    else
                    {
                        result.calendarTruncated =
                            result.calendarEvents.size() > MaximumEvents;
                        if (result.calendarTruncated)
                            result.calendarEvents.resize(MaximumEvents);
                    }
                    result.calendarRevision = calendarEventsRevision_;
                    result.available = true;
                    setFreshness(timestampNow);
                }
            }
        }
        else if (result.topic == "calendar.selectedDate")
        {
            result.calendarSelectedDate = RuntimeCalendarSelectedDate();
            result.calendarRevision = calendarSelectionRevision_;
            result.available = !result.calendarSelectedDate.empty();
            if (!result.available)
                result.error = "notPresent";
            setFreshness(timestampNow);
        }
        else if (result.topic == "app.indexStatus")
        {
            result.appIndexState = applicationIndexStatusProvider_
                ? applicationIndexStatusProvider_() : "unavailable";
            if (result.appIndexState != "ready" &&
                result.appIndexState != "indexing" &&
                result.appIndexState != "unavailable")
                result.appIndexState = "unavailable";
            result.appIndexRevision = appIndexRevision_;
            result.available = applicationIndexStatusProvider_ != nullptr &&
                result.appIndexState != "unavailable";
            if (!result.available)
                result.error = "providerUnavailable";
            setFreshness(timestampNow);
        }
    }
    if (result.topic == "audio.output.analysis")
    {
        result.audioAnalysis = snowdesktop::widget_runtime::
            ProjectWidgetAudioAnalysisSnapshot(result.audioAnalysis,
                { binding->options.audioWaveform,
                    binding->options.audioSpectrum,
                    binding->options.audioRms,
                    binding->options.audioPeak,
                    binding->options.audioWaveformPoints,
                    binding->options.audioSpectrumBins });
    }
    if (result.error.empty())
    {
        const auto provider = dataBroker_->Snapshot(result.topic);
        if (provider && !provider->lastError.empty())
            result.error = provider->lastError;
    }
    return result;
}

void WidgetEngine::OnAudioAnalysisWake()
{
    DrainAudioAnalysisChanges();
}

void WidgetEngine::EnsureSystemSnapshotServiceStarted()
{
    if (!systemSnapshotService_ || systemSnapshotServiceStarted_) return;
    systemSnapshotChanged_.store(false);
    mediaSnapshotChanged_.store(false);
    systemSnapshotService_->Start([this](bool systemChanged, bool mediaChanged) {
        if (systemChanged) systemSnapshotChanged_.store(true);
        if (mediaChanged) mediaSnapshotChanged_.store(true);
    });
    systemSnapshotServiceStarted_ = true;
}

CpuSnapshot WidgetEngine::RuntimeGetCpuSnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true, 42.0, 12,
            _L("app.widget_preview.api.cpu") };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetCpu() : CpuSnapshot{};
}

MemorySnapshot WidgetEngine::RuntimeGetMemorySnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true, 16ull * 1024 * 1024 * 1024,
            9ull * 1024 * 1024 * 1024,
            7ull * 1024 * 1024 * 1024, 56.25 };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetMemory() : MemorySnapshot{};
}

BatterySnapshot WidgetEngine::RuntimeGetBatterySnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true, 78.0, true, true, false };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetBattery() : BatterySnapshot{};
}

NetworkSnapshot WidgetEngine::RuntimeGetNetworkSnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true, true, 3ull * 1024 * 1024,
            640ull * 1024, 8ull * 1024 * 1024 * 1024,
            2ull * 1024 * 1024 * 1024 };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetNetwork() : NetworkSnapshot{};
}

GpuSnapshot WidgetEngine::RuntimeGetGpuSnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true, _L("app.widget_preview.api.gpu"), 36.0,
            8ull * 1024 * 1024 * 1024,
            3ull * 1024 * 1024 * 1024 };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetGpu() : GpuSnapshot{};
}

MediaSnapshot WidgetEngine::RuntimeGetMediaSnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true,
            _LW("app.widget_preview.api.media_title"),
            _LW("app.widget_preview.api.media_artist"),
            _LW("app.widget_preview.api.media_album"),
            _LW("app.widget_preview.api.media_genre"),
            "playing", true, true, true };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesMediaSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetMedia() : MediaSnapshot{};
}

bool WidgetEngine::RuntimeMediaPlayPause()
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return false;
    EnsureSystemSnapshotServiceStarted();
    return systemSnapshotService_ && systemSnapshotService_->RequestMediaPlayPause();
}

bool WidgetEngine::RuntimeMediaNext()
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return false;
    EnsureSystemSnapshotServiceStarted();
    return systemSnapshotService_ && systemSnapshotService_->RequestMediaNext();
}

bool WidgetEngine::RuntimeMediaPrevious()
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return false;
    EnsureSystemSnapshotServiceStarted();
    return systemSnapshotService_ && systemSnapshotService_->RequestMediaPrevious();
}

void WidgetEngine::TickWidgetDataBroker()
{
    const auto runtimeNow =
        snowdesktop::widget_runtime::WidgetDataBroker::Clock::now();
    if (dataBroker_)
    {
        dataBroker_->Tick(runtimeNow);
        ApplyWidgetDataBrokerActions();
    }
    DrainFilesystemWatchCompletions();
    DrainClipboardChanges();
}

void WidgetEngine::DrainWidgetProviderChanges()
{
    if (widgetSystemDataProvider_)
    {
        const auto changedTopics =
            widgetSystemDataProvider_->DrainChangedTopics();
        if (!changedTopics.empty())
        {
            std::unordered_set<std::wstring> dirtyWidgets;
            for (const auto& widget : widgets_)
            {
                if (!widget.valid || widget.preview) continue;
                for (const auto& [_, topic] : widget.dataSubscriptions)
                {
                    if (std::find(changedTopics.begin(), changedTopics.end(),
                            topic) != changedTopics.end())
                    {
                        dirtyWidgets.insert(widget.widgetId);
                        break;
                    }
                }
            }
            for (const auto& widgetId : dirtyWidgets)
                RuntimeInvalidateHost(widgetId);
        }
    }
    DrainAudioAnalysisChanges();
}

void WidgetEngine::DrainWidgetSnapshotChanges()
{
    const bool systemChanged = systemSnapshotChanged_.exchange(false);
    const bool mediaChanged = mediaSnapshotChanged_.exchange(false);
    if (systemChanged || mediaChanged)
    {
        std::unordered_set<std::wstring> dirtyWidgets;
        for (const auto& widget : widgets_)
        {
            if (!widget.valid || widget.preview) continue;
            if ((systemChanged && widget.usesSystemSnapshot) ||
                (mediaChanged && widget.usesMediaSnapshot))
            {
                dirtyWidgets.insert(widget.widgetId);
            }
        }
        for (const auto& widgetId : dirtyWidgets)
            RuntimeInvalidateHost(widgetId);
    }
}

void WidgetEngine::RevokeFilesystemHandlesForPackage(
    const std::string& packageId)
{
    if (!filesystemHandleStore_ || packageId.empty()) return;
    std::vector<std::uint64_t> subscriptions;
    for (const auto& [subscriptionId, watch] : filesystemWatchBindings_)
    {
        if (watch.owner.packageId == packageId)
            subscriptions.push_back(subscriptionId);
    }
    for (const auto subscriptionId : subscriptions)
        (void)RuntimeUnsubscribeData(subscriptionId);
    std::string error;
    (void)filesystemHandleStore_->RevokePackage(packageId, error);
}
