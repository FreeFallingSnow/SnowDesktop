#include "ui/render/ui_animation_scheduler.h"
#include "ui/render/ui_animation_scheduler_rules.h"
#include "ui/render/ui_presentation_scope.h"
#include "settings/animation_settings.h"
#include "layout/popup_animation_rules.h"
#include "dock/dock_magnification.h"
#include "system/status_bar/status_bar_shell_shortcut.h"
#include "system/status_bar/status_bar_task_view_input.h"
#include "system/status_bar/status_bar_shell_shortcut_relay.h"
#include "system/status_bar/status_bar_activation.h"
#include "system/status_bar/status_bar_input_method.h"
#include "system/panel/system_panel_transition.h"

#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <limits>
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

struct ScheduledNativeInput
{
    snowdesktop::UiAnimationScheduler* scheduler = nullptr;
    int owner = 0;
    int paints = 0;
    bool synchronous = false;
};

LRESULT CALLBACK ScheduledNativeInputProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    const snowdesktop::UiImmediatePresentationScope immediatePresentation;
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
            reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* input = reinterpret_cast<ScheduledNativeInput*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (input && message == WM_APP + 91)
    {
        const int before = input->paints;
        input->scheduler->RequestScheduledPresentation(&input->owner,
            [input] { ++input->paints; return true; });
        input->synchronous = input->paints == before + 1;
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
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
    transition.Reset();
    Check(transition.Begin(100, false), "search interleave begins one Task View opening");
    for (const auto action : {StatusBarAction::QuickSearch, StatusBarAction::TaskView,
        StatusBarAction::Calendar, StatusBarAction::Tray, StatusBarAction::MenuQuickSearch,
        StatusBarAction::Dismiss, StatusBarAction::Settings, StatusBarAction::ShowDesktop,
        StatusBarAction::Notifications, StatusBarAction::SystemCalendar, StatusBarAction::SystemControlCenter})
    {
        Check(!transition.AllowsAction(action, 376, false),
            "adjacent search and all surface-changing actions are rejected before Shell settles");
        Check(transition.AllowsAction(action, 1600, false),
            "missing visibility notification releases each action at the bounded deadline");
    }
    transition.Observe(true, 400);
    Check(transition.Remaining(400) == 350 && !transition.AllowsAction(StatusBarAction::QuickSearch, 749, true) &&
        transition.AllowsAction(StatusBarAction::QuickSearch, 750, true),
        "search waits for the actual shown notification plus settling, not a double-click interval");
}

