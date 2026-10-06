#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetAudioOutputTaskRunResult
{
    bool accepted = false;
    std::string error;
};

struct WidgetAudioOutputTaskCompletion
{
    std::uint64_t id = 0;
    bool accepted = false;
    std::string error;
};

struct WidgetAudioOutputTaskRequest
{
    std::string action;
    std::optional<double> volume;
    std::optional<bool> muted;
};

struct WidgetAudioOutputTaskStartResult
{
    bool started = false;
    std::string error;

    explicit operator bool() const noexcept
    {
        return started && error.empty();
    }
};

class WidgetAudioOutputTaskExecutor
{
public:
    using Clock = std::chrono::steady_clock;
    using Runner = std::function<WidgetAudioOutputTaskRunResult(
        const WidgetAudioOutputTaskRequest& request)>;
    using NowProvider = std::function<Clock::time_point()>;

    static constexpr auto MinimumActionInterval =
        std::chrono::milliseconds(100);

    explicit WidgetAudioOutputTaskExecutor(
        Runner runner = {}, NowProvider nowProvider = {});
    ~WidgetAudioOutputTaskExecutor();

    WidgetAudioOutputTaskExecutor(
        const WidgetAudioOutputTaskExecutor&) = delete;
    WidgetAudioOutputTaskExecutor& operator=(
        const WidgetAudioOutputTaskExecutor&) = delete;

    WidgetAudioOutputTaskStartResult Start(std::uint64_t id,
        std::string instanceId, WidgetAudioOutputTaskRequest request);
    bool Cancel(std::uint64_t id);
    void ForgetInstance(std::string_view instanceId);
    std::vector<WidgetAudioOutputTaskCompletion> DrainCompletions();
    std::size_t ActiveCount() const;

    static bool SupportsAction(std::string_view action) noexcept;
    static bool ValidateRequest(
        const WidgetAudioOutputTaskRequest& request) noexcept;

private:
    struct QueuedRequest
    {
        std::uint64_t id = 0;
        std::string instanceId;
        WidgetAudioOutputTaskRequest request;
    };

    static WidgetAudioOutputTaskRunResult RunSystemAction(
        const WidgetAudioOutputTaskRequest& request);
    void WorkerMain(std::stop_token stopToken);

    Runner runner_;
    NowProvider nowProvider_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedRequest> requests_;
    std::unordered_set<std::uint64_t> active_;
    std::unordered_set<std::uint64_t> canceled_;
    std::unordered_map<std::string, Clock::time_point> lastStarts_;
    std::vector<WidgetAudioOutputTaskCompletion> completions_;
    std::jthread worker_;
    bool stopping_ = false;
};
}
