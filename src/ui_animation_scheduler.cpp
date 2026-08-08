#include "ui_animation_scheduler.h"
#include "ui_animation_scheduler_rules.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace snowdesktop
{
namespace
{
using namespace ui_animation_scheduler_rules;

BOOL CALLBACK AccumulateMonitorRefreshRate(
    HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
    auto* maximum = reinterpret_cast<double*>(parameter);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return TRUE;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsExW(
            monitorInfo.szDevice, ENUM_CURRENT_SETTINGS,
            &mode, 0) &&
        mode.dmDisplayFrequency >= 30 &&
        mode.dmDisplayFrequency <= 1000)
    {
        *maximum = std::max(
            *maximum,
            static_cast<double>(mode.dmDisplayFrequency));
    }
    return TRUE;
}
} // namespace

UiAnimationScheduler::~UiAnimationScheduler()
{
    Shutdown();
}

bool UiAnimationScheduler::Initialize()
{
    if (timer_)
        return true;

#ifdef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
    timer_ = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
#endif
    if (!timer_)
        timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!timer_)
        return false;

    RefreshDisplayRate();
    return true;
}

void UiAnimationScheduler::Shutdown()
{
    CancelAll();
    if (timer_)
    {
        CloseHandle(timer_);
        timer_ = nullptr;
    }
}

bool UiAnimationScheduler::HasScheduledWork() const noexcept
{
    return !frameEntries_.empty() || !timerEntries_.empty();
}

UiScheduleToken UiAnimationScheduler::NextToken()
{
    while (nextToken_ == 0 ||
        frameEntries_.contains(nextToken_) ||
        timerEntries_.contains(nextToken_))
    {
        ++nextToken_;
    }
    return nextToken_++;
}

UiScheduleToken UiAnimationScheduler::StartAnimation(
    UiAnimationSurface surface, FrameCallback callback)
{
    if (!callback || (!timer_ && !Initialize()))
        return 0;

    const UiScheduleToken token = NextToken();
    frameEntries_.emplace(
        token, FrameEntry{ surface, std::move(callback) });
    if (frameEntries_.size() == 1)
    {
        const double now = MonotonicMilliseconds();
        nextFrameMilliseconds_ = NextFrameDeadline(
            now, lastPresentationMilliseconds_,
            EffectiveFrameIntervalMs());
    }
    ArmNextWakeup();
    return token;
}

UiScheduleToken UiAnimationScheduler::ScheduleInterval(
    UINT intervalMilliseconds, TimerCallback callback)
{
    if (!callback || (!timer_ && !Initialize()))
        return 0;

    const double interval = static_cast<double>(
        std::max<UINT>(1, intervalMilliseconds));
    const UiScheduleToken token = NextToken();
    timerEntries_.emplace(token, TimerEntry{
        MonotonicMilliseconds() + interval,
        interval,
        true,
        std::move(callback),
    });
    ArmNextWakeup();
    return token;
}

UiScheduleToken UiAnimationScheduler::ScheduleOnce(
    UINT delayMilliseconds, TimerCallback callback)
{
    if (!callback || (!timer_ && !Initialize()))
        return 0;

    const UiScheduleToken token = NextToken();
    timerEntries_.emplace(token, TimerEntry{
        MonotonicMilliseconds() + static_cast<double>(delayMilliseconds),
        0.0,
        false,
        std::move(callback),
    });
    ArmNextWakeup();
    return token;
}

void UiAnimationScheduler::Cancel(UiScheduleToken token)
{
    if (!token)
        return;
    frameEntries_.erase(token);
    timerEntries_.erase(token);
    if (frameEntries_.empty())
    {
        nextFrameMilliseconds_ = 0.0;
        lastDeliveredFrameMilliseconds_ = 0.0;
        consecutiveOverBudgetFrames_ = 0;
        recoveryStartMilliseconds_ = 0.0;
        adaptiveDivisor_ = 1;
    }
    ArmNextWakeup();
}

void UiAnimationScheduler::CancelAll()
{
    frameEntries_.clear();
    timerEntries_.clear();
    nextFrameMilliseconds_ = 0.0;
    lastDeliveredFrameMilliseconds_ = 0.0;
    lastPresentationMilliseconds_ = 0.0;
    consecutiveOverBudgetFrames_ = 0;
    recoveryStartMilliseconds_ = 0.0;
    adaptiveDivisor_ = 1;
    if (timer_)
        CancelWaitableTimer(timer_);
}