void TestTaskViewMouseHandoff()
{
    using namespace snowdesktop;
    using Result = StatusBarShortcutResult;
    const auto chord = ResolveStatusBarShellChord(StatusBarAction::TaskView, true, false);
    const auto legacyChord = ResolveStatusBarShellChord(StatusBarAction::TaskView, false, true);
    Check(chord.waitForPointerRelease && legacyChord.waitForPointerRelease,
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
    const auto queue = [&] {
        return ScheduleStatusBarShellShortcut(scheduler, chord, {
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

    outcomes.clear(); queue(); current = false; WaitAndDispatch(scheduler);
    Check(sends == 2 && outcomes == std::vector<Result>{Result::Cancelled} && !scheduler.HasScheduledWork(),
        "foreground change, hidden bar or externally opened Task View cancels the delayed request");
    current = true; outcomes.clear(); queue();
    held = VK_RBUTTON; now = 7000; WaitAndDispatch(scheduler);
    Check(sends == 2 && outcomes == std::vector<Result>{Result::TimedOut} && !scheduler.HasScheduledWork(),
        "held mouse cannot leave an unbounded deferred opening request");
}

void TestTaskViewPointerGuard()
{
    using snowdesktop::TaskViewPointerGuard;
    // Task View is [-100,-68), adjacent search is [-68,-36). Include every
    // bar, including a second monitor; no desktop input is synthesized.
    TaskViewPointerGuard guard({{-100, 0, -4, 32}, {1000, 0, 1300, 32}});
    Check(!guard.Filter(WM_LBUTTONDOWN, {-90, 16}, 100, false), "unarmed monitor leaves input alone");
    Check(!guard.CanStop(100), "pre-injection physical input cannot retire the pending monitor");
    guard.Arm(100, 1500);
    Check(!guard.Filter(WM_MOUSEMOVE, {-90, 16}, 110, false) &&
        !guard.Filter(WM_LBUTTONDOWN, {200, 16}, 110, false) &&
        !guard.Filter(WM_LBUTTONUP, {-90, 16}, 110, false),
        "pointer motion, clicks outside the bars and unmatched releases pass through");
    Check(!guard.Filter(WM_LBUTTONDOWN, {-90, 16}, 120, true), "injected clicks are not consumed");
    Check(guard.Filter(WM_LBUTTONDOWN, {-50, 16}, 376, false),
        "the original adjacent-search click is intercepted before it can dismiss Shell or open search");
    Check(guard.Filter(WM_RBUTTONDOWN, {1100, 16}, 400, false) &&
        guard.Filter(WM_MBUTTONDOWN, {-90, 16}, 401, false) &&
        guard.Filter(WM_XBUTTONDOWN, {-90, 16}, 402, false, XBUTTON1 << 16) &&
        guard.Filter(WM_XBUTTONDOWN, {-90, 16}, 403, false, XBUTTON2 << 16),
        "other buttons and other monitor bars cannot insert a menu or activation during startup");
    Check(!guard.Filter(WM_LBUTTONUP, {-90, 16}, 410, true),
        "injected release cannot complete a consumed physical press");
    Check(!guard.CanStop(1600), "expiry cannot unhook halfway through consumed presses");
    Check(guard.Filter(WM_LBUTTONUP, {200, 200}, 1610, false) && !guard.CanStop(1610) &&
        guard.Filter(WM_RBUTTONUP, {200, 200}, 1611, false) &&
        guard.Filter(WM_MBUTTONUP, {200, 200}, 1612, false) &&
        guard.Filter(WM_XBUTTONUP, {200, 200}, 1613, false, XBUTTON1 << 16) &&
        !guard.CanStop(1613) && guard.Filter(WM_XBUTTONUP, {200, 200}, 1614, false, XBUTTON2 << 16) &&
        guard.CanStop(1614), "every consumed button drains its own release after timeout, outside all bars");
    Check(!guard.Filter(WM_LBUTTONDOWN, {-90, 16}, 1620, false), "expired shield allows later clicks");

    TaskViewPointerGuard cancelled({{0, 0, 64, 32}});
    cancelled.Arm(0, 1500);
    Check(cancelled.Filter(WM_LBUTTONDOWN, {48, 16}, 1, false), "search press is consumed before cancellation");
    cancelled.Cancel();
    Check(!cancelled.CanStop(2) && cancelled.Filter(WM_LBUTTONUP, {16, 16}, 3, false) && cancelled.CanStop(3),
        "cancellation also drains a consumed press before removing its hook");
    TaskViewPointerGuard settled({{0, 0, 64, 32}});
    settled.Arm(0, 1500); settled.UpdateDeadline(100, 350);
    Check(!settled.CanStop(449) && settled.CanStop(450),
        "shown notification shortens raw-input protection to its settling deadline");
    TaskViewPointerGuard expired({{0, 0, 64, 32}});
    expired.Arm(0xffffff00u, 500);
    Check(!expired.CanStop(0xf3u) && expired.CanStop(0xf4u) &&
        !expired.Filter(WM_LBUTTONDOWN, {16, 16}, 0xf4u, false),
        "deadline is bounded across Windows tick-counter wrap and wins before a new press");

    // A device switch may omit a consumed release. The next delivered press
    // must keep its own release, regardless of why the protection lets it pass.
    struct Button { WPARAM down, up; DWORD data; };
    const Button buttons[]{{WM_LBUTTONDOWN, WM_LBUTTONUP, 0}, {WM_RBUTTONDOWN, WM_RBUTTONUP, 0},
        {WM_MBUTTONDOWN, WM_MBUTTONUP, 0}, {WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON1 << 16},
        {WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON2 << 16}};
    for (const auto& button : buttons)
    {
        for (int scenario = 0; scenario < 4; ++scenario)
        {
            TaskViewPointerGuard resumed({{0, 0, 64, 32}});
            resumed.Arm(100, 200);
            Check(resumed.Filter(button.down, {16, 16}, 110, false, button.data),
                "initial press is consumed before its release is lost");
            if (scenario == 1) resumed.Cancel();
            const DWORD now = scenario >= 2 ? 300 : 120;
            const POINT point = scenario == 0 || scenario == 3 ? POINT{200, 200} : POINT{16, 16};
            Check(!resumed.Filter(button.down, point, now, false, button.data),
                "fresh press outside protection is delivered after a missing release");
            Check(!resumed.Filter(button.up, point, now + 1, false, button.data),
                "fresh delivered press keeps its matching release after a device switch");
            if (scenario == 0)
            {
                Check(!resumed.CanStop(now + 1), "outside click does not cancel active bar protection");
                Check(resumed.Filter(button.down, {16, 16}, now + 2, false, button.data) &&
                    resumed.Filter(button.up, {200, 200}, now + 3, false, button.data),
                    "later protected click still consumes both edges");
                resumed.Cancel();
            }
            Check(resumed.CanStop(now + 4), "missing old release cannot retain the monitor after a fresh click");
        }
    }
    TaskViewPointerGuard independent({{0, 0, 64, 32}});
    independent.Arm(100, 200);
    Check(independent.Filter(WM_LBUTTONDOWN, {16, 16}, 110, false) &&
        independent.Filter(WM_RBUTTONDOWN, {16, 16}, 111, false), "two physical presses are protected");
    independent.Cancel();
    Check(!independent.Filter(WM_RBUTTONDOWN, {200, 200}, 120, true) &&
        !independent.Filter(WM_RBUTTONUP, {200, 200}, 121, true),
        "injected click cannot replace physical release ownership");
    Check(!independent.Filter(WM_LBUTTONDOWN, {200, 200}, 122, false) &&
        !independent.Filter(WM_LBUTTONUP, {200, 200}, 123, false) && !independent.CanStop(123),
        "fresh left click preserves another button's outstanding release");
    Check(independent.Filter(WM_RBUTTONUP, {200, 200}, 124, false) && independent.CanStop(124),
        "other physical release still drains its original consumed press");
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
    const std::vector<RECT> bounds{{0, 0, 96, 32}, {-1920, 0, -1800, 32}};
    auto remaining = [] { return 1500.0; };
    auto request = StatusBarTaskViewInputHandoff::Create(scheduler, bounds, remaining, api);
    Check(request && taskViewHookFixtureThread != GetCurrentThreadId(),
        "shield receives raw input on a dedicated thread instead of the UI thread");
    MSLLHOOKSTRUCT down{}; down.pt = {48, 16};
    Check(SendMessageW(taskViewHookFixtureWindow, WM_APP + 1, WM_LBUTTONDOWN,
        reinterpret_cast<LPARAM>(&down)) == 0, "unarmed production hook passes input without retiring");
    WaitAndDispatch(scheduler);
    Check(scheduler.HasScheduledWork() && IsWindow(taskViewHookFixtureWindow),
        "pre-injection raw events cannot uninstall a pending handoff");
    request.reset(); // Model scheduler cancellation before SendInput.
    WaitAndDispatch(scheduler);
    Check(!scheduler.HasScheduledWork() && !IsWindow(taskViewHookFixtureWindow),
        "cancelled unarmed request releases its independent monitor without completion callback");

    request = StatusBarTaskViewInputHandoff::Create(scheduler, bounds, remaining, api);
    Check(request != nullptr, "shield can be installed again after cancellation");
    request->Begin(1500);
    down.pt = {48, 16};
    Check(SendMessageW(taskViewHookFixtureWindow, WM_APP + 1, WM_LBUTTONDOWN,
        reinterpret_cast<LPARAM>(&down)) == 1, "production hook callback consumes the adjacent search press");
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

    request = StatusBarTaskViewInputHandoff::Create(scheduler, bounds, remaining, api);
    Check(request != nullptr, "device-switch sequence installs the production handoff");
    request->Begin(1500);
    down.pt = {48, 16};
    Check(SendMessageW(taskViewHookFixtureWindow, WM_APP + 1, WM_LBUTTONDOWN,
        reinterpret_cast<LPARAM>(&down)) == 1, "production hook consumes the old press before a missing release");
    request->Cancel(); request.reset();
    WaitAndDispatch(scheduler);
    Check(scheduler.HasScheduledWork() && IsWindow(taskViewHookFixtureWindow),
        "missing release keeps the cancelled monitor alive before the device returns");
    down.pt = {200, 200};
    Check(SendMessageW(taskViewHookFixtureWindow, WM_APP + 1, WM_LBUTTONDOWN,
        reinterpret_cast<LPARAM>(&down)) == 0, "production hook delivers a fresh press on another window");
    Check(SendMessageW(taskViewHookFixtureWindow, WM_APP + 1, WM_LBUTTONUP,
        reinterpret_cast<LPARAM>(&down)) == 0, "production hook delivers the fresh press's own release");
    WaitAndDispatch(scheduler);
    Check(!scheduler.HasScheduledWork() && !IsWindow(taskViewHookFixtureWindow),
        "fresh complete click retires the stale monitor without waiting for the lost old release");

    // Preserve the real scheduler, chord construction, transition state and
    // hook callback together. Only OS installation/input/clock are replaced.
    TaskViewTransitionGuard transition;
    double now = 100;
    int sends = 0;
    request = StatusBarTaskViewInputHandoff::Create(scheduler, bounds, [&] { return transition.Remaining(now); }, api);
    Check(request != nullptr, "opening interleave installs the production handoff");
    ScheduleStatusBarShellShortcut(scheduler, ResolveStatusBarShellChord(StatusBarAction::TaskView, true, false), {
        [](int) { return false; },
        [&](UINT count, INPUT*, int) {
            Check(transition.Begin(now, false), "input boundary starts one transition");
            request->Begin(transition.Remaining(now)); ++sends; return count;
        },
        [](auto) { return true; },
        [&](auto, StatusBarShortcutResult result) {
            Check(result == StatusBarShortcutResult::Sent, "opening chord completes without a delayed retry");
            request->ReleaseIfUnused();
        }, [&] { return now; }});
    const DWORD openingDeadline = GetTickCount() + 1000;
    while (!sends && GetTickCount() - openingDeadline >= 0x80000000u) WaitAndDispatch(scheduler);
    down.pt = {48, 16}; now = 376;
    Check(sends == 1 && SendMessageW(taskViewHookFixtureWindow, WM_APP + 1, WM_LBUTTONDOWN,
        reinterpret_cast<LPARAM>(&down)) == 1 && !transition.AllowsAction(StatusBarAction::QuickSearch, now, false),
        "real opening handoff blocks the reproduced 276ms adjacent click at both raw input and action boundaries");
    transition.Observe(true, 400); now = 749;
    WaitAndDispatch(scheduler);
    Check(IsWindow(taskViewHookFixtureWindow) && !transition.AllowsAction(StatusBarAction::QuickSearch, now, true),
        "early shown callback cannot release either boundary before settling");
    now = 750; request.reset(); WaitAndDispatch(scheduler);
    Check(IsWindow(taskViewHookFixtureWindow) && scheduler.HasScheduledWork() &&
        transition.AllowsAction(StatusBarAction::QuickSearch, now, true),
        "settled action routing resumes while the old physical release is still draining");
    down.pt = {300, 300};
    Check(SendMessageW(taskViewHookFixtureWindow, WM_APP + 1, WM_LBUTTONUP,
        reinterpret_cast<LPARAM>(&down)) == 1, "settled view cannot receive an orphaned release from the blocked click");
    WaitAndDispatch(scheduler);
    Check(sends == 1 && !scheduler.HasScheduledWork() && !IsWindow(taskViewHookFixtureWindow),
        "settling removes the hook without replaying search or a second Win+Tab");

    request = StatusBarTaskViewInputHandoff::Create(scheduler, bounds, remaining, api);
    Check(request != nullptr, "shield can restart for a later independent request");
    request->Begin(1500); request.reset(); scheduler.CancelAll();
    Check(!IsWindow(taskViewHookFixtureWindow), "scheduler shutdown joins and releases the shield");
    taskViewHookFixtureFails = true;
    Check(!StatusBarTaskViewInputHandoff::Create(scheduler, bounds, remaining, api) &&
        !scheduler.HasScheduledWork(), "hook installation failure rejects opening instead of applying an ineffective delay");
    taskViewHookFixtureFails = false;
    Check(UnregisterClassW(kTaskViewHookFixtureClass, windowClass.hInstance) != FALSE, "fixture unregisters after hook shutdown");
}

struct ShellShortcutRelayFixture
{
    std::atomic<HWND> foreground{reinterpret_cast<HWND>(1)};
    std::atomic<bool> allowActivation{true}, allowFocus{true}, holdAfterActivation{false};
    std::atomic<bool> updateForegroundOnActivation{true};
    std::atomic<bool> throwDuringHandoff{false};
    std::atomic<int> held{0};
    std::atomic<unsigned> activations{0}, keyChecks{0}, sendCalls{0};
    std::atomic<DWORD> sendThread{0};
    std::atomic<ULONGLONG> now{0};
    UINT firstAccepted = (std::numeric_limits<UINT>::max)();
    // Written only by the worker; inspected after Running()==false publishes
    // completion. No test invokes real foreground activation or SendInput.
    std::vector<std::vector<INPUT>> inputs;
};

snowdesktop::StatusBarShellShortcutRelay::Api MakeShellShortcutRelayApi(
    const std::shared_ptr<ShellShortcutRelayFixture>& fixture)
{
    snowdesktop::StatusBarShellShortcutRelay::Api api;
    api.foreground = [fixture] {
        if (fixture->throwDuringHandoff.load() && fixture->sendCalls.load() > 0)
            throw std::runtime_error("injected handoff observation failure");
        return fixture->foreground.load();
    };
    api.activate = [fixture](HWND window) {
        ++fixture->activations;
        if (!fixture->allowActivation.load()) return false;
        if (fixture->updateForegroundOnActivation.load()) fixture->foreground.store(window);
        if (fixture->holdAfterActivation.load()) fixture->held.store(VK_CONTROL);
        return true;
    };
    api.focused = [fixture](HWND window, DWORD) {
        return fixture->allowFocus.load() && fixture->foreground.load() == window;
    };
    api.keyDown = [fixture](int key) { ++fixture->keyChecks; return fixture->held.load() == key; };
    api.send = [fixture](UINT count, INPUT* inputs, int size) {
        Check(size == sizeof(INPUT), "relay uses the native INPUT ABI");
        fixture->sendThread.store(GetCurrentThreadId());
        fixture->inputs.emplace_back(inputs, inputs + count);
        return ++fixture->sendCalls == 1 &&
            fixture->firstAccepted != (std::numeric_limits<UINT>::max)() ? fixture->firstAccepted : count;
    };
    api.nowMilliseconds = [fixture] { return fixture->now.load(); };
    return api;
}

// Keep these business assertions on the original relay API so they can also
// run against the previous header in an isolated negative-control build.
void TestStatusBarShellShortcutNullForeground()
{
    using Relay = snowdesktop::StatusBarShellShortcutRelay;
    using Result = Relay::Result;
    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        fixture->foreground.store(nullptr);
        auto relay = Relay::Start(nullptr, {'N'}, MakeShellShortcutRelayApi(fixture));
        Check(relay != nullptr, "a system-panel click with no foreground window starts an isolated relay");
        Check(PumpMessagesUntil([&] { return relay->Poll() == Result::Sent; }),
            "a null initial foreground can hand off to the relay and accept the system-panel chord");
        Check(fixture->activations.load() == 1 && fixture->sendCalls.load() == 1,
            "a null initial foreground does not add activation or injection retries");
        fixture->foreground.store(reinterpret_cast<HWND>(2));
        Check(PumpMessagesUntil([&] { return !relay->Running(); }),
            "the null-origin request retires normally after foreground handoff");
    }
    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        fixture->foreground.store(nullptr);
        fixture->held.store(VK_CONTROL);
        auto relay = Relay::Start(nullptr, {'N'}, MakeShellShortcutRelayApi(fixture));
        Check(relay && PumpMessagesUntil([&] { return fixture->keyChecks.load() > 0; }),
            "a null-origin request waits for physical modifier release");
        fixture->foreground.store(reinterpret_cast<HWND>(2));
        Check(PumpMessagesUntil([&] { return !relay->Running(); }),
            "an external foreground cancels a null-origin request even while the modifier stays held");
        Check(relay->Poll() == Result::Cancelled && fixture->activations.load() == 0 &&
            fixture->sendCalls.load() == 0,
            "null is an exact foreground baseline, never permission to activate over another window");
    }
}

void TestStatusBarShellShortcutRelay()
{
    using Relay = snowdesktop::StatusBarShellShortcutRelay;
    using Result = Relay::Result;
    using snowdesktop::StatusBarShellChord;
    const HWND initial = reinterpret_cast<HWND>(1);
    const StatusBarShellChord taskView{VK_TAB, false, true};
    const auto waitForExit = [](const auto& relay) {
        Check(PumpMessagesUntil([&] { return !relay->Running(); }),
            "relay worker exits without a UI-thread join");
    };

    for (const StatusBarShellChord chord : {
        taskView, StatusBarShellChord{'N'}, StatusBarShellChord{'A'},
        StatusBarShellChord{'D', false, true}, StatusBarShellChord{'D', true}})
    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        auto relay = Relay::Start(initial, chord, MakeShellShortcutRelayApi(fixture));
        Check(relay && PumpMessagesUntil([&] { return relay->Poll() == Result::Sent; }),
            "isolated relay accepts each supported Shell chord");
        const HWND window = relay->Window();
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        Check(window && process == GetCurrentProcessId() && thread != GetCurrentThreadId() &&
            thread == relay->ThreadId() && thread == fixture->sendThread.load(),
            "actual relay HWND and injection boundary run on a separate GUI thread");
        Check(!GetParent(window) && !GetWindow(window, GW_OWNER) &&
            !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD),
            "relay window has no desktop parent, owner, or child style");
        Check(!Relay::Start(initial, chord, MakeShellShortcutRelayApi(fixture)),
            "accepted input keeps the global in-flight slot until its queue retires");
        relay->Cancel();
        Check(relay->Running() && IsWindow(window) && relay->Poll() == Result::Sent,
            "cancellation after acceptance cannot destroy the foreground input queue");
        fixture->foreground.store(initial); // Model Shell taking foreground.
        waitForExit(relay);
        Check(!IsWindow(window) && fixture->activations.load() == 1 && fixture->sendCalls.load() == 1,
            "foreground handoff retires the window without focus or chord retries");
        const auto& input = fixture->inputs.front();
        const unsigned presses = chord.alt ? 3u : 2u;
        Check(input.size() == presses * 2 && relay->AcceptedCount() == input.size() &&
            input[0].ki.wVk == VK_LWIN && input[presses - 1].ki.wVk == chord.key &&
            (!chord.alt || input[1].ki.wVk == VK_MENU),
            "relay preserves Windows, optional Alt, and destination-key order");
        for (unsigned i = 0; i < presses; ++i)
            Check(input[i].type == INPUT_KEYBOARD && !(input[i].ki.dwFlags & KEYEVENTF_KEYUP) &&
                input[input.size() - 1 - i].ki.wVk == input[i].ki.wVk &&
                input[input.size() - 1 - i].ki.dwFlags == (input[i].ki.dwFlags | KEYEVENTF_KEYUP),
                "relay pairs each synthetic press with its reverse-order release");
    }

    for (bool refuseActivation : {true, false})
    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        fixture->allowActivation.store(!refuseActivation);
        fixture->allowFocus.store(refuseActivation);
        auto relay = Relay::Start(initial, taskView, MakeShellShortcutRelayApi(fixture));
        Check(relay != nullptr, "foreground refusal fixture starts asynchronously");
        waitForExit(relay);
        Check(relay->Poll() == Result::Failed && fixture->sendCalls.load() == 0 &&
            fixture->activations.load() == 1,
            "either foreground or focus refusal prevents injection without a fallback");
    }

    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        fixture->held.store(VK_LBUTTON);
        auto relay = Relay::Start(initial, {'N'}, MakeShellShortcutRelayApi(fixture));
        Check(relay && PumpMessagesUntil([&] { return fixture->keyChecks.load() > 0; }),
            "physical pointer hold is observed on the worker");
        relay->Cancel();
        waitForExit(relay);
        Check(relay->Poll() == Result::Cancelled && fixture->sendCalls.load() == 0 &&
            fixture->activations.load() == 0,
            "even a notification chord waits for pointer release and can cancel without activation");
    }

    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        fixture->holdAfterActivation.store(true);
        auto relay = Relay::Start(initial, taskView, MakeShellShortcutRelayApi(fixture));
        Check(relay && PumpMessagesUntil([&] { return fixture->held.load() == VK_CONTROL; }),
            "modifier can become held during foreground activation");
        Check(relay->Poll() == Result::Pending && fixture->sendCalls.load() == 0,
            "relay resamples modifiers after activation instead of sending a stale chord");
        fixture->foreground.store(reinterpret_cast<HWND>(2));
        fixture->held.store(0);
        waitForExit(relay);
        Check(relay->Poll() == Result::Cancelled && fixture->sendCalls.load() == 0 &&
            fixture->activations.load() == 1,
            "external foreground change cancels the waiting request without reclaiming focus");
    }

    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        fixture->held.store(VK_SHIFT);
        auto relay = Relay::Start(initial, taskView, MakeShellShortcutRelayApi(fixture));
        Check(relay && PumpMessagesUntil([&] { return fixture->keyChecks.load() > 0; }),
            "held-modifier timeout fixture reaches the physical-state check");
        fixture->now.store(6000);
        waitForExit(relay);
        Check(relay->Poll() == Result::TimedOut && fixture->sendCalls.load() == 0,
            "preparation has a finite deadline without releasing the user's modifier");
    }

    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        fixture->firstAccepted = 2; // Win and Alt accepted, D never pressed.
        auto relay = Relay::Start(initial, {'D', true}, MakeShellShortcutRelayApi(fixture));
        Check(relay && PumpMessagesUntil([&] { return relay->Poll() == Result::Failed; }),
            "partial input acceptance is reported as failure");
        Check(relay->Running(), "partial acceptance also retains the input queue during handoff");
        fixture->foreground.store(initial);
        waitForExit(relay);
        Check(fixture->sendCalls.load() == 2 && fixture->inputs[1].size() == 2 &&
            fixture->inputs[1][0].ki.wVk == VK_MENU && fixture->inputs[1][1].ki.wVk == VK_LWIN &&
            (fixture->inputs[1][0].ki.dwFlags & KEYEVENTF_KEYUP) &&
            (fixture->inputs[1][1].ki.dwFlags & KEYEVENTF_KEYUP),
            "partial acceptance releases only unmatched synthetic presses and never retries the chord");
    }

    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        auto relay = Relay::Start(initial, taskView, MakeShellShortcutRelayApi(fixture));
        Check(relay && PumpMessagesUntil([&] { return relay->Poll() == Result::Sent; }),
            "request-release fixture reaches accepted input");
        const HWND window = relay->Window();
        const HANDLE worker = OpenThread(SYNCHRONIZE, FALSE, relay->ThreadId());
        Check(worker != nullptr, "fixture observes only its own worker thread lifetime");
        relay.reset();
        const ULONGLONG until = GetTickCount64() + 40;
        PumpMessagesUntil([&] { return GetTickCount64() >= until; });
        Check(IsWindow(window) && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT,
            "releasing the final UI request preserves the worker's foreground queue");
        fixture->foreground.store(initial);
        Check(PumpMessagesUntil([&] { return WaitForSingleObject(worker, 0) == WAIT_OBJECT_0; }) &&
            !IsWindow(window), "worker retains its own state and retires after Shell takes over");
        CloseHandle(worker);
    }

    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        auto relay = Relay::Start(initial, taskView, MakeShellShortcutRelayApi(fixture));
        Check(relay && PumpMessagesUntil([&] { return relay->Poll() == Result::Sent; }),
            "handoff deadline fixture sends once");
        fixture->now.store(1600);
        waitForExit(relay);
        Check(relay->Poll() == Result::Sent && fixture->sendCalls.load() == 1,
            "missing Shell handoff expires only the temporary queue and never replays the chord");
    }
}

