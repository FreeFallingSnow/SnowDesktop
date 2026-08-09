#include "ui_animation_scheduler.h"
#include "ui_animation_scheduler_rules.h"

#include <windows.h>

#include <cstdlib>
#include <iostream>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

void WaitAndDispatch(
    snowdesktop::UiAnimationScheduler& scheduler,
    DWORD timeoutMilliseconds = 1000)
{
    Check(scheduler.WaitHandle() != nullptr,
        "scheduler exposes its waitable timer");
    Check(WaitForSingleObject(
            scheduler.WaitHandle(), timeoutMilliseconds) ==
            WAIT_OBJECT_0,
        "scheduled deadline becomes signaled");
    scheduler.DispatchDue();
}
}

int main()
{
    using snowdesktop::UiAnimationScheduler;
    using snowdesktop::UiAnimationSurface;
    namespace rules =
        snowdesktop::ui_animation_scheduler_rules;

    Check(rules::ClampTargetRefresh(360.0, false) == 240.0 &&
            rules::ClampTargetRefresh(165.0, false) == 165.0,
        "display cadence keeps the active rate and caps it at 240 Hz");
    Check(rules::ClampTargetRefresh(165.0, true) == 60.0,
        "software and remote sessions start at 60 Hz");
    Check(rules::AdvanceRepeatingDeadline(
            100.0, 16.0, 160.0) == 164.0,
        "missed intervals advance directly to the next future deadline");
    Check(rules::NextFrameDeadline(
            104.0, 100.0, 8.0) == 108.0 &&
            rules::NextFrameDeadline(
                109.0, 100.0, 8.0) == 109.0 &&
            rules::NextFrameDeadline(
                104.0, 0.0, 8.0) == 104.0,
        "successive one-shot frames retain display cadence while idle starts remain immediate");
    Check(rules::MissedFrameCount(
            100.0, 149.0, 16.0) == 3,
        "late frames count skipped presentation opportunities");
    Check(rules::ReduceAdaptiveDivisor(120.0, 1) == 2 &&
            rules::ReduceAdaptiveDivisor(60.0, 2) == 2,
        "adaptive cadence halves under load but never drops below 30 Hz");
    Check(rules::RecoveryWindowSatisfied(
            2000.0, 4.0, 10.0) &&
            !rules::RecoveryWindowSatisfied(
                1999.0, 4.0, 10.0),
        "adaptive cadence recovers only after two stable seconds");

    UiAnimationScheduler scheduler;
    Check(scheduler.Initialize(),
        "high-resolution scheduler initializes");
    Check(!scheduler.HasScheduledWork(),
        "new scheduler is idle");

    int cancelledCalls = 0;
    const auto cancelled = scheduler.ScheduleOnce(
        0, [&](auto) { ++cancelledCalls; });
    scheduler.Cancel(cancelled);
    scheduler.DispatchDue();
    Check(cancelledCalls == 0,
        "cancelled deadline never runs");
    Check(!scheduler.HasScheduledWork(),
        "cancelling the last deadline returns to idle");

    int deadlineCalls = 0;
    scheduler.ScheduleOnce(
        15, [&](auto) { ++deadlineCalls; });
    Check(deadlineCalls == 0,
        "future deadline does not run synchronously");
    WaitAndDispatch(scheduler);
    Check(deadlineCalls == 1,
        "single deadline runs exactly once");

    int mergedTimerCalls = 0;
    scheduler.ScheduleOnce(
        0, [&](auto) { ++mergedTimerCalls; });
    scheduler.ScheduleOnce(
        0, [&](auto) { ++mergedTimerCalls; });
    scheduler.DispatchDue();
    Check(mergedTimerCalls == 2,
        "same-cycle component deadlines share one dispatch pass");

    int firstFrames = 0;
    int secondFrames = 0;
    double firstTimestamp = 0.0;
    double secondTimestamp = 0.0;
    scheduler.StartAnimation(
        UiAnimationSurface::Desktop,
        [&](double timestamp) {
            ++firstFrames;
            firstTimestamp = timestamp;
            return false;
        });
    scheduler.StartAnimation(
        UiAnimationSurface::QuickNavigation,
        [&](double timestamp) {
            ++secondFrames;
            secondTimestamp = timestamp;
            return false;
        });
    WaitAndDispatch(scheduler);
    Check(firstFrames == 1 && secondFrames == 1,
        "same-cycle animation callbacks share one scheduler wakeup");
    Check(firstTimestamp == secondTimestamp,
        "independent animation surfaces advance from one frame timestamp");
    Check(!scheduler.HasScheduledWork(),
        "completed frame callbacks leave no active request");

    int deferredTrackFrames = 0;
    scheduler.StartAnimation(
        UiAnimationSurface::Popup,
        [&](double) {
            scheduler.StartAnimation(
                UiAnimationSurface::FloatingDock,
                [&](double) {
                    ++deferredTrackFrames;
                    return false;
                });
            return false;
        });
    WaitAndDispatch(scheduler);
    Check(deferredTrackFrames == 0 &&
            scheduler.HasScheduledWork(),
        "an animation started during a frame joins the next snapshot instead of re-entering the current batch");
    WaitAndDispatch(scheduler);
    Check(deferredTrackFrames == 1,
        "a newly started independent track advances on the next frame");

    int repeatCalls = 0;
    const auto repeating = scheduler.ScheduleInterval(
        5, [&](auto) { ++repeatCalls; });
    Sleep(35);
    scheduler.DispatchDue();
    Check(repeatCalls == 1,
        "missed repeating deadlines do not burst catch-up callbacks");
    Check(WaitForSingleObject(scheduler.WaitHandle(), 0) ==
            WAIT_TIMEOUT,
        "missed interval advances to a future deadline");
    WaitAndDispatch(scheduler);
    Check(repeatCalls == 2,
        "repeating deadline resumes on its next future period");
    scheduler.Cancel(repeating);

    int selfCancelledCalls = 0;
    scheduler.ScheduleInterval(
        1, [&](auto token) {
            ++selfCancelledCalls;
            scheduler.Cancel(token);
        });
    WaitAndDispatch(scheduler);
    Check(selfCancelledCalls == 1 &&
            !scheduler.HasScheduledWork(),
        "unload-style cancellation from a timer callback is safe");

    scheduler.SetDiagnosticsEnabled(true);
    int diagnosticFrames = 0;
    scheduler.StartAnimation(
        UiAnimationSurface::Popup,
        [&](double) {
            ++diagnosticFrames;
            Sleep(2);
            return false;
        });
    WaitAndDispatch(scheduler);
    const auto metrics = scheduler.Metrics();
    Check(metrics.enabled && diagnosticFrames == 1,
        "runtime diagnostics are opt-in and observe delivered frames");
    Check(metrics.requestedFrames == 1 &&
            metrics.deliveredFrames == 1 &&
            metrics.uiWorkP50Ms > 0.0,
        "diagnostics report requests, delivery and UI work percentiles");

    scheduler.CancelAll();
    Check(!scheduler.HasScheduledWork(),
        "cancel-all clears animations and timer deadlines");
    scheduler.Shutdown();

    std::cout << "ui animation scheduler tests passed\n";
    return 0;
}
