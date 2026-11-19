#include "widget/runtime/widget_engine.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_surface_host.h"
#include "widget/preview/widget_preview_context.h"
#include "diagnostics/performance_trace.h"
#include "settings/animation_settings.h"
#include "widget/api/widget_api_registry.h"
#include "widget/runtime/widget_time.h"
#include "widget/runtime/widget_timer_dispatch.h"
#include "widget/view/widget_interaction_lua_value.h"

#include <algorithm>
#include <utility>

using snowdesktop::widget_runtime::PushInteractionValue;

void WidgetEngine::OnWidgetTimer(const std::wstring& widgetId, UINT_PTR timerId)
{
    snowdesktop::performance::Scope performanceScope(
        "widget", "timer.dispatch", widgetId);
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& widget = widgets_[idx];
    lua_State* state = widget.state;
    if (!state) return;
    const std::wstring activeWidgetId = widget.widgetId;
    snowdesktop::widget_runtime::WidgetInvalidationBatch::Scope
        invalidationScope(invalidationBatch_, invalidateCallback_);
    WidgetExecutionContextGuard contextGuard(
        d2dState_, activeWidgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    SetWidgetRectContext(d2dState_, widget.lastBounds);

    if (timerId == widget.animationTimerId)
    {
        if (widget.animationTimerId && widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.animationTimerId);
        widget.animationTimerId = 0;
        if (widget.desktopOccluded && !widget.panelActive) return;
        const bool dataRefreshFrame = widget.animationFrames.ConsumeDataRefresh();
        const auto now = snowdesktop::widget_runtime::
            AnimationFrameRequests::Clock::now();
        const auto nowMilliseconds = snowdesktop::widget_runtime::
            CurrentMonotonicMilliseconds();
        const bool desktopTransitionFrame =
            widget.viewTransitions.Tick(now);
        const bool panelTransitionFrame =
            widget.panelViewTransitions.Tick(now);
        const bool reducedMotion =
            HostManagedAnimationsSuppressed();
        const bool animateIndeterminateProgress = !reducedMotion;
        const bool desktopProgressFrame = animateIndeterminateProgress &&
            widget.hostVisible && !widget.desktopOccluded &&
            widget.viewIndeterminateProgressActive;
        const bool panelProgressFrame = animateIndeterminateProgress &&
            widget.panelActive &&
            widget.panelIndeterminateProgressActive;
        if (desktopTransitionFrame || desktopProgressFrame)
            widget.viewTransitionFramePending = true;
        if ((panelTransitionFrame && widget.panelActive) ||
            panelProgressFrame)
            widget.panelViewTransitionFramePending = true;
        const bool desktopMarqueeFrame = !reducedMotion &&
            widget.desktopVisible && AdvanceNativeMarqueeSurface(
                widget.desktopMarquee, now);
        const auto frames = widget.animationFrames.Consume(now);
        for (const auto& frame : frames)
        {
            (void)InvokeLifecycleEvent(widget, "frame",
                [&frame, nowMilliseconds](lua_State* eventState) {
                    lua_pushlstring(eventState,
                        frame.name.data(), frame.name.size());
                    lua_setfield(eventState, -2, "id");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(nowMilliseconds));
                    lua_setfield(eventState, -2, "now");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(
                            frame.deltaMilliseconds));
                    lua_setfield(eventState, -2, "deltaMs");
                });
        }
        const bool generalAnimationFrame =
            desktopTransitionFrame || desktopProgressFrame ||
            (panelTransitionFrame && widget.panelActive) ||
            panelProgressFrame;
        if (generalAnimationFrame || dataRefreshFrame)
            RuntimeInvalidateHost(activeWidgetId);
        else if (desktopMarqueeFrame && invalidateCallback_)
            invalidateCallback_(activeWidgetId,
                NativeMarqueeDirtyRect(widget.desktopMarquee), "desktop");
        (void)ScheduleAnimationFrame(widget);
        return;
    }

    if (timerId == widget.refreshTimerId)
    {
        (void)InvokeLifecycleEvent(widget, "timer",
            [](lua_State* eventState) {
                lua_pushliteral(eventState, "refresh");
                lua_setfield(eventState, -2, "name");
            });
        RuntimeInvalidateHost(activeWidgetId);
        return;
    }

    if (timerId != widget.namedTimerId)
        return;

    if (widget.namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.namedTimerId);
    widget.namedTimerId = 0;

    const auto now = std::chrono::steady_clock::now();
    const auto wallNow = std::chrono::system_clock::now();
    const auto wallNowMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            wallNow.time_since_epoch()).count();
    const std::vector<std::string> dueNames =
        widget.namedTimers.DueNames(now, wallNow);

    bool invoked = false;
    for (const auto& name : dueNames)
    {
        const auto fire = widget.namedTimers.ConsumeDueInfo(
            name, now, wallNow);
        if (!fire)
            continue;

        (void)InvokeLifecycleEvent(widget, "schedule",
            [&fire, wallNowMilliseconds](lua_State* eventState) {
                lua_pushlstring(eventState,
                    fire->name.data(), fire->name.size());
                lua_setfield(eventState, -2, "id");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(wallNowMilliseconds));
                lua_setfield(eventState, -2, "now");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(fire->missed));
                lua_setfield(eventState, -2, "missed");
                lua_pushboolean(eventState,
                    fire->coalesced ? 1 : 0);
                lua_setfield(eventState, -2, "coalesced");
                if (fire->timeline)
                {
                    PushInteractionValue(eventState, fire->value);
                    lua_setfield(eventState, -2, "value");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(
                            fire->timelineIndex));
                    lua_setfield(eventState, -2, "timelineIndex");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(
                            fire->timelineCount));
                    lua_setfield(eventState, -2, "timelineCount");
                    lua_pushboolean(eventState,
                        fire->timelineEnded ? 1 : 0);
                    lua_setfield(eventState, -2, "timelineEnded");
                    lua_pushboolean(eventState,
                        fire->reload ? 1 : 0);
                    lua_setfield(eventState, -2, "reload");
                }
            });
        invoked = true;
    }

    RescheduleNamedTimer(widget);
    if (invoked)
    {
        (void)snowdesktop::widget_api::ConsumeTransientStateDirty(state);
        RuntimeInvalidateHost(activeWidgetId);
    }
}