void UiAnimationScheduler::DispatchDue()
{
    if (!timer_)
        return;

    const double now = MonotonicMilliseconds();
    std::vector<UiScheduleToken> dueTimers;
    dueTimers.reserve(timerEntries_.size());
    for (const auto& [token, entry] : timerEntries_)
    {
        if (entry.dueMilliseconds <= now + 0.05)
            dueTimers.push_back(token);
    }

    for (UiScheduleToken token : dueTimers)
    {
        auto found = timerEntries_.find(token);
        if (found == timerEntries_.end())
            continue;

        TimerCallback callback = found->second.callback;
        if (found->second.repeating)
        {
            const double interval = std::max(
                1.0, found->second.intervalMilliseconds);
            found->second.dueMilliseconds =
                AdvanceRepeatingDeadline(
                    found->second.dueMilliseconds,
                    interval, now);
        }
        else
        {
            timerEntries_.erase(found);
        }

        try
        {
            callback(token);
        }
        catch (...)
        {
            Cancel(token);
        }
    }

    if (!frameEntries_.empty() &&
        now + 0.05 >= nextFrameMilliseconds_)
    {
        const double interval = EffectiveFrameIntervalMs();
        std::uint64_t missed = 0;
        if (nextFrameMilliseconds_ > 0.0 && now > nextFrameMilliseconds_)
        {
            missed = MissedFrameCount(
                nextFrameMilliseconds_, now, interval);
            skippedFrames_ += missed;
        }
        requestedFrames_ += missed + 1;

        if (diagnosticsEnabled_ &&
            lastDeliveredFrameMilliseconds_ > 0.0)
        {
            PushSample(
                frameIntervalSamples_,
                now - lastDeliveredFrameMilliseconds_);
        }
        lastDeliveredFrameMilliseconds_ = now;
        lastPresentationMilliseconds_ = now;

        std::vector<UiScheduleToken> frameTokens;
        frameTokens.reserve(frameEntries_.size());
        for (const auto& [token, _] : frameEntries_)
            frameTokens.push_back(token);

        const double workStart = MonotonicMilliseconds();
        for (UiScheduleToken token : frameTokens)
        {
            auto found = frameEntries_.find(token);
            if (found == frameEntries_.end())
                continue;

            FrameCallback callback = found->second.callback;
            bool keep = false;
            try
            {
                keep = callback(now);
            }
            catch (...)
            {
                keep = false;
            }
            if (!keep)
                frameEntries_.erase(token);
        }
        const double frameWork =
            MonotonicMilliseconds() - workStart;
        ++deliveredFrames_;
        if (diagnosticsEnabled_)
            PushSample(uiWorkSamples_, frameWork);
        UpdateAdaptiveRate(frameWork);

        if (frameEntries_.empty())
        {
            nextFrameMilliseconds_ = 0.0;
            lastDeliveredFrameMilliseconds_ = 0.0;
        }
        else
        {
            const double nextInterval = EffectiveFrameIntervalMs();
            nextFrameMilliseconds_ += nextInterval;
            while (nextFrameMilliseconds_ <= now)
                nextFrameMilliseconds_ += nextInterval;
        }
    }

    ArmNextWakeup();
}

void UiAnimationScheduler::SetSoftwareRendering(bool softwareRendering)
{
    softwareRendering_ = softwareRendering;
    RefreshDisplayRate();
}

void UiAnimationScheduler::RefreshDisplayRate()
{
    double maximum = 0.0;
    EnumDisplayMonitors(
        nullptr, nullptr,
        AccumulateMonitorRefreshRate,
        reinterpret_cast<LPARAM>(&maximum));
    targetRefreshHz_ = ClampTargetRefresh(
        maximum,
        softwareRendering_ ||
            GetSystemMetrics(SM_REMOTESESSION));
    adaptiveDivisor_ = 1;
    consecutiveOverBudgetFrames_ = 0;
    recoveryStartMilliseconds_ = 0.0;
    lastPresentationMilliseconds_ = 0.0;
    if (!frameEntries_.empty())
        nextFrameMilliseconds_ = MonotonicMilliseconds();
    ArmNextWakeup();
}

void UiAnimationScheduler::SetDiagnosticsEnabled(bool enabled)
{
    diagnosticsEnabled_ = enabled;
    frameIntervalSamples_.clear();
    uiWorkSamples_.clear();
    commitSamples_.clear();
    requestedFrames_ = 0;
    deliveredFrames_ = 0;
    skippedFrames_ = 0;
}

