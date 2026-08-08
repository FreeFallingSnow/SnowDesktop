#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace snowdesktop
{

using UiScheduleToken = std::uint64_t;

enum class UiAnimationSurface : std::uint8_t
{
    Desktop,
    FloatingDock,
    QuickNavigation,
    Popup,
    WindowTransition,
    Count,
};

struct UiAnimationMetricsSnapshot
{
    bool enabled = false;
    double targetRefreshHz = 60.0;
    double effectiveRefreshHz = 60.0;
    std::uint64_t requestedFrames = 0;
    std::uint64_t deliveredFrames = 0;
    std::uint64_t skippedFrames = 0;
    std::size_t activeAnimations = 0;
    std::size_t activeTimers = 0;
    double frameIntervalP50Ms = 0.0;
    double frameIntervalP95Ms = 0.0;
    double frameIntervalP99Ms = 0.0;
    double uiWorkP50Ms = 0.0;
    double uiWorkP95Ms = 0.0;
    double uiWorkP99Ms = 0.0;
    double commitP50Ms = 0.0;
    double commitP95Ms = 0.0;
    double commitP99Ms = 0.0;
};

/**
 * @brief UI 线程上的统一动画帧与组件截止时间调度器。
 *
 * 调度器只负责唤醒和时间推进，不在后台线程执行应用回调。其 waitable
 * timer 由主消息循环等待，因此 D2D、DComp、Lua 和 HWND 操作仍严格发生
 * 在创建它们的 UI 线程上。
 */
class UiAnimationScheduler
{
public:
    using FrameCallback = std::function<bool(double nowMilliseconds)>;
    using TimerCallback = std::function<void(UiScheduleToken)>;

    UiAnimationScheduler() = default;
    ~UiAnimationScheduler();

    UiAnimationScheduler(const UiAnimationScheduler&) = delete;
    UiAnimationScheduler& operator=(const UiAnimationScheduler&) = delete;

    bool Initialize();
    void Shutdown();

    [[nodiscard]] HANDLE WaitHandle() const noexcept { return timer_; }
    [[nodiscard]] bool HasScheduledWork() const noexcept;

    UiScheduleToken StartAnimation(
        UiAnimationSurface surface, FrameCallback callback);
    UiScheduleToken ScheduleInterval(
        UINT intervalMilliseconds, TimerCallback callback);
    UiScheduleToken ScheduleOnce(
        UINT delayMilliseconds, TimerCallback callback);
    void Cancel(UiScheduleToken token);
    void CancelAll();

    /** @brief 由 UI 消息循环在 waitable timer 就绪时调用。 */
    void DispatchDue();

    void SetSoftwareRendering(bool softwareRendering);
    void RefreshDisplayRate();

    void SetDiagnosticsEnabled(bool enabled);
    [[nodiscard]] bool DiagnosticsEnabled() const noexcept
    { return diagnosticsEnabled_; }
    [[nodiscard]] UiAnimationMetricsSnapshot Metrics() const;
    void RecordCommitDuration(double milliseconds);

    [[nodiscard]] static double MonotonicMilliseconds() noexcept;

private:
    struct FrameEntry
    {
        UiAnimationSurface surface = UiAnimationSurface::Desktop;
        FrameCallback callback;
    };

    struct TimerEntry
    {
        double dueMilliseconds = 0.0;
        double intervalMilliseconds = 0.0;
        bool repeating = false;
        TimerCallback callback;
    };

    static constexpr std::size_t kMetricSampleCapacity = 240;

    UiScheduleToken NextToken();
    void ArmNextWakeup();
    void UpdateAdaptiveRate(double frameWorkMilliseconds);
    [[nodiscard]] double EffectiveFrameIntervalMs() const noexcept;
    static void PushSample(
        std::vector<double>& samples, double value);
    static double Percentile(
        const std::vector<double>& samples, double percentile);

    HANDLE timer_ = nullptr;
    UiScheduleToken nextToken_ = 1;
    std::unordered_map<UiScheduleToken, FrameEntry> frameEntries_;
    std::unordered_map<UiScheduleToken, TimerEntry> timerEntries_;
    double nextFrameMilliseconds_ = 0.0;
    double lastDeliveredFrameMilliseconds_ = 0.0;
    double targetRefreshHz_ = 60.0;
    unsigned adaptiveDivisor_ = 1;
    unsigned consecutiveOverBudgetFrames_ = 0;
    double recoveryStartMilliseconds_ = 0.0;
    double lastPresentationMilliseconds_ = 0.0;
    bool softwareRendering_ = false;
    bool diagnosticsEnabled_ = false;
    std::uint64_t requestedFrames_ = 0;
    std::uint64_t deliveredFrames_ = 0;
    std::uint64_t skippedFrames_ = 0;
    std::vector<double> frameIntervalSamples_;
    std::vector<double> uiWorkSamples_;
    std::vector<double> commitSamples_;
};

} // namespace snowdesktop