bool WidgetEngine::RuntimeSetTimer(const std::wstring& widgetId,
    std::uint64_t ownerToken,
    const std::string& name, int intervalMs, bool repeat,
    snowdesktop::widget_runtime::ScheduleHiddenPolicy hiddenPolicy)
{
    if (snowdesktop::widget_runtime::IsDryLoad() &&
        !snowdesktop::widget_runtime::IsPreviewExecution())
        return false;
    using namespace snowdesktop::widget_runtime;
    WidgetTimerMutation mutation;
    mutation.kind = WidgetTimerMutation::Kind::Set;
    mutation.name = name;
    mutation.intervalMs = intervalMs;
    mutation.repeat = repeat;
    mutation.hiddenPolicy = hiddenPolicy;
    return DispatchWidgetTimerMutation(widgets_, widgetId, ownerToken,
        std::move(mutation), NamedTimerSchedule::Clock::now(),
        NamedTimerSchedule::WallClock::now(),
        [this](auto& owner) { RescheduleNamedTimer(owner); });
}

bool WidgetEngine::RuntimeCancelTimer(const std::wstring& widgetId,
    std::uint64_t ownerToken, const std::string& name)
{
    using namespace snowdesktop::widget_runtime;
    WidgetTimerMutation mutation;
    mutation.kind = WidgetTimerMutation::Kind::Cancel;
    mutation.name = name;
    return DispatchWidgetTimerMutation(widgets_, widgetId, ownerToken,
        std::move(mutation), NamedTimerSchedule::Clock::now(),
        NamedTimerSchedule::WallClock::now(),
        [this](auto& owner) { RescheduleNamedTimer(owner); });
}

