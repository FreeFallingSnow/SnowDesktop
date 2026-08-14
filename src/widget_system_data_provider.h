#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
struct WidgetCpuDataSnapshot
{
    bool available = false;
    bool warmingUp = true;
    double usagePercent = 0.0;
    unsigned int logicalProcessors = 0;
    std::string name;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetMemoryDataSnapshot
{
    bool available = false;
    std::uint64_t totalBytes = 0;
    std::uint64_t usedBytes = 0;
    std::uint64_t freeBytes = 0;
    double usagePercent = 0.0;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetPowerDataSnapshot
{
    bool available = false;
    bool acPower = false;
    bool charging = false;
    bool saver = false;
    double batteryPercent = 0.0;
    std::int64_t estimatedRemainingSeconds = -1;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

class WidgetSystemDataProvider
{
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::milliseconds MinimumInterval{ 10 };
    static constexpr std::chrono::milliseconds MaximumInterval{
        24 * 60 * 60 * 1000 };

    WidgetSystemDataProvider() = default;
    ~WidgetSystemDataProvider();

    WidgetSystemDataProvider(const WidgetSystemDataProvider&) = delete;
    WidgetSystemDataProvider& operator=(
        const WidgetSystemDataProvider&) = delete;

    bool StartTopic(std::string_view topic,
        std::chrono::milliseconds interval);
    bool StopTopic(std::string_view topic);
    void StopAll();

    std::optional<WidgetCpuDataSnapshot> Cpu() const;
    std::optional<WidgetMemoryDataSnapshot> Memory() const;
    std::optional<WidgetPowerDataSnapshot> Power() const;
    std::vector<std::string> DrainChangedTopics();

    bool Running() const noexcept;
    std::size_t ActiveTopicCount() const;
    static bool SupportsTopic(std::string_view topic) noexcept;

private:
    struct TopicSchedule
    {
        std::chrono::milliseconds interval{ 1000 };
        Clock::time_point due{};
    };

    void WorkerMain(std::stop_token stopToken);
    WidgetCpuDataSnapshot SampleCpu();
    WidgetMemoryDataSnapshot SampleMemory();
    WidgetPowerDataSnapshot SamplePower();
    void PublishCpu(WidgetCpuDataSnapshot snapshot);
    void PublishMemory(WidgetMemoryDataSnapshot snapshot);
    void PublishPower(WidgetPowerDataSnapshot snapshot);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<std::string, TopicSchedule> schedules_;
    std::unordered_set<std::string> changedTopics_;
    std::optional<WidgetCpuDataSnapshot> cpu_;
    std::optional<WidgetMemoryDataSnapshot> memory_;
    std::optional<WidgetPowerDataSnapshot> power_;
    std::uint64_t configurationGeneration_ = 0;
    std::jthread worker_;
    std::atomic<bool> resetCpuBaseline_{ true };

    std::uint64_t previousIdle_ = 0;
    std::uint64_t previousKernel_ = 0;
    std::uint64_t previousUser_ = 0;
};
}
