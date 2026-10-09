#include "ui/render/ui_animation_scheduler.h"
#include "ui/render/ui_animation_scheduler_rules.h"
#include "settings/animation_settings.h"
#include "layout/popup_animation_rules.h"
#include "system/status_bar/status_bar_shell_shortcut.h"
#include "system/status_bar/status_bar_task_view_input.h"
#include "system/status_bar/status_bar_activation.h"
#include "system/status_bar/status_bar_input_method.h"
#include "system/panel/system_panel_transition.h"

#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

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

template <typename Predicate>
bool PumpMessagesUntil(Predicate done, DWORD timeoutMilliseconds = 3000)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
    while (!done() && GetTickCount64() < deadline)
    {
        // Model a Shell/modal loop: it dispatches messages but knows nothing
        // about the application's waitable animation timer.
        MsgWaitForMultipleObjectsEx(0, nullptr, 10,
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        unsigned count = 0;
        while (++count <= 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return done();
}

constexpr UINT kBarContinuation = WM_APP + 45;
struct BarContinuationFixture
{
    snowdesktop::StatusBarActivationQueue queue;
    int messages = 0;
};
LRESULT CALLBACK BarContinuationProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto* state = reinterpret_cast<BarContinuationFixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        state = static_cast<BarContinuationFixture*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (message == kBarContinuation && state)
    {
        ++state->messages;
        if (auto callback = state->queue.Take()) callback();
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void TestStatusBarContinuationDispatch()
{
    // No desktop/Shell surface: exercise the production single-slot queue on a
    // real message-only HWND and preserve the scheduler's reentrancy boundary.
    constexpr wchar_t name[] = L"SnowDesktop.StatusBarContinuationTest";
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = name; cls.lpfnWndProc = BarContinuationProc;
    Check(RegisterClassW(&cls) != 0, "bar continuation fixture registers");
    BarContinuationFixture state;
    const HWND window = CreateWindowExW(0, name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
        nullptr, cls.hInstance, &state);
    Check(window != nullptr, "bar continuation fixture is message-only");
    snowdesktop::UiAnimationScheduler scheduler;
    bool scheduling = false;
    int delivered = 0, nestedDeadline = 0;
    scheduler.ScheduleOnce(0, [&](auto) {
        scheduling = true;
        Check(state.queue.Post(window, kBarContinuation, [&] {
            Check(!scheduling, "menu activation never runs in its scheduling callback");
            ++delivered;
            scheduler.ScheduleOnce(0, [&](auto) { ++nestedDeadline; });
            scheduler.DispatchDue();
        }), "scheduler posts activation to the bar HWND");
        Check(delivered == 0, "posting does not synchronously enter a modal surface");
        scheduling = false;
    });
    scheduler.DispatchDue();
    Check(PumpMessagesUntil([&] { return delivered == 1; }) && nestedDeadline == 1,
        "window-dispatched activation leaves the scheduler available to a nested menu loop");
    state.queue.Post(window, kBarContinuation, [&] { delivered += 10; });
    state.queue.Post(window, kBarContinuation, [&] { delivered += 100; });
    Check(PumpMessagesUntil([&] { return state.messages == 2; }) && delivered == 101,
        "rapid undelivered requests coalesce to one latest continuation");
    state.queue.Post(window, kBarContinuation, [&] { delivered += 1000; });
    state.queue.Cancel();
    Check(PumpMessagesUntil([&] { return state.messages == 3; }) && delivered == 101,
        "hiding cancels the callback even though its native message is already queued");
    state.queue.Post(window, kBarContinuation, [&] { ++delivered; });
    Check(PumpMessagesUntil([&] { return delivered == 102; }),
        "a later explicit click after cancellation remains a new deliverable action");
    DestroyWindow(window);
    UnregisterClassW(name, cls.hInstance);
}

void TestInputMethodTargetRecovery()
{
    namespace input = snowdesktop::status_bar_input_method;
    // Replace only Windows observations/activation. Use the production request
    // and real UI scheduler; no desktop host or machine input method is changed.
    struct Fixture
    {
        snowdesktop::UiAnimationScheduler scheduler;
        input::Snapshot sample, desktop, resolved;
        double now = 0;
        bool alive = true, windowsValid = true;
        HRESULT activation = S_OK, result = E_PENDING;
        unsigned activations = 0, desktopQueries = 0, completions = 0, selections = 0;
        snowdesktop::UiScheduleToken token = 0;
        std::wstring stage;
        void Start(std::optional<input::Snapshot> target = {})
        {
            input::TargetCallbacks callbacks;
            callbacks.sample = [this] { return sample; };
            callbacks.desktop = [this] { ++desktopQueries; return desktop; };
            callbacks.valid = [this](const auto& value) {
                return windowsValid && value.foreground && value.thread && value.layout;
            };
            callbacks.activate = [this](const auto&) { ++activations; return activation; };
            callbacks.current = [this](auto value) { return alive && token == value; };
            callbacks.nowMilliseconds = [this] { return now; };
            callbacks.trace = [this](const wchar_t* value, const auto&, const auto&, HRESULT, unsigned, double) {
                stage = value;
            };
            callbacks.finished = [this](auto, HRESULT value, input::Snapshot valueTarget) {
                ++completions; result = value; resolved = std::move(valueTarget);
                // The host can execute selection only after a successful handoff.
                if (value == S_OK) ++selections;
            };
            token = input::ScheduleTarget(scheduler, target, std::move(callbacks));
        }
        void Tick(double time) { now = time; WaitAndDispatch(scheduler); }
    };
    input::Snapshot app;
    app.foreground = reinterpret_cast<HWND>(1); app.focus = reinterpret_cast<HWND>(2);
    app.thread = 10; app.layout = reinterpret_cast<HKL>(0x0804);
    auto panel = app; panel.foreground = reinterpret_cast<HWND>(3); panel.focus = panel.foreground; panel.thread = 20;
    auto desktop = app; desktop.foreground = reinterpret_cast<HWND>(4); desktop.focus = nullptr; desktop.thread = 30;
    {
        Fixture f; f.desktop = desktop; f.Start(); f.Tick(16);
        Check(f.completions == 0 && f.desktopQueries == 0,
            "a transient empty foreground cannot produce an invalid selectable target or immediate desktop fallback");
        f.sample = app; f.Tick(32);
        Check(f.result == S_OK && f.resolved == app && f.selections == 1 && f.desktopQueries == 0 && !f.scheduler.HasScheduledWork(),
            "foreground recovery before the deadline opens the picker for the live app exactly once");
    }
    {
        Fixture f; f.desktop = desktop; f.Start(); f.Tick(249);
        Check(f.completions == 0, "desktop fallback does not bypass the acquisition deadline");
        f.Tick(250);
        Check(f.result == S_OK && f.resolved == desktop && f.stage == L"capture-desktop" && f.desktopQueries == 1,
            "persistent absence of a foreground uses the current Shell desktop without a cached app");
    }
    {
        Fixture f; f.Start(); f.Tick(250);
        Check(f.result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && f.selections == 0,
            "missing foreground and missing Shell cannot expose a selectable empty target");
    }
    {
        Fixture f; f.sample = app; f.sample.layout = nullptr; f.desktop = desktop; f.Start(); f.Tick(250);
        Check(f.result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && f.desktopQueries == 0,
            "a known but unavailable app is never replaced by the desktop");
    }
    {
        Fixture f; f.sample = panel; f.Start(app); f.Tick(16);
        Check(f.activations == 1 && f.completions == 0,
            "successful asynchronous activation is not mistaken for restored input focus");
        f.sample = app; f.sample.focus = nullptr; f.Tick(32);
        Check(f.selections == 0 && f.activations == 1, "missing child focus continues waiting without requesting activation again");
        f.sample = app; f.sample.layout = reinterpret_cast<HKL>(0x0409); f.Tick(48);
        Check(f.result == S_OK && f.resolved.layout == reinterpret_cast<HKL>(0x0409) && f.selections == 1 && !f.scheduler.HasScheduledWork(),
            "selection begins once after focus recovery and receives the current keyboard layout");
    }
    {
        Fixture f; f.sample = panel; f.activation = E_ACCESSDENIED; f.Start(app); f.Tick(16);
        Check(f.result == E_ACCESSDENIED && f.stage == L"restore-denied" && f.activations == 1 && f.selections == 0,
            "foreground activation denial has a distinct failure and never executes selection");
    }
    {
        Fixture f; f.sample = panel; f.Start(app); f.Tick(16); f.windowsValid = false; f.Tick(32);
        Check(f.result == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE) && f.activations == 1 && f.selections == 0,
            "destruction or reuse of the saved target cancels restoration before selection");
    }
    {
        Fixture f; f.sample = panel; f.Start(app); f.Tick(16); f.Tick(750);
        Check(f.result == HRESULT_FROM_WIN32(ERROR_TIMEOUT) && f.stage == L"restore-timeout" && f.activations == 1 && f.selections == 0,
            "an unresponsive target stops at the bounded deadline without repeated focus stealing");
    }
    {
        Fixture f; f.sample = panel; f.Start(app); f.Tick(16);
        f.sample = app; f.sample.foreground = reinterpret_cast<HWND>(5); f.Tick(32);
        Check(f.result == HRESULT_FROM_WIN32(ERROR_CANCELLED) && f.stage == L"restore-focus-moved" && f.selections == 0,
            "a newly activated third window cancels the old selection instead of stealing its focus");
    }
    {
        Fixture f; f.sample = panel; f.Start(app); f.alive = false; f.Tick(16);
        Check(f.result == HRESULT_FROM_WIN32(ERROR_CANCELLED) && f.activations == 0 && f.selections == 0,
            "hidden or destroyed host lifetime cancels before native activation");
    }
    {
        Fixture f; f.Start(); f.scheduler.Cancel(f.token); f.sample = app; f.Start(); f.Tick(16);
        Check(f.completions == 1 && f.selections == 1,
            "a replacement bar request removes the old deadline and delivers only the new target");
    }
    {
        Fixture f; f.sample = desktop; f.sample.focus = reinterpret_cast<HWND>(6); f.sample.thread = 40;
        f.Start(desktop); f.Tick(16);
        Check(f.result == S_OK && f.resolved.focus == f.sample.focus && f.activations == 0,
            "a desktop captured without child focus accepts its live child on restoration");
    }
}

void TestSystemPanelTransitionHandoff()
{
    // Same production queue as the popup. Native rendering/device services are
    // outside this boundary; resource-release reentry and message order are not.
    struct Request
    {
        int page, owner;
        bool powerConfirmation = false;
        bool SameTarget(const Request& other) const
        { return page == other.page && owner == other.owner && powerConfirmation == other.powerConfirmation; }
        int Monitor() const { return owner; }
    };
    using Transition = snowdesktop::SystemPanelTransition<Request>;
    using Action = Transition::Action;
    Transition transition;
    const std::optional<Request> current = Request{1, 10};
    Check(transition.Queue({2, 10}, current, true, false) == Action::Close,
        "switching pages first closes the current panel");
    Check(transition.Queue({3, 10}, current, true, true) == Action::Wait &&
        transition.Pending()->page == 3,
        "rapid switching while closing retains only the latest destination");
    Check(transition.Queue({3, 10}, current, true, true) == Action::Close && !transition.Pending(),
        "a second click on the pending destination cancels its opening");
    Check(transition.Queue({1, 10}, current, true, false) == Action::Close && !transition.Pending(),
        "clicking the current page closes it without scheduling a reopen");
    Check(transition.Queue({1, 10}, current, true, true) == Action::Wait,
        "an explicit click after dismissal may reopen the closing page");
    transition.Cancel();
    Check(transition.Queue({1, 20}, current, true, false) == Action::Close &&
        transition.Pending()->owner == 20,
        "the same page on another monitor transfers instead of toggling off");
    // Each fixture bar is on its own monitor. This is the cancellation path
    // taken when the old monitor's auto-hidden bar reports its disappearance.
    Check(!transition.CancelForMonitor(10) && transition.Pending()->owner == 20,
        "old-monitor hiding cannot discard the new-monitor popup request");
    Check(!transition.ShouldCancelOnDeactivation(10, 20),
        "activation of the destination bar retains cross-monitor handoff");
    Check(transition.ShouldCancelOnDeactivation(10, 30),
        "unrelated foreground activation still dismisses queued opening");
    Check(transition.CancelForMonitor(20) && !transition.Pending(),
        "hiding the destination monitor cancels its own queued popup");

    Check(transition.ShouldDismissForDpiChange(), "external DPI changes dismiss an idle popup");
    {
        auto placement = transition.BeginPlacement();
        Check(!transition.ShouldDismissForDpiChange(),
            "cross-monitor window placement cannot dismiss the destination on WM_DPICHANGED");
        {
            auto nested = transition.BeginPlacement();
            Check(!transition.ShouldDismissForDpiChange(), "nested placement preserves DPI ownership");
        }
        Check(!transition.ShouldDismissForDpiChange(), "inner placement cannot clear its parent's DPI ownership");
    }
    Check(transition.ShouldDismissForDpiChange(), "DPI dismissal resumes after destination placement");
    Check(transition.Queue({1, 10, true}, current, true, false) == Action::Close &&
        transition.Pending()->powerConfirmation,
        "a power confirmation is distinct from the control overview");
    transition.Cancel();

    constexpr wchar_t name[] = L"SnowDesktop.SystemPanelHandoffTest";
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = name; cls.lpfnWndProc = BarContinuationProc;
    Check(RegisterClassW(&cls) != 0, "panel handoff fixture registers");
    BarContinuationFixture state;
    const HWND window = CreateWindowExW(0, name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
        nullptr, cls.hInstance, &state);
    Check(window != nullptr, "panel handoff fixture is message-only");
    bool oldConsumerAttached = true;
    int opened = 0;
    const auto dispatch = [&] {
        if (auto request = transition.Take())
        {
            Check(!oldConsumerAttached, "new page never subscribes before old consumer release");
            opened = request->page;
        }
    };
    {
        auto release = transition.BeginRelease();
        Check(static_cast<bool>(release), "old panel acquires release ownership");
        // HideNow has detached its model and reset showing. This used to let a
        // nested click open immediately, then be erased by the old cleanup.
        Check(transition.Queue({2, 10}, {}, false, false) == Action::Wait,
            "click during consumer release waits despite the hidden HWND");
        Check(state.queue.Post(window, kBarContinuation, dispatch), "release callback posts nested opening");
        Check(PumpMessagesUntil([&] { return state.messages == 1; }) && opened == 0 &&
            transition.Pending()->page == 2,
            "nested message cannot consume or open the destination during cleanup");
        {
            auto nested = transition.BeginRelease();
            Check(!nested, "reentrant hiding cannot clean up the same panel twice");
        }
        Check(transition.Releasing(), "nested guard cannot end its parent's release");
        Check(transition.Queue({3, 10}, {}, false, false) == Action::Wait,
            "newest page supersedes the old request during release");
        oldConsumerAttached = false;
    }
    Check(state.queue.Post(window, kBarContinuation, dispatch), "release endpoint reposts opening");
    Check(PumpMessagesUntil([&] { return opened == 3; }) && !transition.Pending(),
        "release endpoint opens only the newest page exactly once");
    Check(transition.Queue({2, 10}, {}, false, true) == Action::Wait,
        "menu lifetime defers the next destination");
    transition.Cancel();
    dispatch();
    Check(opened == 3, "outside dismissal removes the queued destination");
    Check(transition.Queue({2, 10}, {}, false, false) == Action::Open,
        "a new click after cancellation remains deliverable");
    dispatch();
    Check(opened == 2, "later click opens after the old consumer is gone");
    DestroyWindow(window);
    UnregisterClassW(name, cls.hInstance);
}

void TestTaskViewTransition()
{
    using namespace snowdesktop;
    TaskViewTransitionGuard transition;
    Check(!transition.Busy(0) && transition.Begin(0, false), "first Task View request starts immediately");
    int accepted = 1;
    for (int time = 1; time < 350; ++time)
    {
        if (time == 50) transition.Observe(true, time);
        if (transition.Begin(time, true)) ++accepted;
    }
    Check(accepted == 1, "a burst during native opening produces one toggle and queues no replay");
    Check(!transition.Busy(400) && !transition.Begin(400, true) && !transition.Begin(5000, true),
        "Windows 10's visible bar cannot toggle an open Task View after animation or timeout");
    Check(!transition.Begin(5000, false), "an observed shown state also prevents a stale caller from reopening");
    transition.Observe(false, 6000);
    transition.Observe(false, 6200);
    Check(transition.Busy(6349) && !transition.Busy(6350),
        "external dismissal settles before reopening; duplicate hidden events do not extend it");
    Check(transition.Begin(6350, false), "a closed, settled view permits a new explicit request");
    transition.Observe(false, 6400);
    Check(transition.Busy(7849) && !transition.Busy(7850),
        "missing Shell callbacks on either Windows version cannot permanently block buttons");
    Check(transition.Begin(7850, false), "timeout permits recovery without replaying a queued toggle");
    transition.Observe(true, 9400);
    Check(!transition.Begin(10000, false), "a shown event arriving after timeout still blocks repeated opening");
    transition.Reset();
    Check(!transition.Busy(10001) && transition.Begin(10001, false),
        "Explorer restart or completely failed injection releases transition protection");
    transition.Reset();
    Check(!transition.Begin(11000, true), "Task View opened outside the bar cannot be toggled by its button");
    transition.Observe(true, 11000);
    transition.Observe(true, 11200);
    Check(!transition.Busy(11350) && !transition.CanBegin(11350, true),
        "duplicate shown events allow other native panels after settling but keep Task View open-only");
    Check(IsTaskViewTransitionSensitiveAction(StatusBarAction::TaskView) &&
        IsTaskViewTransitionSensitiveAction(StatusBarAction::Notifications) &&
        IsTaskViewTransitionSensitiveAction(StatusBarAction::SystemCalendar) &&
        IsTaskViewTransitionSensitiveAction(StatusBarAction::SystemControlCenter) &&
        !IsTaskViewTransitionSensitiveAction(StatusBarAction::Dismiss) &&
        !IsTaskViewTransitionSensitiveAction(StatusBarAction::Settings),
        "other native panels cannot interrupt Task View's transition, while dismissal and settings remain available");
}

void TestTaskViewMouseHandoff()
{
    using namespace snowdesktop;
    using Result = StatusBarShortcutResult;
    const auto chord = ResolveStatusBarShellChord(StatusBarAction::TaskView, true, false);
    const auto legacyChord = ResolveStatusBarShellChord(StatusBarAction::TaskView, false, true);
    Check(chord.pointerQuietMilliseconds == 0 && chord.waitForPointerRelease &&
        legacyChord.pointerQuietMilliseconds == 0 && legacyChord.waitForPointerRelease,
        "both Windows versions open Task View without a fixed double-click delay");
    UiAnimationScheduler scheduler;
    Check(scheduler.Initialize(), "Task View handoff scheduler initializes");
    // Run the production scheduler/input construction with only time, physical
    // input and SendInput replaced; never synthesize desktop input in tests.
    double now = 0;
    int held = 0;
    int sends = 0;
    bool current = true;
    std::vector<Result> outcomes;
    const auto queue = [&](UINT quiet = 0) {
        auto request = chord;
        request.pointerQuietMilliseconds = quiet;
        return ScheduleStatusBarShellShortcut(scheduler, request, {
            [&](int code) { return code == held; },
            [&](UINT count, INPUT*, int) { ++sends; return count; },
            [&](auto) { return current; },
            [&](auto, Result result) { outcomes.push_back(result); },
            [&] { return now; }});
    };
    queue();
    WaitAndDispatch(scheduler);
    Check(sends == 1 && outcomes == std::vector<Result>{Result::Sent} && !scheduler.HasScheduledWork(),
        "first Task View release sends at the next dispatch without waiting for double-click time");
    outcomes.clear(); held = VK_LBUTTON; queue();
    now = 499; WaitAndDispatch(scheduler);
    Check(sends == 1 && outcomes.empty(), "held pointer is never handed to an opening Shell surface");
    held = 0; WaitAndDispatch(scheduler);
    Check(sends == 2 && outcomes == std::vector<Result>{Result::Sent},
        "pointer release completes the request without starting a new quiet interval");

    // Negative control: the former production chord cannot meet the same
    // first-dispatch expectation. Keep that wait only as installation fallback.
    outcomes.clear(); now = 0; queue(500);
    now = 499; WaitAndDispatch(scheduler);
    Check(sends == 2 && outcomes.empty(), "fallback still finishes the double-click interval");
    held = VK_LBUTTON; WaitAndDispatch(scheduler);
    now = 1000; WaitAndDispatch(scheduler);
    Check(sends == 2, "fallback held second press cannot open Task View under the pointer");
    held = 0; now = 1499; WaitAndDispatch(scheduler);
    Check(sends == 2, "fallback release still leaves its quiet interval");
    now = 1500; WaitAndDispatch(scheduler);
    Check(sends == 3 && outcomes == std::vector<Result>{Result::Sent} && !scheduler.HasScheduledWork(),
        "fallback completed double-click opens exactly once, with no deferred replay");

    outcomes.clear(); queue(); current = false; WaitAndDispatch(scheduler);
    Check(sends == 3 && outcomes == std::vector<Result>{Result::Cancelled} && !scheduler.HasScheduledWork(),
        "foreground change, hidden bar or externally opened Task View cancels the delayed request");
    current = true; outcomes.clear(); queue();
    held = VK_RBUTTON; now = 7000; WaitAndDispatch(scheduler);
    Check(sends == 3 && outcomes == std::vector<Result>{Result::TimedOut} && !scheduler.HasScheduledWork(),
        "held mouse cannot leave an unbounded deferred opening request");
    held = 0; outcomes.clear();
    queue(5000); now = 12000; WaitAndDispatch(scheduler);
    Check(sends == 4 && outcomes == std::vector<Result>{Result::Sent},
        "maximum Windows double-click interval has its own budget before the input timeout");
}

void TestTaskViewPointerGuard()
{
    using snowdesktop::TaskViewPointerGuard;
    // Model the physical second click landing in Shell after it covers the
    // original button. Time and raw hook events are the only replaced inputs.
    TaskViewPointerGuard guard({-100, 0, -68, 32}, 500);
    Check(!guard.Filter(WM_LBUTTONDOWN, {-90, 16}, 100, false), "unarmed monitor leaves input alone");
    guard.Arm(100);
    Check(!guard.Filter(WM_RBUTTONDOWN, {-90, 16}, 110, false) &&
        !guard.Filter(WM_MOUSEMOVE, {-90, 16}, 110, false) &&
        !guard.Filter(WM_LBUTTONDOWN, {200, 16}, 110, false) &&
        !guard.Filter(WM_LBUTTONUP, {-90, 16}, 110, false),
        "other buttons, pointer motion, unrelated clicks and unmatched releases pass through");
    Check(!guard.Filter(WM_LBUTTONDOWN, {-90, 16}, 120, true), "injected clicks are not consumed");
    Check(guard.Filter(WM_LBUTTONDOWN, {-90, 16}, 130, false),
        "rapid physical repeat at the covered Task View button is intercepted");
    Check(!guard.Filter(WM_LBUTTONUP, {-90, 16}, 140, true),
        "injected release cannot complete a consumed physical press");
    Check(!guard.CanStop(800), "expiry cannot unhook halfway through a consumed press");
    Check(guard.Filter(WM_LBUTTONUP, {200, 200}, 810, false) && guard.CanStop(810),
        "matching physical release is consumed outside the button after expiry before unhooking");
    Check(!guard.Filter(WM_LBUTTONDOWN, {-90, 16}, 820, false), "expired shield allows later clicks");

    TaskViewPointerGuard cancelled({0, 0, 32, 32}, 5000);
    cancelled.Arm(0);
    Check(cancelled.Filter(WM_LBUTTONDOWN, {16, 16}, 1, false), "maximum double-click interval protects repeats");
    cancelled.Cancel();
    Check(!cancelled.CanStop(2) && cancelled.Filter(WM_LBUTTONUP, {16, 16}, 3, false) && cancelled.CanStop(3),
        "cancellation also drains a consumed press before removing its hook");
    TaskViewPointerGuard expired({0, 0, 32, 32}, 500);
    expired.Arm(0xffffff00u);
    Check(!expired.CanStop(0xf3u) && expired.CanStop(0xf4u) &&
        !expired.Filter(WM_LBUTTONDOWN, {16, 16}, 0xf4u, false),
        "deadline is bounded across Windows tick-counter wrap and wins before a new press");
}

HWND taskViewHookFixtureWindow = nullptr;
HOOKPROC taskViewHookFixtureCallback = nullptr;
DWORD taskViewHookFixtureThread = 0;
bool taskViewHookFixtureFails = false;
constexpr wchar_t kTaskViewHookFixtureClass[] = L"SnowDesktopTaskViewHookFixture";

HHOOK WINAPI InstallTaskViewHookFixture(int type, HOOKPROC callback, HINSTANCE instance, DWORD targetThread)
{
    Check(type == WH_MOUSE_LL && targetThread == 0, "Task View shield requests the low-level mouse boundary");
    taskViewHookFixtureThread = GetCurrentThreadId();
    taskViewHookFixtureCallback = callback;
    if (taskViewHookFixtureFails) return nullptr;
    taskViewHookFixtureWindow = CreateWindowExW(0, kTaskViewHookFixtureClass, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    return reinterpret_cast<HHOOK>(taskViewHookFixtureWindow);
}
BOOL WINAPI UninstallTaskViewHookFixture(HHOOK hook)
{
    Check(GetCurrentThreadId() == taskViewHookFixtureThread, "shield uninstalls on its own input thread");
    return DestroyWindow(reinterpret_cast<HWND>(hook));
}

void TestTaskViewInputMonitorLifetime()
{
    using namespace snowdesktop;
    WNDCLASSW windowClass{};
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kTaskViewHookFixtureClass;
    windowClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
        if (message == WM_APP + 1) return taskViewHookFixtureCallback(HC_ACTION, wp, lp);
        return DefWindowProcW(window, message, wp, lp);
    };
    Check(RegisterClassW(&windowClass) != 0, "Task View input fixture registers");
    const LowLevelMouseHook::Api api{&InstallTaskViewHookFixture, &UninstallTaskViewHookFixture};
    UiAnimationScheduler scheduler;
    Check(scheduler.Initialize(), "shield lifetime scheduler initializes");
    auto request = StatusBarTaskViewInputHandoff::Create(scheduler, {0, 0, 32, 32}, 5000, api);
    Check(request && taskViewHookFixtureThread != GetCurrentThreadId(),
        "shield receives raw input on a dedicated thread instead of the UI thread");
    request.reset(); // Model scheduler cancellation before SendInput.
    WaitAndDispatch(scheduler);
    Check(!scheduler.HasScheduledWork() && !IsWindow(taskViewHookFixtureWindow),
        "cancelled unarmed request releases its independent monitor without completion callback");

    request = StatusBarTaskViewInputHandoff::Create(scheduler, {0, 0, 32, 32}, 5000, api);
    Check(request != nullptr, "shield can be installed again after cancellation");
    request->Begin();
    MSLLHOOKSTRUCT down{}; down.pt = {16, 16};
    Check(SendMessageW(taskViewHookFixtureWindow, WM_APP + 1, WM_LBUTTONDOWN,
        reinterpret_cast<LPARAM>(&down)) == 1, "production hook callback consumes the covered-button repeat");
    request->Cancel(); request.reset();
    WaitAndDispatch(scheduler);
    Check(scheduler.HasScheduledWork() && IsWindow(taskViewHookFixtureWindow),
        "shortcut completion leaves the shield alive until its consumed release");
    down.pt = {200, 200};
    Check(SendMessageW(taskViewHookFixtureWindow, WM_APP + 1, WM_LBUTTONUP,
        reinterpret_cast<LPARAM>(&down)) == 1, "production hook callback pairs the release outside the button");
    WaitAndDispatch(scheduler);
    Check(!scheduler.HasScheduledWork() && !IsWindow(taskViewHookFixtureWindow),
        "cancelled consumed press uninstalls immediately after its paired release");
    request = StatusBarTaskViewInputHandoff::Create(scheduler, {0, 0, 32, 32}, 5000, api);
    Check(request != nullptr, "shield can restart for a later independent request");
    request->Begin(); request.reset(); scheduler.CancelAll();
    Check(!IsWindow(taskViewHookFixtureWindow), "scheduler shutdown joins and releases the shield");
    taskViewHookFixtureFails = true;
    Check(!StatusBarTaskViewInputHandoff::Create(scheduler, {0, 0, 32, 32}, 500, api) &&
        !scheduler.HasScheduledWork(), "hook installation failure returns the signal for the safe delayed fallback");
    taskViewHookFixtureFails = false;
    Check(UnregisterClassW(kTaskViewHookFixtureClass, windowClass.hInstance) != FALSE, "fixture unregisters after hook shutdown");
}

void TestStatusBarShellShortcuts()
{
    using namespace snowdesktop;
    using Action = StatusBarAction;
    using Result = StatusBarShortcutResult;
    // No global keyboard injection: only the OS key-state/input boundaries are
    // replaced. Production scheduling, cancellation and INPUT construction run.
    const auto request = ResolveStatusBarClick(Action::ControlCenter, true);
    Check(request == Action::SystemControlCenter && ResolveStatusBarClick(request, false) == request,
        "Ctrl click intent survives release while a nested menu unwinds");
    Check(ResolveStatusBarClick(Action::ControlCenter, false) == Action::ControlCenter &&
        ResolveStatusBarClick(Action::Dismiss, true) == Action::Dismiss,
        "ordinary click opens the local panel and blank click still dismisses with Ctrl held");
    StatusBarSettings settings;
    Check(ResolveStatusBarClick(Action::Calendar, false, settings, true) == Action::Calendar &&
        ResolveStatusBarClick(Action::ControlCenter, false, settings, true) == Action::ControlCenter,
        "new click options preserve the existing local-panel defaults");
    settings.clockSystemPanel = settings.controlCenterSystemPanel = true;
    Check(ResolveStatusBarClick(Action::Calendar, false, settings, false) == Action::SystemCalendar &&
        ResolveStatusBarClick(Action::ControlCenter, false, settings, true) == Action::SystemControlCenter &&
        ResolveStatusBarClick(Action::ControlCenter, true, settings, false) == Action::ControlCenter &&
        ResolveStatusBarClick(Action::SystemControlCenter, false, settings, false) == Action::ControlCenter,
        "native clock works on both OS versions but Windows 10 cannot select Quick Settings even with stored preferences or Ctrl");
    Check(!StatusBarSupportsSystemQuickSettings(10, 19045) && !StatusBarSupportsSystemQuickSettings(0, 0) &&
        StatusBarSupportsSystemQuickSettings(10, 22000), "Quick Settings uses the OS version rather than taskbar appearance");
    const auto legacyClock = ResolveStatusBarShellChord(Action::SystemCalendar, false, true);
    Check(legacyClock.key == 'D' && legacyClock.alt &&
        ResolveStatusBarShellChord(Action::SystemCalendar, true, true).key == 'N' &&
        ResolveStatusBarShellChord(Action::TaskView, false, true).key == VK_TAB &&
        !ResolveStatusBarShellChord(Action::SystemControlCenter, false, true).key,
        "clock and Task View select their own Shell entries without replacing the Windows 10 date panel with Action Center");

    UiAnimationScheduler scheduler;
    Check(scheduler.Initialize(), "status bar shortcut scheduler initializes");
    int held = VK_CONTROL;
    bool current = true;
    std::vector<std::vector<INPUT>> batches;
    UINT sentCount = 4;
    std::vector<Result> outcomes;
    const auto queue = [&](WORD key, UINT timeout = 5000, bool alt = false) {
        return ScheduleStatusBarShellShortcut(scheduler, StatusBarShellChord{key, alt}, {
            [&](int code) { return code == held; },
            [&](UINT count, INPUT* input, int size) {
                Check(size == sizeof(INPUT), "shortcut uses the native INPUT size");
                batches.emplace_back(input, input + count);
                return count >= 4 ? sentCount : count;
            },
            [&](auto) { return current; },
            [&](auto, Result result) { outcomes.push_back(result); }}, timeout);
    };
    const auto expectChord = [&](WORD key) {
        Check(batches.size() == 1 && batches.front().size() == 4,
            "system surface gets exactly one complete chord, with no modifier restoration");
        const auto& input = batches.front();
        const WORD keys[]{VK_LWIN, key, key, VK_LWIN};
        const DWORD flags[]{KEYEVENTF_EXTENDEDKEY, 0, KEYEVENTF_KEYUP, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP};
        for (std::size_t i = 0; i < 4; ++i)
            Check(input[i].type == INPUT_KEYBOARD && input[i].ki.wVk == keys[i] && input[i].ki.dwFlags == flags[i],
                "system shortcut has balanced Windows/key presses and releases");
        Check(outcomes == std::vector<Result>{Result::Sent} && !scheduler.HasScheduledWork(),
            "successful shortcut reports once and leaves no polling timer");
    };

    queue('A');
    for (const int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN, 0x41})
    {
        held = key;
        WaitAndDispatch(scheduler);
        Check(batches.empty() && outcomes.empty(), "physically held modifiers or chord key prevent injection");
    }
    held = 0;
    WaitAndDispatch(scheduler);
    expectChord('A');
    batches.clear(); outcomes.clear();
    queue('N'); WaitAndDispatch(scheduler); expectChord('N');
    batches.clear(); outcomes.clear();
    queue(VK_TAB); WaitAndDispatch(scheduler); expectChord(VK_TAB);
    batches.clear(); outcomes.clear();
    sentCount = 6;
    queue('D', 5000, true); held = VK_MENU; WaitAndDispatch(scheduler);
    Check(batches.empty() && outcomes.empty(), "date-panel shortcut waits for a physically held Alt key");
    held = 0; WaitAndDispatch(scheduler);
    Check(batches.size() == 1 && batches.front().size() == 6 && outcomes == std::vector<Result>{Result::Sent},
        "Windows 10 clock sends one complete Win+Alt+D chord");
    if (batches.size() == 1 && batches.front().size() == 6)
    {
        const WORD keys[]{VK_LWIN, VK_MENU, 'D', 'D', VK_MENU, VK_LWIN};
        for (std::size_t index = 0; index < 6; ++index)
            Check(batches.front()[index].ki.wVk == keys[index] &&
                ((batches.front()[index].ki.dwFlags & KEYEVENTF_KEYUP) != 0) == (index >= 3),
                "date-panel modifier presses and releases are balanced in reverse order");
    }
    batches.clear(); outcomes.clear(); sentCount = 4;

    held = VK_CONTROL;
    const auto cancelled = queue('A');
    WaitAndDispatch(scheduler);
    scheduler.Cancel(cancelled); // Same token used by blank clicks and bar disable.
    held = 0; scheduler.DispatchDue();
    Check(batches.empty() && outcomes.empty() && !scheduler.HasScheduledWork(),
        "blank click or shutdown cancels the pending shortcut before release");
    queue('A'); current = false; WaitAndDispatch(scheduler);
    Check(batches.empty() && outcomes == std::vector<Result>{Result::Cancelled} && !scheduler.HasScheduledWork(),
        "changed foreground, hidden owner or fullscreen guard cancels without input");
    current = true; outcomes.clear();
    queue('A', 0); WaitAndDispatch(scheduler);
    Check(batches.empty() && outcomes == std::vector<Result>{Result::TimedOut} && !scheduler.HasScheduledWork(),
        "expired shortcut never opens a system panel after the user moved on");
    outcomes.clear();

    for (UINT partial = 0; partial < 4; ++partial)
    {
        sentCount = partial;
        queue('A'); WaitAndDispatch(scheduler);
        Check(outcomes == std::vector<Result>{Result::Failed} && !scheduler.HasScheduledWork(),
            "input refusal or partial insertion is failure, never a successful system toggle");
        Check(batches.size() == (partial ? 2u : 1u), "failed chord is not retried");
        if (partial)
        {
            const auto& recovery = batches.back();
            Check(recovery.size() == (partial == 2 ? 2u : 1u) && recovery.back().ki.wVk == VK_LWIN,
                "partial input releases the injected Windows key");
            for (const auto& input : recovery)
                Check((input.ki.dwFlags & KEYEVENTF_KEYUP) != 0 &&
                    (input.ki.wVk == VK_LWIN || input.ki.wVk == 'A'),
                    "recovery only releases unmatched synthetic presses");
        }
        batches.clear(); outcomes.clear();
    }
    for (UINT partial = 1; partial < 6; ++partial)
    {
        sentCount = partial; queue('D', 5000, true); WaitAndDispatch(scheduler);
        Check(outcomes == std::vector<Result>{Result::Failed} && batches.size() == 2 && !scheduler.HasScheduledWork(),
            "partially inserted date-panel chord fails without retrying the system toggle");
        if (batches.size() == 2)
        {
            const std::vector<WORD> expected = partial == 1 || partial == 5 ? std::vector<WORD>{VK_LWIN} :
                partial == 2 || partial == 4 ? std::vector<WORD>{VK_MENU, VK_LWIN} : std::vector<WORD>{'D', VK_MENU, VK_LWIN};
            const auto& release = batches.back();
            Check(release.size() == expected.size(), "partial date chord releases only its remaining synthetic keys");
            for (std::size_t index = 0; index < release.size() && index < expected.size(); ++index)
                Check(release[index].ki.wVk == expected[index] && (release[index].ki.dwFlags & KEYEVENTF_KEYUP),
                    "date chord cleanup does not leave Alt pressed or release an unrelated key");
        }
        batches.clear(); outcomes.clear();
    }
}
}