void TestStatusBarShellShortcutRelayDiagnostics()
{
    using Relay = snowdesktop::StatusBarShellShortcutRelay;
    using Result = Relay::Result;
    using Reason = Relay::Reason;
    enum class Scenario { Accepted, ActivationRejected, ForegroundUnchanged, ExternalForeground, FocusRejected };
    for (const auto scenario : {Scenario::Accepted, Scenario::ActivationRejected,
        Scenario::ForegroundUnchanged, Scenario::ExternalForeground, Scenario::FocusRejected})
    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        fixture->foreground.store(nullptr);
        fixture->allowActivation.store(scenario != Scenario::ActivationRejected);
        fixture->updateForegroundOnActivation.store(scenario != Scenario::ForegroundUnchanged);
        fixture->allowFocus.store(scenario != Scenario::FocusRejected);
        auto api = MakeShellShortcutRelayApi(fixture);
        if (scenario == Scenario::ExternalForeground)
            api.activate = [fixture](HWND) {
                ++fixture->activations;
                fixture->foreground.store(reinterpret_cast<HWND>(2));
                return true;
            };
        auto relay = Relay::Start(nullptr, {'N'}, std::move(api));
        Check(relay && PumpMessagesUntil([&] { return relay->Poll() != Result::Pending; }),
            "each activation boundary publishes a terminal diagnostic");
        const auto beforeCleanup = relay->Diagnostics();
        if (scenario == Scenario::Accepted)
        {
            Check(beforeCleanup.result == Result::Sent && beforeCleanup.reason == Reason::InputAccepted &&
                beforeCleanup.activationReturned && beforeCleanup.focusChecked && beforeCleanup.focused,
                "accepted input records successful activation and focus validation without claiming Shell visibility");
            fixture->foreground.store(reinterpret_cast<HWND>(2));
        }
        else if (scenario == Scenario::ActivationRejected)
            Check(beforeCleanup.result == Result::Failed && beforeCleanup.reason == Reason::ActivationRejected &&
                !beforeCleanup.activationReturned && !beforeCleanup.focusChecked && !beforeCleanup.observedForeground,
                "activation rejection is distinguishable from a focus check that never ran");
        else if (scenario == Scenario::ForegroundUnchanged)
            Check(beforeCleanup.result == Result::Failed && beforeCleanup.reason == Reason::ForegroundNotAcquired &&
                beforeCleanup.activationReturned && !beforeCleanup.focusChecked && !beforeCleanup.observedForeground,
                "successful activation return without observed foreground ownership has its own reason");
        else if (scenario == Scenario::ExternalForeground)
            Check(beforeCleanup.result == Result::Cancelled && beforeCleanup.reason == Reason::ForegroundChanged &&
                beforeCleanup.activationReturned && !beforeCleanup.focusChecked &&
                beforeCleanup.observedForeground == reinterpret_cast<HWND>(2),
                "an external foreground observed during activation cancels without focus probing or retrying");
        else
            Check(beforeCleanup.result == Result::Failed && beforeCleanup.reason == Reason::FocusNotAcquired &&
                beforeCleanup.activationReturned && beforeCleanup.focusChecked && !beforeCleanup.focused &&
                beforeCleanup.observedForeground == beforeCleanup.relayWindow,
                "focus validation failure preserves the acquired foreground and actual failed check");
        Check(!beforeCleanup.initialForeground && beforeCleanup.relayWindow &&
            beforeCleanup.activationAttempted && fixture->activations.load() == 1 &&
            fixture->sendCalls.load() == (scenario == Scenario::Accepted ? 1u : 0u),
            "diagnostics retain the exact null baseline and single activation attempt");
        Check(PumpMessagesUntil([&] { return !relay->Running(); }),
            "diagnostic fixture releases its temporary GUI queue");
        const auto afterCleanup = relay->Diagnostics();
        Check(!relay->Window() && afterCleanup.result == beforeCleanup.result &&
            afterCleanup.reason == beforeCleanup.reason && afterCleanup.relayWindow == beforeCleanup.relayWindow &&
            afterCleanup.observedForeground == beforeCleanup.observedForeground &&
            afterCleanup.activationAttempted == beforeCleanup.activationAttempted &&
            afterCleanup.activationReturned == beforeCleanup.activationReturned &&
            afterCleanup.focusChecked == beforeCleanup.focusChecked && afterCleanup.focused == beforeCleanup.focused,
            "cleanup cannot overwrite the published terminal diagnostic or erase its relay window snapshot");
        Check(Relay::ReasonName(afterCleanup.reason)[0] != L'\0', "terminal reasons have stable diagnostic names");
    }
    for (bool partial : {false, true})
    {
        auto fixture = std::make_shared<ShellShortcutRelayFixture>();
        fixture->throwDuringHandoff.store(true);
        if (partial) fixture->firstAccepted = 1;
        auto relay = Relay::Start(fixture->foreground.load(), {'N'}, MakeShellShortcutRelayApi(fixture));
        Check(relay && PumpMessagesUntil([&] { return !relay->Running(); }),
            "a post-publication handoff exception still retires the relay window");
        const auto snapshot = relay->Diagnostics();
        Check(snapshot.result == (partial ? Result::Failed : Result::Sent) &&
            snapshot.reason == (partial ? Reason::InputPartial : Reason::InputAccepted) &&
            snapshot.observedForeground == snapshot.relayWindow && snapshot.relayWindow &&
            !relay->Window() && snapshot.activationReturned && snapshot.focused,
            "a later handoff exception preserves the first published result and diagnostic");
    }
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
    Check(ResolveStatusBarClick(Action::InputMethodPanel, false, settings, true) == Action::InputMethodPanel &&
        ResolveStatusBarClick(Action::InputMethodPanel, true, settings, true) == Action::SystemInputMethodPanel &&
        ResolveStatusBarClick(Action::InputMethod, true, settings, true) == Action::InputMethod,
        "the default layout picker and Ctrl override are separate from input-mode toggling");
    settings.inputMethodSystemPanel = true;
    Check(ResolveStatusBarClick(Action::InputMethodPanel, false, settings, false) == Action::SystemInputMethodPanel &&
        ResolveStatusBarClick(Action::SystemInputMethodPanel, false, settings, true) == Action::SystemInputMethodPanel,
        "the saved system layout picker works independently of Quick Settings support and preserves resolved intent");
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
    const auto queueChord = [&](StatusBarShellChord chord, UINT timeout = 5000) {
        return ScheduleStatusBarShellShortcut(scheduler, chord, {
            [&](int code) { return code == held; },
            [&](UINT count, INPUT* input, int size) {
                Check(size == sizeof(INPUT), "shortcut uses the native INPUT size");
                batches.emplace_back(input, input + count);
                return count >= 4 ? sentCount : count;
            },
            [&](auto) { return current; },
            [&](auto, Result result) { outcomes.push_back(result); }}, timeout);
    };
    const auto queue = [&](WORD key, UINT timeout = 5000, bool alt = false) {
        return queueChord(StatusBarShellChord{key, alt}, timeout);
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
    for (const bool windows11 : {false, true})
    {
        const auto desktop = ResolveStatusBarShellChord(Action::ShowDesktop, windows11, !windows11);
        Check(desktop.key == 'D' && !desktop.alt && desktop.waitForPointerRelease,
            "show desktop uses Win+D on both Windows versions rather than the Win+Alt+D calendar");
        queueChord(desktop);
        held = VK_LBUTTON; WaitAndDispatch(scheduler);
        Check(batches.empty() && outcomes.empty(), "show desktop waits until the initiating mouse gesture ends");
        held = VK_CONTROL; WaitAndDispatch(scheduler);
        Check(batches.empty() && outcomes.empty(), "show desktop does not inject through a held modifier");
        held = 0; WaitAndDispatch(scheduler); expectChord('D');
        batches.clear(); outcomes.clear();
    }
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
    TestStatusBarShellShortcutNullForeground();
    TestStatusBarShellShortcutRelay();
    TestStatusBarShellShortcutRelayDiagnostics();
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

    {
        UiAnimationScheduler batch;
        Check(batch.Initialize(), "timed presentation scheduler initializes");
        int owner = 0, otherOwner = 0;
        int firstState = 0, secondState = 0, paints = 0, otherPaints = 0;
        bool completeState = false;
        const auto request = [&] {
            return batch.RequestScheduledPresentation(&owner, [&] {
                ++paints;
                completeState = firstState == 1 && secondState == 1;
                batch.DispatchDue();
                return true;
            });
        };
        batch.ScheduleOnce(0, [&](auto) {
            firstState = 1;
            Check(request() && paints == 0, "first timed update queues without presenting a partial batch");
        });
        batch.ScheduleOnce(0, [&](auto) {
            secondState = 1;
            Check(request() && paints == 0, "second timed update shares the current snapshot");
            batch.RequestScheduledPresentation(&otherOwner, [&] { ++otherPaints; return true; });
        });
        batch.DispatchDue();
        Check(paints == 1 && otherPaints == 1 && completeState && !batch.HasScheduledWork(),
            "deadline-only updates present both complete states once without an extra wakeup");

        Check(!batch.RequestScheduledPresentation(&owner, [&] { ++paints; return false; }) && paints == 2,
            "direct input presentation remains synchronous and preserves failure results");
        int immediatePointerPaints = 0;
        batch.ScheduleOnce(0, [&](auto) {
            batch.RequestFramePresentation(&owner, [&] { ++immediatePointerPaints; });
            Check(immediatePointerPaints == 1,
                "existing immediate presentation is not deferred by a deadline callback");
        });
        batch.DispatchDue();

        int lastState = 0, publishedState = 0, mixedPaints = 0;
        batch.ScheduleOnce(0, [&](auto) {
            lastState = 1;
            batch.RequestScheduledPresentation(&owner, [&] { ++mixedPaints; publishedState = lastState; return true; });
        });
        batch.StartAnimation(UiAnimationSurface::Desktop, [&](double) {
            lastState = 2;
            batch.RequestFramePresentation(&owner, [&] { ++mixedPaints; publishedState = lastState; });
            return false;
        });
        batch.DispatchDue();
        Check(mixedPaints == 1 && publishedState == 2 && !batch.HasScheduledWork(),
            "timer and animation owners share their latest state in one presentation");

        int cancelledPresentations = 0;
        batch.ScheduleOnce(0, [&](auto) {
            batch.RequestScheduledPresentation(&owner, [&] { ++cancelledPresentations; return true; });
            batch.CancelAll();
        });
        batch.DispatchDue();
        Check(cancelledPresentations == 0 && !batch.HasScheduledWork(),
            "cancelling timed work discards its pending presentation");
        batch.ScheduleOnce(0, [&](auto) {
            batch.RequestScheduledPresentation(&owner, [&] { batch.CancelAll(); return true; });
            batch.RequestScheduledPresentation(&otherOwner, [&] { ++cancelledPresentations; return true; });
        });
        batch.DispatchDue();
        Check(cancelledPresentations == 0, "cancellation during timed presentation prevents later owners publishing");
        batch.ScheduleOnce(0, [&](auto) {
            batch.RequestScheduledPresentation(&owner, [&] { ++cancelledPresentations; return true; });
            throw std::runtime_error("retired component callback");
        });
        batch.DispatchDue();
        Check(cancelledPresentations == 1 && !batch.HasScheduledWork(),
            "failed deadline callbacks still publish their queued terminal state");

        // Use a private real HWND and nested dispatch, never the desktop host.
        // Restoring the old no-input-guard path delays this feedback and fails.
        ScheduledNativeInput input{&batch};
        WNDCLASSW nativeClass{};
        nativeClass.hInstance = GetModuleHandleW(nullptr);
        nativeClass.lpfnWndProc = ScheduledNativeInputProc;
        nativeClass.lpszClassName = L"SnowDesktopScheduledNativeInputTest";
        Check(RegisterClassW(&nativeClass) != 0,
            "native input fixture registers its private window class");
        const HWND inputWindow = CreateWindowExW(0, nativeClass.lpszClassName,
            L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nativeClass.hInstance, &input);
        Check(inputWindow != nullptr, "native input fixture creates a message-only window");
        int backgroundPaints = 0;
        batch.ScheduleOnce(0, [&](auto) {
            batch.RequestScheduledPresentation(&input.owner, [&] { ++input.paints; return true; });
            Check(input.paints == 0, "background preparation initially remains batched");
            SendMessageW(inputWindow, WM_APP + 91, 0, 0);
            Check(input.synchronous && input.paints == 1,
                "native input inside a scheduled modal pump presents synchronously");
            Check(!snowdesktop::UiImmediatePresentationScope::IsActive(),
                "returning from native dispatch restores the scheduled batch boundary");
            batch.RequestScheduledPresentation(&otherOwner, [&] { ++backgroundPaints; return true; });
            Check(backgroundPaints == 0, "background work continues batching after immediate input");
        });
        batch.DispatchDue();
        Check(input.paints == 1 && backgroundPaints == 1,
            "immediate input supersedes its queued owner without duplicate presentation");
        batch.StartAnimation(UiAnimationSurface::Desktop, [&](double) {
            Check(PostMessageW(inputWindow, WM_APP + 91, 0, 0) != FALSE,
                "frame fixture posts only to its own native input window");
            MSG message{};
            Check(PeekMessageW(&message, inputWindow, WM_APP + 91, WM_APP + 91, PM_REMOVE) != FALSE,
                "frame fixture receives its private input inside the active snapshot");
            DispatchMessageW(&message);
            Check(input.synchronous && input.paints == 2,
                "posted native input also remains synchronous within an animation snapshot");
            return false;
        });
        batch.DispatchDue();
        DestroyWindow(inputWindow);
        UnregisterClassW(nativeClass.lpszClassName, nativeClass.hInstance);
    }

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
        // A live pointer can still receive messages in a foreign modal loop,
        // yet magnification stays at its initial 1x without scheduler delivery.
        // This is the Dock's actual entry state, not a synthetic frame counter.
        const auto runHoverLoop = [](bool bridge) {
            UiAnimationScheduler modalScheduler;
            snowdesktop::dock_magnification::HoverEntryAnimation hover;
            const double began = UiAnimationScheduler::MonotonicMilliseconds();
            hover.SetHovered(true, began, 1.0);
            int flushes = 0;
            modalScheduler.StartAnimation(UiAnimationSurface::FloatingDock,
                [&](double now) { hover.Advance(now); return hover.IsAnimating(); });
            const auto nestedLoop = [&] {
                PumpMessagesUntil([&] {
                    // Repeated pointer samples must not restart the timeline.
                    hover.SetHovered(true, UiAnimationScheduler::MonotonicMilliseconds(), 1.0);
                    return UiAnimationScheduler::MonotonicMilliseconds() - began >= 220.0;
                });
            };
            if (bridge)
            {
                UiAnimationScheduler::MessagePumpScope pump(modalScheduler, [&] { ++flushes; });
                Check(pump.IsAvailable(), "modal hover bridge initializes");
                nestedLoop();
            }
            else nestedLoop();
            Check(bridge ? hover.FocusScale(1.28f) > 1.27f && flushes > 0
                         : hover.FocusScale(1.28f) == 1.0f && flushes == 0,
                "a modal hover needs timeline delivery even when pointer samples continue");
        };
        runHoverLoop(false);
        runHoverLoop(true);
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
