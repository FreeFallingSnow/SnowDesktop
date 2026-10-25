#include "widget_media_task_executor.h"
#include "diagnostics/performance_trace.h"
#include "widget/runtime/widget_media_session_registry.h"
#include "system/controls/system_control_windows.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/base.h>

namespace snowdesktop::widget_runtime
{
namespace
{
using MediaSession = winrt::Windows::Media::Control::
    GlobalSystemMediaTransportControlsSession;
using MediaSessionManager = winrt::Windows::Media::Control::
    GlobalSystemMediaTransportControlsSessionManager;

MediaSession ResolveMediaSession(
    const MediaSessionManager& manager, std::string_view sessionId)
{
    return ResolveMediaSessionIdentity(manager, sessionId);
}
}

WidgetMediaTaskExecutor::WidgetMediaTaskExecutor(Runner runner)
    : runner_(std::move(runner))
{
    if (!runner_) runner_ = RunSystemAction;
}

WidgetMediaTaskExecutor::~WidgetMediaTaskExecutor()
{
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
        for (const auto& [id, flag] : cancellationFlags_) { (void)id; flag->store(true); }
    }
    if (worker_.joinable())
    {
        worker_.request_stop();
        condition_.notify_all();
        worker_.join();
    }
}

bool WidgetMediaTaskExecutor::Start(
    std::uint64_t id, WidgetMediaTaskRequest request)
{
    if (id == 0 || !ValidateRequest(request)) return false;
    std::scoped_lock lock(mutex_);
    if (stopping_ || active_.contains(id)) return false;
    active_.insert(id);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    cancellationFlags_[id] = canceled;
    requests_.push_back({ id, std::move(request), std::move(canceled) });
    if (!worker_.joinable())
    {
        worker_ = std::jthread(
            [this](std::stop_token stopToken) {
                WorkerMain(stopToken);
            });
    }
    condition_.notify_one();
    return true;
}

bool WidgetMediaTaskExecutor::Cancel(std::uint64_t id)
{
    std::scoped_lock lock(mutex_);
    if (!active_.contains(id)) return false;
    canceled_.insert(id);
    cancellationFlags_.at(id)->store(true);
    condition_.notify_all();
    return true;
}

std::vector<WidgetMediaTaskCompletion>
WidgetMediaTaskExecutor::DrainCompletions()
{
    std::scoped_lock lock(mutex_);
    return std::exchange(completions_, {});
}

std::size_t WidgetMediaTaskExecutor::ActiveCount() const
{
    std::scoped_lock lock(mutex_);
    return active_.size();
}

bool WidgetMediaTaskExecutor::SupportsAction(
    std::string_view action) noexcept
{
    return action == "media.play" || action == "media.pause" ||
        action == "media.toggle" || action == "media.stop" ||
        action == "media.next" || action == "media.previous" ||
        action == "media.seek" || action == "media.setRate" ||
        action == "media.setShuffle" || action == "media.setRepeat";
}

bool WidgetMediaTaskExecutor::ValidateRequest(
    const WidgetMediaTaskRequest& request) noexcept
{
    if (!SupportsAction(request.action) || request.sessionId.size() > 128 ||
        request.sessionId.find('\0') != std::string::npos)
        return false;
    const bool noValue = !request.positionMs && !request.rate &&
        !request.shuffle && request.repeatMode.empty();
    if (request.action == "media.play" ||
        request.action == "media.pause" ||
        request.action == "media.toggle" ||
        request.action == "media.stop" ||
        request.action == "media.next" ||
        request.action == "media.previous")
        return noValue;
    if (request.action == "media.seek")
    {
        return request.positionMs && *request.positionMs >= 0 &&
            *request.positionMs <=
                std::numeric_limits<std::int64_t>::max() / 10000 &&
            !request.rate && !request.shuffle && request.repeatMode.empty();
    }
    if (request.action == "media.setRate")
    {
        return request.rate && std::isfinite(*request.rate) &&
            *request.rate > 0.0 && !request.positionMs &&
            !request.shuffle && request.repeatMode.empty();
    }
    if (request.action == "media.setShuffle")
    {
        return request.shuffle.has_value() && !request.positionMs &&
            !request.rate && request.repeatMode.empty();
    }
    return request.action == "media.setRepeat" &&
        (request.repeatMode == "none" ||
            request.repeatMode == "track" ||
            request.repeatMode == "list") &&
        !request.positionMs && !request.rate && !request.shuffle;
}

