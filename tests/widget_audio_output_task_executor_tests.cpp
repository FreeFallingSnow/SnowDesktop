#include "widget_audio_output_task_executor.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using snowdesktop::widget_runtime::WidgetAudioOutputTaskExecutor;
using snowdesktop::widget_runtime::WidgetAudioOutputTaskRequest;
using snowdesktop::widget_runtime::WidgetAudioOutputTaskRunResult;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

template <typename Predicate>
bool WaitUntil(Predicate predicate)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

void TestValidationCompletionAndRateLimit()
{
    Check(WidgetAudioOutputTaskExecutor::SupportsAction(
                "audio.output.setVolume") &&
            WidgetAudioOutputTaskExecutor::SupportsAction(
                "audio.output.setMute") &&
            !WidgetAudioOutputTaskExecutor::SupportsAction(
                "audio.output.setDevice"),
        "the executor must expose only bounded default-output controls");
    Check(WidgetAudioOutputTaskExecutor::ValidateRequest(
                { .action = "audio.output.setVolume", .volume = 0.5 }) &&
            WidgetAudioOutputTaskExecutor::ValidateRequest(
                { .action = "audio.output.setVolume", .volume = 2.0 }) &&
            WidgetAudioOutputTaskExecutor::ValidateRequest(
                { .action = "audio.output.setMute", .muted = false }) &&
            !WidgetAudioOutputTaskExecutor::ValidateRequest(
                { .action = "audio.output.setVolume" }) &&
            !WidgetAudioOutputTaskExecutor::ValidateRequest(
                { .action = "audio.output.setMute", .volume = 0.5,
                    .muted = true }),
        "audio control payloads must be finite and action-specific");

    auto now = WidgetAudioOutputTaskExecutor::Clock::time_point{};
    WidgetAudioOutputTaskExecutor executor(
        [](const WidgetAudioOutputTaskRequest& request)
            -> WidgetAudioOutputTaskRunResult {
            return request.action == "audio.output.setVolume"
                ? WidgetAudioOutputTaskRunResult{ true, {} }
                : WidgetAudioOutputTaskRunResult{
                    false, "audioControlRejected" };
        },
        [&] { return now; });
    Check(static_cast<bool>(executor.Start(1, "widget-a",
            { .action = "audio.output.setVolume", .volume = 1.5 })),
        "the first valid audio action must start");
    const auto limited = executor.Start(2, "widget-a",
        { .action = "audio.output.setMute", .muted = true });
    Check(!limited && limited.error == "rateLimited",
        "one widget must not issue audio changes faster than the limit");
    Check(static_cast<bool>(executor.Start(3, "widget-b",
            { .action = "audio.output.setMute", .muted = false })),
        "rate limiting must be isolated by widget instance");
    now += WidgetAudioOutputTaskExecutor::MinimumActionInterval;
    Check(static_cast<bool>(executor.Start(4, "widget-a",
            { .action = "audio.output.setMute", .muted = false })),
        "the same widget may act after the minimum interval");

    std::vector<snowdesktop::widget_runtime::
        WidgetAudioOutputTaskCompletion> completions;
    Check(WaitUntil([&] {
            completions = executor.DrainCompletions();
            return completions.size() == 3;
        }),
        "started audio actions must complete asynchronously");
    Check(completions[0].id == 1 && completions[0].accepted &&
            completions[1].id == 3 && !completions[1].accepted &&
            completions[1].error == "audioControlRejected" &&
            completions[2].id == 4 && !completions[2].accepted,
        "audio completion IDs and stable errors must be preserved");

    executor.ForgetInstance("widget-a");
    Check(static_cast<bool>(executor.Start(5, "widget-a",
            { .action = "audio.output.setMute", .muted = true })),
        "disposing an instance must release its rate-limit state");
}

void TestCancellationOverridesLateResult()
{
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    WidgetAudioOutputTaskExecutor executor(
        [&](const WidgetAudioOutputTaskRequest&)
            -> WidgetAudioOutputTaskRunResult {
            std::unique_lock lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
            return { true, {} };
        });
    Check(static_cast<bool>(executor.Start(9, "widget-c",
            { .action = "audio.output.setMute", .muted = true })),
        "a valid audio task must enter the worker queue");
    {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, 1s, [&] { return entered; });
    }
    Check(entered && executor.Cancel(9),
        "an active audio task must accept cancellation");
    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    condition.notify_all();

    std::vector<snowdesktop::widget_runtime::
        WidgetAudioOutputTaskCompletion> completions;
    Check(WaitUntil([&] {
            completions = executor.DrainCompletions();
            return !completions.empty();
        }),
        "a canceled audio task must complete");
    Check(completions.size() == 1 && completions[0].id == 9 &&
            !completions[0].accepted &&
            completions[0].error == "canceled",
        "cancellation must override a late successful audio result");
}
}

int main()
{
    TestValidationCompletionAndRateLimit();
    TestCancellationOverridesLateResult();
    std::cout << "widget audio output task executor tests passed\n";
    return 0;
}
