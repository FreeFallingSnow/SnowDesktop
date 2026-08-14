#include "widget_media_task_executor.h"

#include <utility>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/base.h>

namespace snowdesktop::widget_runtime
{
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
    std::uint64_t id, std::string action)
{
    if (id == 0 || !SupportsAction(action)) return false;
    std::scoped_lock lock(mutex_);
    if (stopping_ || active_.contains(id)) return false;
    active_.insert(id);
    requests_.push_back({ id, std::move(action) });
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
    return action == "media.toggle" || action == "media.next" ||
        action == "media.previous";
}

WidgetMediaTaskRunResult WidgetMediaTaskExecutor::RunSystemAction(
    std::string_view action)
{
    using namespace winrt::Windows::Media::Control;
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
        const auto session = manager.GetCurrentSession();
        if (!session) return { false, "notAvailable" };
        const auto controls = session.GetPlaybackInfo().Controls();

        bool accepted = false;
        if (action == "media.toggle")
        {
            if (!controls.IsPlayPauseToggleEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TryTogglePlayPauseAsync().get();
        }
        else if (action == "media.next")
        {
            if (!controls.IsNextEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TrySkipNextAsync().get();
        }
        else if (action == "media.previous")
        {
            if (!controls.IsPreviousEnabled())
                return { false, "actionUnsupported" };
            accepted = session.TrySkipPreviousAsync().get();
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
        Request request;
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
            result = runner_(request.action);
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