int main()
{
    TestStatusBarContinuationDispatch();
    TestInputMethodTargetRecovery();
    TestSystemPanelTransitionHandoff();
    TestTaskViewTransition();
    TestTaskViewMouseHandoff();
    TestTaskViewPointerGuard();
    TestTaskViewInputMonitorLifetime();
    TestStatusBarShellShortcuts();
    namespace motion = snowdesktop::animation;
    Check(!motion::ResolveEnabled(motion::FollowSystem, false) &&
        motion::ResolveEnabled(motion::FollowSystem, true) &&
        motion::ResolveEnabled(motion::AlwaysOn, false) &&
        !motion::ResolveEnabled(motion::Disabled, true),
        "global preferences respect system choice, explicit enable and disable");
    Check(motion::ResolveFrameLimit(0, true, false, true, false) == 30 &&
        motion::ResolveFrameLimit(120, false, true, false, true) == 30 &&
        motion::ResolveFrameLimit(120, true, false, false, true) == 120 &&
        motion::ResolveFrameLimit(0, true, false, false, false) == 0,
        "battery preference is independent of system saver and never raises a cap");
    motion::SetRuntimePreferences(1, 2, 1, 30, false, false, 1);
    {
        snowdesktop::UiAnimationScheduler limited;
        limited.SetSoftwareRendering(true);
        Check(limited.Metrics().effectiveRefreshHz <= 30.001,
            "explicit limit applies to actual UI scheduler cadence");
    }
    // Isolate remaining scheduler tests from the machine's power policy.
    motion::SetRuntimePreferences(0, 2, 1, 0, false, false, 1);
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

    {
        // Production batch boundary; substitute only model state and pixels.
        // Closing one item while hover settles must present both final states
        // once, without coupling the two animation lifetimes.
        UiAnimationScheduler batch;
        int dockOwner = 0, secondDockOwner = 0;
        int hover = 0, presence = 0, paints = 0, secondPaints = 0;
        int paintedHover = 0, paintedPresence = 0;
        const auto request = [&] {
            batch.RequestFramePresentation(&dockOwner, [&] {
                ++paints;
                paintedHover = hover;
                paintedPresence = presence;
                batch.DispatchDue(); // Paint/native-menu reentry is guarded.
            });
        };
        batch.StartAnimation(UiAnimationSurface::FloatingDock, [&](double) {
            ++hover;
            request();
            return hover < 2;
        });
        batch.StartAnimation(UiAnimationSurface::FloatingDock, [&](double) {
            ++presence;
            request();
            return false;
        });
        batch.StartAnimation(UiAnimationSurface::FloatingDock, [&](double) {
            batch.RequestFramePresentation(&secondDockOwner, [&] { ++secondPaints; });
            return false;
        });
        WaitAndDispatch(batch);
        Check(paints == 1 && secondPaints == 1 && paintedHover == 1 && paintedPresence == 1,
            "one Dock presents both latest states once, independently of other Dock owners");
        Check(batch.HasScheduledWork(), "presence completion cannot stop the hover track");
        WaitAndDispatch(batch);
        Check(paints == 2 && paintedHover == 2 && paintedPresence == 1 && !batch.HasScheduledWork(),
            "the surviving hover track presents its terminal frame and then stops");
        request();
        Check(paints == 3,
            "same-bounds content or immediate pointer feedback presents synchronously outside an animation snapshot");

        batch.StartAnimation(UiAnimationSurface::FloatingDock, [&](double) -> bool {
            request();
            throw std::runtime_error("failed model callback after invalidation");
        });
        WaitAndDispatch(batch);
        Check(paints == 4 && !batch.HasScheduledWork(),
            "a failed animation callback cannot strand its pending terminal presentation");

        // Shell modal pump uses the same end-of-snapshot presentation boundary.
        batch.StartAnimation(UiAnimationSurface::FloatingDock, [&](double) {
            ++hover; request(); return false;
        });
        batch.StartAnimation(UiAnimationSurface::FloatingDock, [&](double) {
            ++presence; request(); return false;
        });
        UiAnimationScheduler::MessagePumpScope pump(batch, [&] {
            Check(paintedHover == hover && paintedPresence == presence,
                "modal presentation flush observes both updated animation states");
        });
        Check(pump.IsAvailable(), "batch regression creates the real message-only Shell pump");
        const ULONGLONG deadline = GetTickCount64() + 3000;
        while (batch.HasScheduledWork() && GetTickCount64() < deadline)
        {
            MSG message{};
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
            else MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        Check(paints == 5 && !batch.HasScheduledWork(),
            "a nested Shell loop coalesces the two Dock tracks before flushing");

        int cancelledPaints = 0;
        batch.StartAnimation(UiAnimationSurface::FloatingDock, [&](double) {
            batch.RequestFramePresentation(&dockOwner, [&] { batch.CancelAll(); });
            batch.RequestFramePresentation(&secondDockOwner, [&] { ++cancelledPaints; });
            return false;
        });
        WaitAndDispatch(batch);
        Check(cancelledPaints == 0 && !batch.HasScheduledWork(),
            "cancellation during presentation prevents later owners from publishing a retired batch");

        int oldHover = 0, oldPresence = 0, oldPaints = 0;
        bool oldMixedFrame = false;
        UiAnimationScheduler eager;
        const auto oldPaint = [&] {
            ++oldPaints;
            oldMixedFrame |= oldHover != oldPresence;
        };
        eager.StartAnimation(UiAnimationSurface::FloatingDock, [&](double) {
            ++oldHover; oldPaint(); return false;
        });
        eager.StartAnimation(UiAnimationSurface::FloatingDock, [&](double) {
            ++oldPresence; oldPaint(); return false;
        });
        WaitAndDispatch(eager);
        Check(oldPaints == 2 && oldMixedFrame,
            "negative control: presenting inside each track exposes a mixed frame and duplicate paint");
    }

    {
        // Protect against replaying an old pose after a slow component update.
        // Only event ordering is asserted, with no wall-clock latency ceiling.
        UiAnimationScheduler busyScheduler;
        double timerFinished = 0.0;
        double popupTimestamp = 0.0;
        double dockTimestamp = 0.0;
        busyScheduler.StartAnimation(UiAnimationSurface::Popup,
            [&](double timestamp) {
                popupTimestamp = timestamp;
                return false;
            });
        busyScheduler.StartAnimation(UiAnimationSurface::FloatingDock,
            [&](double timestamp) {
                dockTimestamp = timestamp;
                return false;
            });
        busyScheduler.ScheduleOnce(0, [&](auto) {
            Sleep(25);
            timerFinished = UiAnimationScheduler::MonotonicMilliseconds();
        });
        busyScheduler.DispatchDue();
        Check(timerFinished > 0.0 && popupTimestamp >= timerFinished,
            "animation progress must use a timestamp sampled after slow timer work");
        Check(popupTimestamp == dockTimestamp,
            "surfaces must still share one fresh timestamp after slow timer work");
        Check(!busyScheduler.HasScheduledWork(),
            "a delayed frame completes in one dispatch without catch-up requests");
    }

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
    Check(scheduler.HasScheduledWork(),
        "the repeating interval remains scheduled after a missed deadline");
    WaitAndDispatch(scheduler);
    Check(repeatCalls == 2,
        "the repeating interval delivers exactly one callback at the next wake");
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

    {
        // Regression: the native shrink reaches its endpoint while a Shell
        // elevation wait starves the callback that hides the popup HWND.
        UiAnimationScheduler modalScheduler;
        snowdesktop::popup_animation_rules::State popup;
        popup.ShowImmediately();
        popup.Close(static_cast<std::uint64_t>(
            UiAnimationScheduler::MonotonicMilliseconds()));
        int completions = 0;
        int presented = 0;
        int frames = 0;
        modalScheduler.ScheduleOnce(90, [&](auto) {
            popup.Advance(static_cast<std::uint64_t>(
                UiAnimationScheduler::MonotonicMilliseconds()));
            ++completions;
        });
        modalScheduler.StartAnimation(UiAnimationSurface::FloatingDock,
            [&](double) { return ++frames < 2; });
        {
            UiAnimationScheduler::MessagePumpScope pump(
                modalScheduler, [&]() { ++presented; });
            Check(pump.IsAvailable(), "Shell message-loop bridge initializes");
            UiAnimationScheduler::MessagePumpScope nestedPump(
                modalScheduler, [&]() { ++presented; });
            Check(nestedPump.IsAvailable(), "nested Shell invocation bridge initializes");
            Check(PumpMessagesUntil([&]() { return !modalScheduler.HasScheduledWork(); }),
                "popup completion and frame callbacks run inside a message-only Shell loop");
        }
        modalScheduler.DispatchDue();
        Check(popup.IsHidden() && completions == 1,
            "a closing popup reaches hidden exactly once inside the Shell loop");
        Check(frames == 2, "independent frame callbacks also finish inside the Shell loop");
        Check(presented > 0,
            "whichever nested invocation delivers work also flushes its presentation");
    }

    {
        UiAnimationScheduler modalScheduler;
        int laterCalls = 0;
        bool insideCallback = false;
        bool reentered = false;
        modalScheduler.ScheduleOnce(0, [&](auto) {
            insideCallback = true;
            modalScheduler.ScheduleOnce(0, [&](auto) {
                reentered = insideCallback;
                ++laterCalls;
            });
            modalScheduler.DispatchDue();
            const ULONGLONG end = GetTickCount64() + 40;
            PumpMessagesUntil([&]() { return GetTickCount64() >= end; });
            Check(laterCalls == 0,
                "a callback's nested modal loop cannot reenter the scheduler snapshot");
            insideCallback = false;
        });
        {
            UiAnimationScheduler::MessagePumpScope pump(modalScheduler, {});
            Check(PumpMessagesUntil([&]() { return laterCalls == 1; }),
                "a deadline deferred by reentrancy runs after the outer callback returns");
        }
        Check(!reentered, "nested Shell loops never reenter an active scheduler callback");

        int afterScopeCalls = 0;
        {
            UiAnimationScheduler::MessagePumpScope pump(modalScheduler, {});
            modalScheduler.ScheduleOnce(15, [&](auto) { ++afterScopeCalls; });
        }
        const ULONGLONG end = GetTickCount64() + 40;
        PumpMessagesUntil([&]() { return GetTickCount64() >= end; });
        Check(afterScopeCalls == 0,
            "leaving a Shell command removes its message-loop bridge");
        WaitAndDispatch(modalScheduler);
        Check(afterScopeCalls == 1,
            "leaving a Shell command preserves deadlines for the normal host loop");
    }

    std::cout << "ui animation scheduler tests passed\n";
    return 0;
}
