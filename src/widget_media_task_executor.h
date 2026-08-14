#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetMediaTaskRunResult
{
    bool accepted = false;
    std::string error;
};

struct WidgetMediaTaskCompletion
{
    std::uint64_t id = 0;
    bool accepted = false;
    std::string error;
};

class WidgetMediaTaskExecutor
{
public:
    using Runner = std::function<WidgetMediaTaskRunResult(
        std::string_view action)>;

    explicit WidgetMediaTaskExecutor(Runner runner = {});
    ~WidgetMediaTaskExecutor();

    WidgetMediaTaskExecutor(const WidgetMediaTaskExecutor&) = delete;
    WidgetMediaTaskExecutor& operator=(
        const WidgetMediaTaskExecutor&) = delete;

    bool Start(std::uint64_t id, std::string action);
    bool Cancel(std::uint64_t id);
    std::vector<WidgetMediaTaskCompletion> DrainCompletions();
    std::size_t ActiveCount() const;

    static bool SupportsAction(std::string_view action) noexcept;

private:
    struct Request
    {
        std::uint64_t id = 0;
        std::string action;
    };

    static WidgetMediaTaskRunResult RunSystemAction(
        std::string_view action);
    void WorkerMain(std::stop_token stopToken);

    Runner runner_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Request> requests_;
    std::unordered_set<std::uint64_t> active_;
    std::unordered_set<std::uint64_t> canceled_;
    std::vector<WidgetMediaTaskCompletion> completions_;
    std::jthread worker_;
    bool stopping_ = false;
};
}