UiAnimationMetricsSnapshot UiAnimationScheduler::Metrics() const
{
    UiAnimationMetricsSnapshot result;
    result.enabled = diagnosticsEnabled_;
    result.targetRefreshHz = targetRefreshHz_;
    result.effectiveRefreshHz =
        1000.0 / EffectiveFrameIntervalMs();
    result.requestedFrames = requestedFrames_;
    result.deliveredFrames = deliveredFrames_;
    result.skippedFrames = skippedFrames_;
    result.activeAnimations = frameEntries_.size();
    result.activeTimers = timerEntries_.size();
    if (diagnosticsEnabled_)
    {
        result.frameIntervalP50Ms =
            Percentile(frameIntervalSamples_, 0.50);
        result.frameIntervalP95Ms =
            Percentile(frameIntervalSamples_, 0.95);
        result.frameIntervalP99Ms =
            Percentile(frameIntervalSamples_, 0.99);
        result.uiWorkP50Ms = Percentile(uiWorkSamples_, 0.50);
        result.uiWorkP95Ms = Percentile(uiWorkSamples_, 0.95);
        result.uiWorkP99Ms = Percentile(uiWorkSamples_, 0.99);
        result.commitP50Ms = Percentile(commitSamples_, 0.50);
        result.commitP95Ms = Percentile(commitSamples_, 0.95);
        result.commitP99Ms = Percentile(commitSamples_, 0.99);
    }
    return result;
}

void UiAnimationScheduler::RecordCommitDuration(double milliseconds)
{
    if (diagnosticsEnabled_ && milliseconds >= 0.0)
        PushSample(commitSamples_, milliseconds);
}

double UiAnimationScheduler::MonotonicMilliseconds() noexcept
{
    static const LONGLONG frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value)
            ? value.QuadPart : 0LL;
    }();
    LARGE_INTEGER counter{};
    if (frequency <= 0 ||
        !QueryPerformanceCounter(&counter))
    {
        return static_cast<double>(GetTickCount64());
    }
    return static_cast<double>(counter.QuadPart) * 1000.0 /
        static_cast<double>(frequency);
}

void UiAnimationScheduler::ArmNextWakeup()
{
    if (!timer_)
        return;

    double next = std::numeric_limits<double>::infinity();
    if (!frameEntries_.empty())
        next = nextFrameMilliseconds_;
    for (const auto& [_, entry] : timerEntries_)
        next = std::min(next, entry.dueMilliseconds);

    if (!std::isfinite(next))
    {
        CancelWaitableTimer(timer_);
        return;
    }

    const double delayMilliseconds = std::max(
        0.0, next - MonotonicMilliseconds());
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<LONGLONG>(
        1,
        static_cast<LONGLONG>(
            std::ceil(delayMilliseconds * 10000.0)));
    SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
}

void UiAnimationScheduler::UpdateAdaptiveRate(
    double frameWorkMilliseconds)
{
    const double now = MonotonicMilliseconds();
    const double budget = EffectiveFrameIntervalMs();
    if (frameWorkMilliseconds > budget)
    {
        recoveryStartMilliseconds_ = 0.0;
        if (++consecutiveOverBudgetFrames_ >= 3)
        {
            adaptiveDivisor_ = ReduceAdaptiveDivisor(
                targetRefreshHz_, adaptiveDivisor_);
            consecutiveOverBudgetFrames_ = 0;
        }
        return;
    }

    consecutiveOverBudgetFrames_ = 0;
    if (adaptiveDivisor_ <= 1 ||
        frameWorkMilliseconds >= budget * 0.70)
    {
        recoveryStartMilliseconds_ = 0.0;
        return;
    }

    if (recoveryStartMilliseconds_ <= 0.0)
        recoveryStartMilliseconds_ = now;
    else if (RecoveryWindowSatisfied(
        now - recoveryStartMilliseconds_,
        frameWorkMilliseconds, budget))
    {
        adaptiveDivisor_ = std::max(1U, adaptiveDivisor_ / 2);
        recoveryStartMilliseconds_ = 0.0;
    }
}

double UiAnimationScheduler::EffectiveFrameIntervalMs() const noexcept
{
    const double effectiveRefresh = std::max(
        kMinimumRefreshHz,
        targetRefreshHz_ /
            static_cast<double>(std::max(1U, adaptiveDivisor_)));
    return 1000.0 / effectiveRefresh;
}

void UiAnimationScheduler::PushSample(
    std::vector<double>& samples, double value)
{
    if (samples.size() >= kMetricSampleCapacity)
        samples.erase(samples.begin());
    samples.push_back(value);
}

double UiAnimationScheduler::Percentile(
    const std::vector<double>& samples, double percentile)
{
    if (samples.empty())
        return 0.0;
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const double position = std::clamp(
        percentile, 0.0, 1.0) *
        static_cast<double>(sorted.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(
        std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(
        std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] +
        (sorted[upper] - sorted[lower]) * fraction;
}

} // namespace snowdesktop
