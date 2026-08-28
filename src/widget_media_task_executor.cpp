#include "widget_media_task_executor.h"
#include "widget_media_contract.h"

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
    const auto currentSession = manager.GetCurrentSession();
    if (sessionId.empty()) return currentSession;

    std::unordered_map<std::wstring, std::size_t> sourceOccurrences;
    std::size_t exposed = 0;
    const auto match = [&](const MediaSession& session) -> MediaSession {
        if (!session || exposed >= MaximumExposedMediaSessions)
            return nullptr;
        ++exposed;
        const std::wstring sourceId =
            session.SourceAppUserModelId().c_str();
        const std::size_t occurrence = sourceOccurrences[sourceId]++;
        return OpaqueMediaSessionId(sourceId, occurrence) == sessionId
            ? session : MediaSession{ nullptr };
    };

    if (currentSession)
    {
        if (const auto found = match(currentSession)) return found;
    }
    for (const auto& session : manager.GetSessions())
    {
        if (exposed >= MaximumExposedMediaSessions) break;
        if (currentSession && session == currentSession) continue;
        if (const auto found = match(session)) return found;
    }
    return nullptr;
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
    requests_.push_back({ id, std::move(request) });
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
    const WidgetMediaTaskRequest& request)
{
    using namespace winrt::Windows::Media::Control;
    if (!ValidateRequest(request))
        return { false, "invalidArguments" };
    try
    {
        thread_local GlobalSystemMediaTransportControlsSessionManager manager{
            nullptr };
        if (!manager)
        {
            manager = GlobalSystemMediaTransportControlsSessionManager::
                RequestAsync().get();
        }
        if (!manager) return { false, "mediaSessionManagerUnavailable" };
        const auto session = ResolveMediaSession(manager, request.sessionId);
        if (!session) return { false, "notAvailable" };
        const auto controls = session.GetPlaybackInfo().Controls();

        bool accepted = false;
        if (request.action == "media.play")
        {
            if (!controls.IsPlayEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TryPlayAsync().get();
        }
        else if (request.action == "media.pause")
        {
            if (!controls.IsPauseEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TryPauseAsync().get();
        }
        else if (request.action == "media.toggle")
        {
            if (!controls.IsPlayPauseToggleEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TryTogglePlayPauseAsync().get();
        }
        else if (request.action == "media.stop")
        {
            if (!controls.IsStopEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TryStopAsync().get();
        }
        else if (request.action == "media.next")
        {
            if (!controls.IsNextEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TrySkipNextAsync().get();
        }
        else if (request.action == "media.previous")
        {
            if (!controls.IsPreviousEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TrySkipPreviousAsync().get();
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
            accepted = session.TryChangePlaybackPositionAsync(target).get();
        }
        else if (request.action == "media.setRate")
        {
            if (!controls.IsPlaybackRateEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TryChangePlaybackRateAsync(*request.rate).get();
        }
        else if (request.action == "media.setShuffle")
        {
            if (!controls.IsShuffleEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TryChangeShuffleActiveAsync(
                *request.shuffle).get();
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
            accepted = session.TryChangeAutoRepeatModeAsync(mode).get();
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
                completions_.push_back(
                    { request.id, false, "canceled" });
                continue;
            }
        }

        WidgetMediaTaskRunResult result;
        try
        {
            result = runner_(request.request);
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
            completions_.push_back({ request.id, result.accepted,
                std::move(result.error) });
        }
    }
    if (apartmentInitialized)
        winrt::uninit_apartment();
}
}