WidgetMediaTaskRunResult WidgetMediaTaskExecutor::RunSystemAction(
    const WidgetMediaTaskRequest& request,
    const system_control::Cancellation& cancellation)
{
    using namespace winrt::Windows::Media::Control;
    if (cancellation.Stop()) return {false, cancellation.Failure().error};
    if (!ValidateRequest(request))
        return { false, "invalidArguments" };
    try
    {
        const auto manager = AcquireMediaSessionManager(cancellation);
        if (!manager) return { false, "mediaSessionManagerUnavailable" };
        const auto session = ResolveMediaSession(manager, request.sessionId);
        if (!session) return { false, "notAvailable" };
        const auto controls = session.GetPlaybackInfo().Controls();
        if (cancellation.Stop()) return {false, cancellation.Failure().error};

        bool accepted = false;
        if (request.action == "media.play")
        {
            if (!controls.IsPlayEnabled())
                return { false, "actionUnsupported" };
            accepted = system_control::windows::Await(session.TryPlayAsync(), cancellation);
        }
        else if (request.action == "media.pause")
        {
            if (!controls.IsPauseEnabled())
                return { false, "actionUnsupported" };
            accepted = system_control::windows::Await(session.TryPauseAsync(), cancellation);
        }
        else if (request.action == "media.toggle")
        {
            if (!controls.IsPlayPauseToggleEnabled())
                return { false, "actionUnsupported" };
            accepted = system_control::windows::Await(session.TryTogglePlayPauseAsync(), cancellation);
        }
        else if (request.action == "media.stop")
        {
            if (!controls.IsStopEnabled())
                return { false, "actionUnsupported" };
            accepted = system_control::windows::Await(session.TryStopAsync(), cancellation);
        }
        else if (request.action == "media.next")
        {
            if (!controls.IsNextEnabled())
                return { false, "actionUnsupported" };
            accepted = system_control::windows::Await(session.TrySkipNextAsync(), cancellation);
        }
        else if (request.action == "media.previous")
        {
            if (!controls.IsPreviousEnabled())
                return { false, "actionUnsupported" };
            accepted = system_control::windows::Await(session.TrySkipPreviousAsync(), cancellation);
        }
        else if (request.action == "media.seek")
        {
            if (!controls.IsPlaybackPositionEnabled())
                return { false, "actionUnsupported" };
            const auto timeline = session.GetTimelineProperties();
            const std::int64_t start = timeline.StartTime().count();
            const std::int64_t delta =
                std::chrono::duration_cast<
                    winrt::Windows::Foundation::TimeSpan>(
                        std::chrono::milliseconds(*request.positionMs))
                    .count();
            if (delta > 0 && start >
                    std::numeric_limits<std::int64_t>::max() - delta)
                return { false, "seekOutOfRange" };
            const std::int64_t target = start + delta;
            if (target < timeline.MinSeekTime().count() ||
                target > timeline.MaxSeekTime().count())
                return { false, "seekOutOfRange" };
            accepted = system_control::windows::Await(session.TryChangePlaybackPositionAsync(target), cancellation);
        }
        else if (request.action == "media.setRate")
        {
            if (!controls.IsPlaybackRateEnabled())
                return { false, "actionUnsupported" };
            accepted = system_control::windows::Await(session.TryChangePlaybackRateAsync(*request.rate), cancellation);
        }
        else if (request.action == "media.setShuffle")
        {
            if (!controls.IsShuffleEnabled())
                return { false, "actionUnsupported" };
            accepted = system_control::windows::Await(session.TryChangeShuffleActiveAsync(
                *request.shuffle), cancellation);
        }
        else if (request.action == "media.setRepeat")
        {
            if (!controls.IsRepeatEnabled())
                return { false, "actionUnsupported" };
            using winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
            const auto mode = request.repeatMode == "track"
                ? MediaPlaybackAutoRepeatMode::Track
                : request.repeatMode == "list"
                    ? MediaPlaybackAutoRepeatMode::List
                    : MediaPlaybackAutoRepeatMode::None;
            accepted = system_control::windows::Await(session.TryChangeAutoRepeatModeAsync(mode), cancellation);
        }
        else
        {
            return { false, "taskNotImplemented" };
        }
        return accepted
            ? WidgetMediaTaskRunResult{ true, {} }
            : WidgetMediaTaskRunResult{ false, "actionRejected" };
    }
    catch (...)
    {
        if (cancellation.Stop()) return {false, cancellation.Failure().error};
        return { false, "mediaActionFailed" };
    }
}

void WidgetMediaTaskExecutor::WorkerMain(std::stop_token stopToken)
{
    bool apartmentInitialized = false;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;
    }
    catch (...)
    {
    }

    while (!stopToken.stop_requested())
    {
        QueuedRequest request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] {
                return stopToken.stop_requested() || !requests_.empty();
            });
            if (stopToken.stop_requested()) break;
            request = std::move(requests_.front());
            requests_.pop_front();
            if (canceled_.contains(request.id))
            {
                active_.erase(request.id);
                canceled_.erase(request.id);
                cancellationFlags_.erase(request.id);
                completions_.push_back(
                    { request.id, false, "canceled" });
                continue;
            }
        }

        WidgetMediaTaskRunResult result;
        try
        {
            const system_control::Cancellation cancellation{request.canceled,
                std::chrono::steady_clock::now() + std::chrono::seconds(5)};
            performance::TaskExecution trace(request.id, "media");
            result = runner_(request.request, cancellation);
        }
        catch (...)
        {
            result = { false, "mediaActionFailed" };
        }
        {
            std::scoped_lock lock(mutex_);
            if (canceled_.erase(request.id) > 0)
                result = { false, "canceled" };
            active_.erase(request.id);
            cancellationFlags_.erase(request.id);
            completions_.push_back({ request.id, result.accepted,
                std::move(result.error) });
        }
    }
    if (apartmentInitialized)
        winrt::uninit_apartment();
}
}
