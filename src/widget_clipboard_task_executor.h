#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetClipboardTaskRequest
{
    std::string action;
    std::string format;
    std::string text;
};

struct WidgetClipboardTaskRunResult
{
    bool ok = false;
    std::string format;
    std::string text;
    std::string error;
};

struct WidgetClipboardTaskCompletion
{
    std::uint64_t id = 0;
    std::string action;
    bool ok = false;
    std::string format;
    std::string text;
    std::string error;
};

struct WidgetClipboardTaskStartResult
{
    bool started = false;
    std::string error;

    explicit operator bool() const noexcept
    {
        return started && error.empty();
    }
};

class WidgetClipboardTaskExecutor
{
public:
    using Clock = std::chrono::steady_clock;
    using Runner = std::function<WidgetClipboardTaskRunResult(
        const WidgetClipboardTaskRequest& request)>;
    using NowProvider = std::function<Clock::time_point()>;

    static constexpr std::size_t MaximumTextBytes = 256 * 1024;
    static constexpr auto MinimumActionInterval =
        std::chrono::milliseconds(100);

    explicit WidgetClipboardTaskExecutor(
        Runner runner = {}, NowProvider nowProvider = {});
    ~WidgetClipboardTaskExecutor();

    WidgetClipboardTaskExecutor(
        const WidgetClipboardTaskExecutor&) = delete;
    WidgetClipboardTaskExecutor& operator=(
        const WidgetClipboardTaskExecutor&) = delete;

    WidgetClipboardTaskStartResult Start(std::uint64_t id,
        std::string instanceId, WidgetClipboardTaskRequest request);
    bool Cancel(std::uint64_t id);
    void ForgetInstance(std::string_view instanceId);
    std::vector<WidgetClipboardTaskCompletion> DrainCompletions();
    std::size_t ActiveCount() const;

    static bool SupportsAction(std::string_view action) noexcept;
    static bool ValidateRequest(
        const WidgetClipboardTaskRequest& request) noexcept;

private:
    struct QueuedRequest
    {
        std::uint64_t id = 0;
        std::string instanceId;
        WidgetClipboardTaskRequest request;
    };

    static WidgetClipboardTaskRunResult RunSystemAction(
        const WidgetClipboardTaskRequest& request);
    void WorkerMain(std::stop_token stopToken);

    Runner runner_;
    NowProvider nowProvider_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedRequest> requests_;
    std::unordered_set<std::uint64_t> active_;
    std::unordered_set<std::uint64_t> canceled_;
    std::unordered_map<std::string, Clock::time_point> lastStarts_;
    std::vector<WidgetClipboardTaskCompletion> completions_;
    std::jthread worker_;
    bool stopping_ = false;
};
}
