#pragma once
#include "status_bar.h"
#include "ui/render/ui_animation_scheduler.h"
#include <algorithm>
#include <memory>
#include <utility>

namespace snowdesktop
{
inline constexpr bool StatusBarSupportsSystemQuickSettings(DWORD major, DWORD build)
{ return major > 10 || (major == 10 && build >= 22000); }
inline bool StatusBarSupportsSystemQuickSettings()
{
    static const bool supported = [] {
        using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto getVersion = reinterpret_cast<GetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW version{sizeof(version)};
        return getVersion && getVersion(&version) == 0 &&
            StatusBarSupportsSystemQuickSettings(version.dwMajorVersion, version.dwBuildNumber);
    }();
    return supported;
}
// Resolve the click before a nested menu unwinds: Ctrl may be released while
// that happens, but it must not turn a system-panel request into our panel.
inline StatusBarAction ResolveStatusBarClick(StatusBarAction action, bool controlDown)
{
    return action == StatusBarAction::ControlCenter && controlDown ?
        StatusBarAction::SystemControlCenter : action;
}
inline StatusBarAction ResolveStatusBarClick(StatusBarAction action, bool controlDown,
    const StatusBarSettings& settings, bool systemQuickSettings)
{
    if (action == StatusBarAction::Calendar && settings.clockSystemPanel) return StatusBarAction::SystemCalendar;
    if (action == StatusBarAction::ControlCenter && systemQuickSettings &&
        (controlDown || settings.controlCenterSystemPanel)) return StatusBarAction::SystemControlCenter;
    if (action == StatusBarAction::SystemControlCenter && !systemQuickSettings) return StatusBarAction::ControlCenter;
    return action;
}

struct StatusBarShellChord
{
    WORD key = 0;
    bool alt = false;
    UINT pointerQuietMilliseconds = 0;
    bool waitForPointerRelease = false;
};
inline StatusBarShellChord ResolveStatusBarShellChord(StatusBarAction action, bool windows11, bool classicTaskbar)
{
    // The caller shields repeated presses at the opening button before sending
    // Win+Tab. Wait only for held input, not the full double-click interval.
    if (action == StatusBarAction::TaskView) return {VK_TAB, false, 0, true};
    if (action == StatusBarAction::SystemCalendar) return windows11 ? StatusBarShellChord{'N'} : StatusBarShellChord{'D', true};
    if (action == StatusBarAction::SystemControlCenter) return windows11 ? StatusBarShellChord{'A'} : StatusBarShellChord{};
    if (action == StatusBarAction::Notifications) return {static_cast<WORD>(classicTaskbar ? 'A' : 'N')};
    return {};
}

enum class StatusBarShortcutResult { Sent, Cancelled, TimedOut, Failed };

// Sending Win+Tab finishes input injection, not the Shell's transition. Keep
// that transition separate from the cancellable status-bar continuation so a
// second click cannot cancel its protection and immediately toggle it again.
// Dropped clicks are never replayed after the Shell finishes its animation.
class TaskViewTransitionGuard
{
public:
    bool Busy(double now) const noexcept
    {
        return now < settledAt_ || (awaitingShown_ && now < deadline_);
    }
    bool CanBegin(double now, bool currentlyVisible) const noexcept
    {
        // On Windows 10 the bar remains clickable in Task View. Its button is
        // open-only: a visible view is never toggled, even after the timeout.
        return !currentlyVisible && !visible_ && !Busy(now);
    }
    bool Begin(double now, bool currentlyVisible) noexcept
    {
        if (!CanBegin(now, currentlyVisible)) return false;
        awaitingShown_ = true;
        // Visibility callbacks may precede the native animation's endpoint.
        settledAt_ = now + 350;
        deadline_ = now + 1500;
        return true;
    }
    void Observe(bool visible, double now) noexcept
    {
        if (visible == visible_) return;
        visible_ = visible;
        awaitingShown_ = false;
        // Opening externally and dismissing with Escape/a selection also need
        // settling time; duplicate or initial hidden events cannot release or
        // indefinitely extend an in-flight opening request.
        settledAt_ = (std::max)(settledAt_, now + 350);
    }
    void Reset() noexcept { *this = {}; }
private:
    double deadline_ = 0;
    double settledAt_ = 0;
    bool awaitingShown_ = false;
    bool visible_ = false;
};

inline bool IsTaskViewTransitionSensitiveAction(StatusBarAction action) noexcept
{
    return action == StatusBarAction::TaskView || action == StatusBarAction::Notifications ||
        action == StatusBarAction::SystemCalendar || action == StatusBarAction::SystemControlCenter;
}

struct StatusBarShortcutCallbacks
{
    std::function<bool(int)> keyDown;
    std::function<UINT(UINT, INPUT*, int)> send;
    std::function<bool(UiScheduleToken)> current;
    std::function<void(UiScheduleToken, StatusBarShortcutResult)> finished;
    std::function<double()> nowMilliseconds = UiAnimationScheduler::MonotonicMilliseconds;
};

// Do not synthesize release/restore pairs for physically held modifiers. Wait
// asynchronously for the user's release instead. The caller cancels this same
// token on another bar action, dismissal or shutdown; the context guard also
// rejects a hidden bar, fullscreen or a changed foreground window.
inline UiScheduleToken ScheduleStatusBarShellShortcut(UiAnimationScheduler& scheduler,
    StatusBarShellChord chord, StatusBarShortcutCallbacks callbacks, UINT timeoutMilliseconds = 5000)
{
    // DispatchDue copies callbacks so cancellation/reentrancy cannot invalidate
    // the active call. Keep the mutable gesture deadline in request-owned state.
    const auto notBefore = std::make_shared<double>(callbacks.nowMilliseconds() + chord.pointerQuietMilliseconds);
    const double deadline = *notBefore + timeoutMilliseconds;
    return scheduler.ScheduleInterval(16,
        [&scheduler, chord, deadline, notBefore, callbacks = std::move(callbacks)](UiScheduleToken token) {
            const auto finish = [&](StatusBarShortcutResult result) {
                scheduler.Cancel(token);
                callbacks.finished(token, result);
            };
            if (!callbacks.current(token)) { finish(StatusBarShortcutResult::Cancelled); return; }
            if (!chord.key) { finish(StatusBarShortcutResult::Failed); return; }
            const double now = callbacks.nowMilliseconds();
            if (now >= deadline)
            { finish(StatusBarShortcutResult::TimedOut); return; }
            if (chord.waitForPointerRelease || chord.pointerQuietMilliseconds)
            {
                for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
                    if (callbacks.keyDown(button))
                    { *notBefore = now + chord.pointerQuietMilliseconds; return; }
                if (now < *notBefore) return;
            }
            for (const int modifier : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN})
                if (callbacks.keyDown(modifier)) return;
            if (callbacks.keyDown(chord.key)) return;

            INPUT input[6]{};
            for (auto& item : input) item.type = INPUT_KEYBOARD;
            const UINT presses = chord.alt ? 3u : 2u, count = presses * 2;
            const WORD keys[]{VK_LWIN, chord.alt ? static_cast<WORD>(VK_MENU) : chord.key, chord.key};
            for (UINT index = 0; index < presses; ++index)
            {
                input[index].ki.wVk = keys[index];
                input[index].ki.dwFlags = index == 0 ? KEYEVENTF_EXTENDEDKEY : 0;
                input[count - 1 - index] = input[index];
                input[count - 1 - index].ki.dwFlags |= KEYEVENTF_KEYUP;
            }
            const UINT sent = callbacks.send(count, input, sizeof(INPUT));
            if (sent > 0 && sent < count)
            {
                // Best-effort release of only our unmatched synthetic presses.
                // Never retry the chord: it toggles the system surface.
                INPUT release[3]{}; UINT releases = 0;
                for (UINT index = presses; index-- > 0;)
                    if (sent > index && sent <= count - 1 - index)
                        release[releases++] = input[count - 1 - index];
                if (releases) callbacks.send(releases, release, sizeof(INPUT));
            }
            finish(sent == count ? StatusBarShortcutResult::Sent : StatusBarShortcutResult::Failed);
        });
}
inline UiScheduleToken ScheduleStatusBarShellShortcut(UiAnimationScheduler& scheduler,
    WORD key, StatusBarShortcutCallbacks callbacks, UINT timeoutMilliseconds = 5000)
{ return ScheduleStatusBarShellShortcut(scheduler, StatusBarShellChord{key}, std::move(callbacks), timeoutMilliseconds); }
}