std::string WidgetEngine::RuntimeRequestAnimationFrame(
    const std::wstring& widgetId, const std::string& name)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return "previewUnavailable";
    const int index = FindWidget(widgetId);
    if (index < 0 || !widgets_[index].state)
        return "hostUnavailable";
    LuaWidget& widget = widgets_[index];
    if (widget.preview)
        return "previewUnavailable";
    if (!widget.hostVisible)
        return "hidden";
    if (QueryWidgetSystemEnvironment().reducedMotion)
        return "reducedMotion";
    if (!widget.animationFrames.Request(name))
        return "quotaExceeded";
    // Retain author requests for the first live frame after uncovering.
    if ((widget.desktopOccluded || widget.desktopOcclusionResumePending) && !widget.panelActive)
        return {};
    if (ScheduleAnimationFrame(widget))
        return {};
    (void)widget.animationFrames.Cancel(name);
    return "hostUnavailable";
}

bool WidgetEngine::RuntimeCancelAnimationFrame(
    const std::wstring& widgetId, const std::string& name)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    LuaWidget& widget = widgets_[index];
    const bool removed = widget.animationFrames.Cancel(name);
    const bool indeterminateProgressActive =
        !HostManagedAnimationsSuppressed() &&
        ((widget.hostVisible && widget.viewIndeterminateProgressActive) ||
            (widget.panelActive &&
                widget.panelIndeterminateProgressActive));
    const bool nativeMarqueeActive =
        !HostManagedAnimationsSuppressed() &&
        widget.desktopVisible &&
        !widget.desktopMarquee.compositionManaged &&
        HasActiveNativeMarquee(widget.desktopMarquee);
    if (!widget.animationFrames.HasPending() &&
        !widget.viewTransitions.HasActive() &&
        !widget.panelViewTransitions.HasActive() &&
        !indeterminateProgressActive &&
        !nativeMarqueeActive &&
        widget.animationTimerId)
    {
        if (widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.animationTimerId);
        widget.animationTimerId = 0;
    }
    return removed;
}

void WidgetEngine::RebindHostTimers()
{
    for (auto& widget : widgets_)
    {
        // The host retires all old deadline tokens before rebinding. Do not
        // route stale IDs through the kill callback because another host
        // implementation may already have reused them for the new surface.
        widget.refreshTimerId = 0;
        widget.namedTimerId = 0;
        widget.animationTimerId = 0;

        if (widget.preview)
            continue;

        if (widget.manifest.refreshIntervalMs > 0 &&
            widgetTimerRequestCallback_)
        {
            widget.refreshTimerId = widgetTimerRequestCallback_(
                widget.widgetId,
                static_cast<UINT>(widget.manifest.refreshIntervalMs));
        }
        RescheduleNamedTimer(widget);
        (void)ScheduleAnimationFrame(widget);
    }
}

void WidgetEngine::RescheduleNamedTimer(LuaWidget& widget)
{
    if (widget.namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.namedTimerId);
    widget.namedTimerId = 0;

    if (widget.preview && (!previewOnly_ || !widgetTimerRequestCallback_))
        return;

    if (!widgetTimerRequestCallback_)
        return;

    const auto delay = widget.namedTimers.NextDelay(
        std::chrono::steady_clock::now());
    if (!delay)
        return;
    widget.namedTimerId = widgetTimerRequestCallback_(
        widget.widgetId, static_cast<UINT>(delay->count()));
}

