#include "widget_audio_output_task_executor.h"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr GUID kSnowDesktopAudioControlContext{
    0x7f0d8045, 0x40cd, 0x48de,
    { 0x8c, 0x32, 0xb9, 0x9d, 0xcd, 0x89, 0x71, 0xa8 }
};
}

WidgetAudioOutputTaskExecutor::WidgetAudioOutputTaskExecutor(
    Runner runner, NowProvider nowProvider)
    : runner_(std::move(runner)), nowProvider_(std::move(nowProvider))
{
    if (!runner_) runner_ = RunSystemAction;
    if (!nowProvider_)
        nowProvider_ = [] { return Clock::now(); };
}

WidgetAudioOutputTaskExecutor::~WidgetAudioOutputTaskExecutor()
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

WidgetAudioOutputTaskStartResult WidgetAudioOutputTaskExecutor::Start(
    std::uint64_t id, std::string instanceId,
    WidgetAudioOutputTaskRequest request)
{
    if (id == 0 || instanceId.empty() || !ValidateRequest(request))
        return { false, "invalidArguments" };
    const auto now = nowProvider_();
    std::scoped_lock lock(mutex_);
    if (stopping_ || active_.contains(id))
        return { false, "taskExecutorUnavailable" };
    if (const auto last = lastStarts_.find(instanceId);
        last != lastStarts_.end() && now >= last->second &&
        now - last->second < MinimumActionInterval)
    {
        return { false, "rateLimited" };
    }
    lastStarts_.insert_or_assign(instanceId, now);
    active_.insert(id);
    requests_.push_back(
        { id, std::move(instanceId), std::move(request) });
    if (!worker_.joinable())
    {
        worker_ = std::jthread(
            [this](std::stop_token stopToken) {
                WorkerMain(stopToken);
            });
    }
    condition_.notify_one();
    return { true, {} };
}

bool WidgetAudioOutputTaskExecutor::Cancel(std::uint64_t id)
{
    std::scoped_lock lock(mutex_);
    if (!active_.contains(id)) return false;
    canceled_.insert(id);
    condition_.notify_all();
    return true;
}

void WidgetAudioOutputTaskExecutor::ForgetInstance(
    std::string_view instanceId)
{
    std::scoped_lock lock(mutex_);
    lastStarts_.erase(std::string(instanceId));
}

std::vector<WidgetAudioOutputTaskCompletion>
WidgetAudioOutputTaskExecutor::DrainCompletions()
{
    std::scoped_lock lock(mutex_);
    return std::exchange(completions_, {});
}

std::size_t WidgetAudioOutputTaskExecutor::ActiveCount() const
{
    std::scoped_lock lock(mutex_);
    return active_.size();
}

bool WidgetAudioOutputTaskExecutor::SupportsAction(
    std::string_view action) noexcept
{
    return action == "audio.output.setVolume" ||
        action == "audio.output.setMute";
}

bool WidgetAudioOutputTaskExecutor::ValidateRequest(
    const WidgetAudioOutputTaskRequest& request) noexcept
{
    if (!SupportsAction(request.action)) return false;
    if (request.action == "audio.output.setVolume")
    {
        return request.volume && std::isfinite(*request.volume) &&
            !request.muted;
    }
    return request.muted.has_value() && !request.volume;
}

WidgetAudioOutputTaskRunResult
WidgetAudioOutputTaskExecutor::RunSystemAction(
    const WidgetAudioOutputTaskRequest& request)
{
    if (!ValidateRequest(request))
        return { false, "invalidArguments" };
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))) || !enumerator)
    {
        return { false, "audioEnumeratorUnavailable" };
    }
    Microsoft::WRL::ComPtr<IMMDevice> endpoint;
    const HRESULT endpointStatus = enumerator->GetDefaultAudioEndpoint(
        eRender, eMultimedia, &endpoint);
    if (FAILED(endpointStatus) || !endpoint)
    {
        return { false,
            endpointStatus == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
                ? "notPresent" : "audioEndpointUnavailable" };
    }
    Microsoft::WRL::ComPtr<IAudioEndpointVolume> endpointVolume;
    if (FAILED(endpoint->Activate(__uuidof(IAudioEndpointVolume),
            CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void**>(endpointVolume.GetAddressOf()))) ||
        !endpointVolume)
    {
        return { false, "audioVolumeUnavailable" };
    }

    HRESULT status = E_FAIL;
    if (request.action == "audio.output.setVolume")
    {
        const float volume = static_cast<float>(
            std::clamp(*request.volume, 0.0, 1.0));
        status = endpointVolume->SetMasterVolumeLevelScalar(
            volume, &kSnowDesktopAudioControlContext);
    }
    else
    {
        status = endpointVolume->SetMute(
            *request.muted ? TRUE : FALSE,
            &kSnowDesktopAudioControlContext);
    }
    return SUCCEEDED(status)
        ? WidgetAudioOutputTaskRunResult{ true, {} }
        : WidgetAudioOutputTaskRunResult{
            false, "audioControlRejected" };
}

void WidgetAudioOutputTaskExecutor::WorkerMain(
    std::stop_token stopToken)
{
    const HRESULT apartmentStatus =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool apartmentInitialized = SUCCEEDED(apartmentStatus);
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

        WidgetAudioOutputTaskRunResult result;
        try
        {
            result = runner_(request.request);
        }
        catch (...)
        {
            result = { false, "audioControlFailed" };
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
    if (apartmentInitialized) CoUninitialize();
}
}