bool WidgetEngine::ScheduleAnimationFrame(LuaWidget& widget)
{
    if ((widget.desktopOccluded || widget.desktopOcclusionResumePending) && !widget.panelActive)
        return false;
    if (widget.animationTimerId)
        return true;
    const bool indeterminateProgressActive =
        !HostManagedAnimationsSuppressed() &&
        ((widget.hostVisible && !widget.desktopOccluded &&
            widget.viewIndeterminateProgressActive) ||
            (widget.panelActive &&
                widget.panelIndeterminateProgressActive));
    const bool nativeMarqueeActive =
        !HostManagedAnimationsSuppressed() &&
        widget.desktopVisible && !widget.desktopOccluded &&
        !widget.desktopMarquee.compositionManaged &&
        HasActiveNativeMarquee(widget.desktopMarquee);
    const bool highRateAnimationPending =
        widget.animationFrames.HasPending() ||
        widget.viewTransitions.HasActive() ||
        (widget.panelActive && widget.panelViewTransitions.HasActive()) ||
        indeterminateProgressActive;
    const bool pending = highRateAnimationPending || nativeMarqueeActive;
    if ((widget.preview && (!previewOnly_ || !widgetTimerRequestCallback_)) || (!widget.hostVisible && !widget.panelActive) ||
        !pending ||
        !widgetTimerRequestCallback_)
        return false;
    const int frameLimit = snowdesktop::animation::RuntimeFrameLimit();
    const UINT baseInterval = highRateAnimationPending ? 16 : 33;
    const UINT interval = frameLimit > 0
        ? std::max(baseInterval, static_cast<UINT>((1000 + frameLimit - 1) / frameLimit))
        : baseInterval;
    const auto& requestFrame = widgetAnimationTimerRequestCallback_
        ? widgetAnimationTimerRequestCallback_ : widgetTimerRequestCallback_;
    widget.animationTimerId = requestFrame(widget.widgetId, interval);
    return widget.animationTimerId != 0;
}

void WidgetEngine::StopAnimationFrames(LuaWidget& widget)
{
    if (widget.animationTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.animationTimerId);
    widget.animationTimerId = 0;
    (void)widget.animationFrames.SetVisible(false);
    widget.animationFrames.Clear();
    widget.viewTransitions.Clear();
    widget.panelViewTransitions.Clear();
    widget.viewTransitionFramePending = false;
    widget.panelViewTransitionFramePending = false;
    widget.desktopMarquee.framePending = false;
    widget.desktopMarquee.lastAdvance = {};
}

bool WidgetEngine::SyncNativeMarqueeComposition(
    LuaWidget& widget, bool reducedMotion)
{
    auto& surface = widget.desktopMarquee;
    surface.compositionManaged = false;
    if (widget.preview || !widget.desktopVisible ||
        !nativeMarqueeSyncCallback_)
        return false;
    const float durationScale = static_cast<float>(
        snowdesktop::animation::RuntimeDurationScale());
    if (durationScale == 1.0f || reducedMotion)
        surface.compositionManaged = nativeMarqueeSyncCallback_(
            widget.widgetId, surface.marquees, reducedMotion);
    else
    {
        auto presentation = surface.marquees;
        for (auto& marquee : presentation)
            marquee.speed /= durationScale;
        surface.compositionManaged = nativeMarqueeSyncCallback_(
            widget.widgetId, presentation, reducedMotion);
    }
    return surface.compositionManaged;
}

void WidgetEngine::ApplyHostAnimationPreferences()
{
    const bool enabled = !HostManagedAnimationsSuppressed();
    const double durationScale = snowdesktop::animation::RuntimeDurationScale();
    const int frameLimit = snowdesktop::animation::RuntimeFrameLimit();
    const bool motionChanged = !hostAnimationPreferencesKnown_ ||
        enabled != hostAnimationsEnabled_ || durationScale != hostAnimationDurationScale_;
    if (!motionChanged && frameLimit == hostAnimationFrameLimit_)
        return;
    hostAnimationPreferencesKnown_ = true;
    hostAnimationsEnabled_ = enabled;
    hostAnimationDurationScale_ = durationScale;
    hostAnimationFrameLimit_ = frameLimit;
    std::vector<std::wstring> ids;
    for (const auto& widget : widgets_)
        if (widget.valid && !widget.preview) ids.push_back(widget.widgetId);
    for (const auto& id : ids)
    {
        const int index = FindWidget(id);
        if (index < 0) continue;
        auto& widget = widgets_[index];
        if (motionChanged)
        {
            widget.viewTransitions.SetDurationScale(durationScale);
            widget.panelViewTransitions.SetDurationScale(durationScale);
            widget.viewTransitions.Settle();
            widget.panelViewTransitions.Settle();
            widget.viewTransitionFramePending = static_cast<bool>(widget.viewTree);
            widget.panelViewTransitionFramePending = static_cast<bool>(widget.panelViewTree);
            widget.desktopMarquee.lastAdvance = std::chrono::steady_clock::now();
            widget.desktopMarquee.framePending = true;
            if (!enabled)
                for (auto& marquee : widget.desktopMarquee.marquees) marquee.offset = 0.0f;
            (void)SyncNativeMarqueeComposition(widget, !enabled);
        }
        // Re-arm only the shared presentation wake. Named/refresh timers,
        // queued script frames and data-refresh requests are left untouched.
        if (widget.animationTimerId && widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.animationTimerId);
        widget.animationTimerId = 0;
        (void)ScheduleAnimationFrame(widget);
        const bool desktopVisible = widget.desktopVisible;
        const bool panelActive = widget.panelActive;
        const auto panelSurface = widget.panelSurface;
        if (motionChanged && desktopVisible)
            invalidationBatch_.Invalidate(invalidateCallback_, id, std::nullopt, "desktop");
        if (motionChanged && panelActive)
            invalidationBatch_.Invalidate(invalidateCallback_, id, std::nullopt, panelSurface);
    }
}

void WidgetEngine::ClearNativeMarqueeComposition(LuaWidget& widget)
{
    widget.desktopMarquee.compositionManaged = false;
    if (!widget.preview && nativeMarqueeSyncCallback_)
        (void)nativeMarqueeSyncCallback_(widget.widgetId, {}, true);
}

bool WidgetEngine::RuntimeSetTimerAt(const std::wstring& widgetId,
    std::uint64_t ownerToken,
    const std::string& name, std::int64_t epochMilliseconds,
    snowdesktop::widget_runtime::ScheduleHiddenPolicy hiddenPolicy)
{
    if (snowdesktop::widget_runtime::IsDryLoad() &&
        !snowdesktop::widget_runtime::IsPreviewExecution())
        return false;
    using namespace snowdesktop::widget_runtime;
    WidgetTimerMutation mutation;
    mutation.kind = WidgetTimerMutation::Kind::At;
    mutation.name = name;
    mutation.epochMilliseconds = epochMilliseconds;
    mutation.hiddenPolicy = hiddenPolicy;
    return DispatchWidgetTimerMutation(widgets_, widgetId, ownerToken,
        std::move(mutation), NamedTimerSchedule::Clock::now(),
        NamedTimerSchedule::WallClock::now(),
        [this](auto& owner) { RescheduleNamedTimer(owner); });
}

bool WidgetEngine::RuntimeSetTimeline(const std::wstring& widgetId,
    std::uint64_t ownerToken,
    const std::string& name,
    std::vector<snowdesktop::widget_runtime::
        NamedTimerSchedule::TimelineEntry> entries,
    snowdesktop::widget_runtime::ScheduleHiddenPolicy hiddenPolicy,
    bool reloadAtEnd)
{
    if (snowdesktop::widget_runtime::IsDryLoad() &&
        !snowdesktop::widget_runtime::IsPreviewExecution())
        return false;
    using namespace snowdesktop::widget_runtime;
    WidgetTimerMutation mutation;
    mutation.kind = WidgetTimerMutation::Kind::Timeline;
    mutation.name = name;
    mutation.entries = std::move(entries);
    mutation.hiddenPolicy = hiddenPolicy;
    mutation.reloadAtEnd = reloadAtEnd;
    return DispatchWidgetTimerMutation(widgets_, widgetId, ownerToken,
        std::move(mutation), NamedTimerSchedule::Clock::now(),
        NamedTimerSchedule::WallClock::now(),
        [this](auto& owner) { RescheduleNamedTimer(owner); });
}
