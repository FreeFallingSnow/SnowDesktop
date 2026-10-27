#include "dock/dock_fullscreen_policy.h"
#include "dock/dock_fullscreen_storage.h"
#include "dock/floating_dock_rules.h"
#include "dock/dock_running_animation.h"
#include "dock/dock_minimize_completion.h"
#include "dock/dock_window_transition.h"
#include "platform/foreground_fullscreen.h"
#include "system/status_bar/status_bar_appbar.h"

#include <iostream>
#include <array>
#include <map>
#include <thread>

namespace
{
void CheckMinimizeCompletion(const auto& check)
{
    namespace completion = snowdesktop::dock_minimize_completion;
    const auto window = [](UINT_PTR id) { return reinterpret_cast<HWND>(id); };
    const auto monitor = [](UINT_PTR id) { return reinterpret_cast<HMONITOR>(id); };
    const HWND source = window(1), dockB = window(3);
    const HMONITOR monitorA = monitor(10), monitorB = monitor(11);
    const completion::Target target{dockB, monitorB, {-900, 1000, -850, 1050}};
    completion::Pending pending;
    completion::WindowState state{42};
    std::vector<completion::Target> completed;
    const auto poll = [&](ULONGLONG now) {
        pending.Poll(now, [&](HWND observed) { check(observed == source, "completion observes its original source"); return state; },
            [&](const auto& result) { completed.push_back(result); });
    };
    pending.Track(source, 42, target, 100);
    poll(101);
    check(completed.empty() && pending.Waiting(), "an accepted minimize request is not yet a successful minimize");
    state = {42, true, true, false}; poll(200);
    check(completed.empty(), "native minimization waits for the custom minimize image to retire");
    state.minimizing = false; poll(220);
    check(completed.size() == 1 && completed.front().dock == dockB && completed.front().monitor == monitorB,
        "completion retains the associated Dock monitor even when the source or current foreground is on another screen");
    poll(240);
    check(completed.size() == 1 && !pending.Waiting(), "successful minimization delivers one completion, without another polling timer");

    bool promotedA = true, promotedB = true, passiveB = true;
    HMONITOR fullscreenMonitor = monitorA;
    bool liveDock = true;
    const auto dismiss = [&] {
        return completion::DismissForFullscreen(target,
            [&](const auto& current) { return liveDock && current.dock == dockB && current.monitor == monitorB; },
            [&](HMONITOR observed) { return observed == fullscreenMonitor; },
            [&](const auto&) { promotedB = passiveB = false; });
    };
    check(!dismiss() && promotedA && promotedB && passiveB,
        "fullscreen on the source/other screen cannot cancel the associated Dock's summon");
    fullscreenMonitor = monitorB;
    check(dismiss() && promotedA && !promotedB && !passiveB,
        "fullscreen on the associated Dock screen clears manual and passive summons while preserving another screen");
    promotedB = true; poll(260);
    check(promotedB && completed.size() == 1, "a later deliberate summon survives the already-consumed minimize completion");
    liveDock = false;
    check(!dismiss() && promotedB, "a replaced or moved Dock cannot inherit an earlier minimize action");

    const auto reset = [&] { pending.Clear(); completed.clear(); state = {42}; pending.Track(source, 42, target, 100); };
    reset(); poll(3100);
    check(completed.empty() && !pending.Waiting(), "vetoed or failed native minimize expires without canceling a Dock summon");
    reset(); state = {}; poll(120);
    check(completed.empty() && !pending.Waiting(), "a destroyed source is not a successful minimize");
    reset(); state.process = 43; poll(120);
    check(completed.empty() && !pending.Waiting(), "reusing a native HWND in another process cannot complete an old minimize");
    reset(); state = {42, true, true, false}; poll(120); state = {42}; poll(140);
    check(completed.empty() && !pending.Waiting(), "restoring the source during its minimize image cancels the old action");
    reset(); state = {42, true, false, true}; poll(120);
    check(completed.empty() && !pending.Waiting(), "a reversal into restore cannot cancel a Dock summon as a completed minimize");
    reset(); state.minimized = true; poll(120);
    check(completed.size() == 1, "native minimization without a custom animation still completes");
    pending.Clear(); pending.Track(source, 42, {nullptr, monitorB, {}}, 100);
    check(!pending.Waiting(), "an action without an associated live Dock never starts an observation");
}

void CheckNativeMinimizeCompletion(HWND source, HINSTANCE instance, RECT screen, const auto& check)
{
    snowdesktop::UiAnimationScheduler scheduler;
    check(scheduler.Initialize(), "initialize isolated native-minimize observation scheduler");
    DockWindowTransition transition;
    check(transition.Initialize(instance, &scheduler, nullptr, nullptr), "initialize observer without custom composition support");
    HWND dock = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SnowFullscreenFixture", L"Dock target",
        WS_POPUP, screen.left, screen.bottom - 32, 32, 32, nullptr, nullptr, instance, nullptr);
    check(dock != nullptr, "create isolated associated Dock identity");
    const RECT anchor{screen.left, screen.bottom - 32, screen.left + 32, screen.bottom};
    const HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    unsigned completed = 0;
    transition.SetMinimizeCompletionCallbacks([&](HWND observed, std::optional<RECT> requested) {
        check(observed == source, "native completion provider receives the minimized window");
        return std::optional{DockWindowTransition::MinimizeTarget{dock, monitor, requested.value_or(anchor)}};
    }, [&](const auto& target) {
        ++completed;
        check(IsIconic(source) && !transition.IsActive(), "native completion requires the real minimized state after animation retirement");
        check(target.dock == dock && target.monitor == monitor && EqualRect(&target.anchor, &anchor),
            "native completion preserves the associated Dock and its actual screen anchor");
    });
    const auto pump = [&](const auto& predicate, ULONGLONG duration) {
        const ULONGLONG deadline = GetTickCount64() + duration;
        do
        {
            scheduler.DispatchDue();
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            if (predicate()) return true;
            WaitForSingleObject(scheduler.WaitHandle(), 10);
        } while (GetTickCount64() < deadline);
        return predicate();
    };
    ShowWindow(source, SW_SHOWNOACTIVATE);
    check(!transition.StartMinimize(source, anchor, dock), "missing composition falls back to the native action");
    ShowWindow(source, SW_MINIMIZE);
    check(pump([&] { return completed == 1; }, 500), "Dock-click native fallback delivers its successful minimize completion");
    transition.ObserveNativeMinimize(source);
    check(!transition.StartMinimize(source, anchor, dock), "an already minimized source cannot start another minimize animation");
    pump([] { return false; }, 40);
    check(completed == 1, "duplicate native events and already-minimized requests cannot repeat a Dock-click completion");
    transition.ForgetNativeMinimize(source);
    ShowWindow(source, SW_SHOWNOACTIVATE);
    transition.ObserveNativeMinimize(source);
    ShowWindow(source, SW_MINIMIZE);
    check(pump([&] { return completed == 2; }, 500), "application-triggered native minimization uses the same completion observer");
    transition.SetMinimizeCompletionCallbacks({}, {});
    DestroyWindow(dock);
}

// The production picker collector consumes Dock caches; replace only the OS
// window/path reader so fixed entries need no live desktop or installed game.
void CheckApplicationPicker(const auto& check)
{
    namespace policy = snowdesktop::dock_fullscreen;
    struct Running
    {
        snowdesktop::dock_running_animation::Presence presence;
        HWND window;
        std::wstring title, executablePath;
    };
    struct Pinned
    {
        HWND window;
        bool running;
        std::vector<HWND> trackedWindows;
    };
    const auto window = [](UINT_PTR id) { return reinterpret_cast<HWND>(id); };
    Running player{{}, window(1), L"Player", L"C:\\Apps\\Player.exe"};
    player.presence.SetVisible(true, 0, false);
    Running exiting{{}, window(7), L"Exiting", L"C:\\Apps\\Exiting.exe"};
    exiting.presence.SetVisible(true, 0, false);
    exiting.presence.SetVisible(false, 0, true);
    const std::vector<Running> running{player, exiting};
    const std::map<std::wstring, Pinned> pinned{
        {L"game", {window(2), true, {window(2), window(3)}}},
        {L"helper", {window(4), true, {window(4), window(5)}}},
        {L"not-running", {window(6), false, {window(6)}}},
        {L"steam-process-only", {nullptr, true, {}}}};
    const std::map<HWND, DockFullscreenApplication> live{
        {window(1), {L"Player", L"C:/Apps/PLAYER.exe"}},
        {window(2), {L"Game", L"D:\\Games\\game.exe"}},
        {window(3), {L"Second game window", L"d:/games/GAME.EXE"}},
        {window(4), {L"Player pinned too", L"C:\\Apps\\Player.exe"}},
        {window(5), {L"Game child", L"D:\\Games\\bin\\client.exe"}},
        {window(6), {L"Stopped", L"C:\\Apps\\Stopped.exe"}},
        {window(7), {L"Exiting", L"C:\\Apps\\Exiting.exe"}}};
    const auto readWindow = [&](HWND handle, const std::wstring&, const std::wstring&)
        -> std::optional<DockFullscreenApplication> {
        const auto found = live.find(handle);
        return found == live.end() ? std::nullopt : std::optional{found->second};
    };
    const auto applications = policy::CollectApplications(running, pinned, readWindow);
    const std::vector<DockFullscreenApplication> expected{
        {L"Player", L"C:\\Apps\\PLAYER.exe"},
        {L"Game", L"D:\\Games\\game.exe"},
        {L"Game child", L"D:\\Games\\bin\\client.exe"}};
    check(applications == expected,
        "picker includes running fixed apps and real child executables, deduplicates windows/paths and excludes stopped/exiting entries");
    const std::vector<Running> emptyRunning;
    const std::map<std::wstring, Pinned> onlyPinned{
        {L"game", {window(2), true, {window(2)}}}};
    check(policy::CollectApplications(emptyRunning, onlyPinned, readWindow) ==
        std::vector<DockFullscreenApplication>{{L"Game", L"D:\\Games\\game.exe"}},
        "pinning the only running app cannot empty the exception picker");
    auto stalePinned = onlyPinned;
    stalePinned.begin()->second.window = window(99);
    stalePinned.begin()->second.trackedWindows = {window(99)};
    check(policy::CollectApplications(emptyRunning, stalePinned, readWindow).empty(),
        "closed windows remaining in a Dock cache cannot become exceptions");
}
}

int RunFullscreenFocusTests()
{
    namespace policy = snowdesktop::dock_fullscreen;
    namespace fullscreen = snowdesktop::fullscreen;
    int failures = 0;
    const auto check = [&](bool value, const char* description) {
        if (!value) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
    };
    CheckApplicationPicker(check);
    CheckMinimizeCompletion(check);
    using P = DockFullscreenPolicy;
    using S = DockRevealSource;
    check(policy::LoadPolicy({}, {}) == P::FullProtection,
        "fresh preferences protect both exclusive and borderless fullscreen");
    check(policy::LoadPolicy({}, true) == P::BlockGestures && policy::LoadPolicy({}, false) == P::Allow,
        "legacy toggle migration retains the user's deliberate shortcut availability");
    check(policy::LoadPolicy(0, true) == P::Allow && policy::LoadPolicy(2, false) == P::FullProtection,
        "saved new policies take precedence over the downgrade compatibility field");
    check(policy::LoadPolicy(99, false) == P::FullProtection,
        "corrupt policy cannot silently disable fullscreen protection");
    check(policy::BlocksReveal(P::FullProtection, S::Hotkey, true, false) &&
        policy::BlocksReveal(P::FullProtection, S::AssociatedSurface, true, false) &&
        policy::BlocksReveal(P::FullProtection, S::Gesture, true, false),
        "full protection blocks every reveal source even with the cursor on another monitor");
    check(!policy::BlocksReveal(P::BlockGestures, S::Hotkey, true, true) &&
        policy::BlocksReveal(P::BlockGestures, S::Gesture, true, true) &&
        !policy::BlocksReveal(P::BlockGestures, S::Gesture, true, false),
        "gesture-only protection preserves explicit hotkeys and the other monitor's passive Dock");
    check(!policy::BlocksReveal(P::Allow, S::Gesture, true, true) &&
        !policy::BlocksReveal(P::FullProtection, S::Hotkey, false, true),
        "opt-outs and ordinary windows preserve invocation");
    check(!policy::StartsKeyboardSession(S::Gesture) && !policy::StartsKeyboardSession(S::Hotkey) &&
        !policy::StartsKeyboardSession(S::WindowAction),
        "ordinary foreground windows retain activation after either Dock summon trigger");
    check(policy::StartsKeyboardSession(S::Gesture, true) && policy::StartsKeyboardSession(S::Hotkey, true) &&
        policy::StartsKeyboardSession(S::WindowAction, true),
        "both summon triggers reuse the keyboard path for a higher-privilege foreground");
    check(!policy::StartsKeyboardSession(S::Gesture, true, true) &&
        !policy::StartsKeyboardSession(S::Hotkey, true, true) &&
        !policy::StartsKeyboardSession(S::AssociatedSurface, true, true),
        "foreground fullscreen suppresses every implicit keyboard activation path");
    check(policy::StartsKeyboardSession(S::AssociatedSurface),
        "explicit keyboard-associated surfaces keep their input path outside fullscreen");
    const RECT dismissMonitorBounds{-1920, 0, 0, 1080};
    for (const RECT dock : {RECT{-1500, 1000, -400, 1080}, RECT{-1500, 0, -400, 80},
            RECT{-1920, 200, -1840, 900}, RECT{-80, 200, 0, 900}})
    {
        const RECT button = snowdesktop::floating_dock_rules::ExplicitDismissBounds(dock, dismissMonitorBounds, 48);
        RECT intersection{};
        check(button.right - button.left == 48 && button.bottom - button.top == 48 &&
            button.left >= dismissMonitorBounds.left && button.top >= dismissMonitorBounds.top && button.right <= dismissMonitorBounds.right &&
            button.bottom <= dismissMonitorBounds.bottom && IntersectRect(&intersection, &dock, &button),
            "explicit dismiss overlaps the visible Dock corner and stays on each monitor edge at high DPI");
        check(snowdesktop::floating_dock_rules::IsPointOnExplicitDismiss(button,
                {(button.left + button.right) / 2, (button.top + button.bottom) / 2}) &&
            !snowdesktop::floating_dock_rules::IsPointOnExplicitDismiss(button, {button.left, button.top}),
            "the circular dismiss accepts its center but does not claim transparent square corners");
    }
    const RECT panel{-1500, 500, -400, 580};
    const RECT movedPanel{-1450, 540, -250, 620};
    const RECT initialButton = snowdesktop::floating_dock_rules::ExplicitDismissBounds(panel, dismissMonitorBounds, 48);
    const RECT movedButton = snowdesktop::floating_dock_rules::ExplicitDismissBounds(movedPanel, dismissMonitorBounds, 48);
    check(initialButton.left + 24 == panel.right && initialButton.top + 24 == panel.top &&
        movedButton.left - initialButton.left == movedPanel.right - panel.right &&
        movedButton.top - initialButton.top == movedPanel.top - panel.top,
        "dismiss follows the painted Dock corner as the panel moves and changes width");

    std::vector<DockFullscreenException> exceptions{
        {L"C:\\Apps\\Player.exe", P::Allow}, {L"C:/Apps/PLAYER.exe", P::BlockGestures},
        {L"player.exe", P::Allow}, {L"D:\\Games\\game.exe", P::FullProtection}};
    policy::NormalizeExceptions(exceptions);
    check(exceptions.size() == 2 && policy::SelectPolicy(P::FullProtection, exceptions,
        L"\\\\?\\C:\\Apps\\player.exe") == P::BlockGestures,
        "full executable paths normalize separators, extended prefixes and case; duplicates update instead of expanding scope");
    check(policy::SelectPolicy(P::FullProtection, exceptions, L"D:\\Other\\Player.exe") == P::FullProtection,
        "same filename in another directory cannot inherit a player exception");
    check(policy::NormalizeExecutable(std::wstring(L"C:\\Player.exe\0Other.exe", 23)).empty(),
        "embedded null cannot shorten the executable exception to a different file");
    JsonValue document;
    check(ParseJson(R"({"floatingEdgeSwipeBlockFullscreen":true})", document) &&
        policy::DecodePolicy(document) == P::BlockGestures,
        "the production JSON loader migrates the legacy true toggle to gestures only");
    check(ParseJson(R"({"fullscreenPolicy":"invalid","floatingEdgeSwipeBlockFullscreen":false})", document) &&
        policy::DecodePolicy(document) == P::FullProtection,
        "an invalid new field cannot silently inherit an old opt-out");
    check(ParseJson(R"({"fullscreenExceptions":[{"executable":"C:\\Apps\\播放器.exe","policy":0}]})", document) &&
        policy::DecodeExceptions(document, exceptions) && exceptions.size() == 1 && exceptions[0].executable == L"C:\\Apps\\播放器.exe",
        "persisted exceptions retain Unicode executable paths");
    const auto previous = exceptions;
    check(ParseJson(R"({"fullscreenExceptions":[{"executable":"player.exe","policy":0}]})", document) &&
        !policy::DecodeExceptions(document, exceptions) && exceptions == previous,
        "invalid exception cannot broaden matching or partially replace prior preferences");

    int activationCalls = 0;
    bool fullscreenActive = true;
    const auto observe = [&]() { return fullscreenActive; };
    const auto activate = [&]() { ++activationCalls; };
    check(!fullscreen::GuardFocusRequest(false, observe, activate),
        "automatic completion must never steal foreground focus, independent of shortcut settings");
    check(activationCalls == 0, "fullscreen gate prevents activation side effects, not just invocation labels");
    fullscreenActive = false;
    check(fullscreen::GuardFocusRequest(false, observe, activate), "ordinary desktop interaction can request input focus");
    fullscreenActive = true;
    check(!fullscreen::GuardFocusRequest(false, observe, activate) && activationCalls == 1,
        "a game entering fullscreen before an attached-input retry prevents the second activation");
    check(fullscreen::GuardFocusRequest(true, observe, activate) && activationCalls == 2,
        "an explicitly allowed hotkey can intentionally leave fullscreen");

    // Real geometry/style observation on a private desktop. Never switch the
    // user's input desktop, activate the production host or drive a game.
    const auto name = L"SnowFullscreenFocus_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
    check(desktop != nullptr, "create isolated fullscreen fixture desktop");
    if (!desktop) return failures;
    std::thread fixture([&]() {
        if (!SetThreadDesktop(desktop)) { check(false, "attach isolated fixture thread"); return; }
        if (!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        { check(false, "observe terminal inset in physical pixels on the isolated fixture thread"); return; }
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW cls{};
        // Custom browser chrome owns the nonclient calculation, even while
        // WS_THICKFRAME remains set on an ordinary maximized window.
        cls.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
            if (message == WM_NCCALCSIZE)
            {
                auto& client = wp ? reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0]
                                  : *reinterpret_cast<RECT*>(lp);
                client.bottom -= static_cast<LONG>(GetWindowLongPtrW(window, GWLP_USERDATA));
                return 0;
            }
            return DefWindowProcW(window, message, wp, lp);
        };
        cls.hInstance = instance;
        cls.lpszClassName = L"SnowFullscreenFixture";
        if (!RegisterClassW(&cls)) { check(false, "register fixture window"); return; }
        MONITORINFO monitor{sizeof(monitor)};
        const auto handle = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(handle, &monitor)) { check(false, "read fixture monitor"); UnregisterClassW(cls.lpszClassName, instance); return; }
        const RECT rect = monitor.rcMonitor;
        HWND window = CreateWindowExW(0, cls.lpszClassName, L"Fullscreen fixture", WS_POPUP,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
        check(window != nullptr, "create fullscreen-sized fixture");
        if (window)
        {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            check(fullscreen::ObserveWindow(window, 0).has_value(), "native borderless client is detected without a D3D-exclusive hint");
            check(!fullscreen::ObserveWindow(window).has_value(), "own-process desktop surfaces do not trigger external fullscreen protection");
            check(fullscreen::ObserveMonitor(handle, 0).has_value(), "monitor observation detects a visible borderless client without foreground ownership");
            check(!fullscreen::ObserveMonitor(handle), "monitor observation ignores our own presentation surfaces");
            const LONG centerX = rect.left + (rect.right - rect.left) / 2;
            const LONG centerY = rect.top + (rect.bottom - rect.top) / 2;
            HWND covering = CreateWindowExW(0, cls.lpszClassName, L"Temporary application", WS_POPUP,
                centerX - 120, centerY - 80, 240, 160, nullptr, nullptr, instance, nullptr);
            check(covering != nullptr, "create temporary application over the fullscreen client");
            if (covering)
            {
                SetWindowPos(covering, HWND_TOP, centerX - 120, centerY - 80, 240, 160, SWP_SHOWWINDOW | SWP_NOACTIVATE);
                check(!fullscreen::ObserveMonitor(handle, 0), "a foreground-layer ordinary application obscures background fullscreen on this monitor");
                ShowWindow(covering, SW_MINIMIZE);
                check(fullscreen::ObserveMonitor(handle, 0).has_value(), "minimizing the covering app reveals the same monitor's fullscreen client");
                DestroyWindow(covering);
            }
            int nativeActivationCalls = 0;
            // Original Terminal sample: complete outer monitor, client bottom
            // two physical pixels short for the auto-hidden taskbar. Use the
            // real native sampler and scan; only the process boundary is waived.
            SetWindowLongPtrW(window, GWLP_USERDATA, 2);
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
            const auto terminalSample = fullscreen::SampleWindow(window, 0);
            check(terminalSample && terminalSample->client.bottom == rect.bottom - 2 &&
                EqualRect(&terminalSample->frame, &rect), "native terminal fixture retains its original two-pixel client inset");
            const auto terminal = fullscreen::ObserveWindow(window, 0);
            const std::array terminalMonitors{handle};
            const auto terminalSources = fullscreen::ObserveMonitors(terminalMonitors, 0);
            check(terminal && terminalSources.front() && terminalSources.front()->window == window &&
                fullscreen::ObserveMonitor(handle, 0).has_value(),
                "direct, single-monitor and shared multi-monitor observation recognize the same inset fullscreen terminal");
            snowdesktop::StatusBarFullscreenState terminalBar;
            terminalBar.Observe(terminalSources.front() ? terminalSources.front()->window : nullptr, false, false, false);
            check(terminalBar.ShouldHide(false), "shared terminal observation drives the independent status bar hide state");
            check(policy::BlocksReveal(P::FullProtection, S::Gesture, terminal.has_value(), true) &&
                !fullscreen::GuardFocusRequest(false, [&]() { return fullscreen::ObserveWindow(window, 0); },
                    [&]() { ++nativeActivationCalls; }) && nativeActivationCalls == 0,
                "terminal inset observation blocks protected Dock reveal and automatic keyboard activation");
            SetWindowLongPtrW(window, GWLP_USERDATA, 3);
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
            check(!fullscreen::ObserveWindow(window, 0) && !fullscreen::ObserveMonitor(handle, 0),
                "a larger native client inset cannot inherit the terminal taskbar allowance");
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
            check(!fullscreen::GuardFocusRequest(false, [&]() { return fullscreen::ObserveWindow(window, 0); },
                [&]() { ++nativeActivationCalls; SetFocus(window); }) && nativeActivationCalls == 0,
                "native fullscreen observation blocks the production focus boundary before SetFocus");
            // Reproduce custom chrome with a retained resize frame. Only the
            // external-process boundary is substituted; geometry, maximization,
            // observation and reveal policy use the production implementation.
            SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) | WS_THICKFRAME);
            ShowWindow(window, SW_SHOWMAXIMIZED);
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            check(IsZoomed(window) != FALSE, "custom-chrome fixture is actually maximized");
            const auto maximized = fullscreen::ObserveWindow(window, 0);
            check(!maximized, "monitor-sized maximized clients without WS_CAPTION are not fullscreen");
            check(!fullscreen::IsCandidate(
                GetWindowLongPtrW(window, GWL_STYLE), 0, IsZoomed(window) != FALSE, false),
                "ordinary maximized custom chrome retains its status bar");
            for (const auto source : {S::Gesture, S::AssociatedSurface, S::Hotkey})
                check(!policy::BlocksReveal(P::FullProtection, source, maximized.has_value(), false),
                    "a maximized app on another monitor cannot suppress Dock reveal, promotion or preview interaction");
            int maximizedFocusCalls = 0;
            check(fullscreen::GuardFocusRequest(false, [&]() { return fullscreen::ObserveWindow(window, 0); },
                [&]() { ++maximizedFocusCalls; }) && maximizedFocusCalls == 1,
                "maximized custom-chrome apps do not block the Dock keyboard focus boundary");

            // Chromium removes WS_CAPTION/WS_THICKFRAME on entering page or
            // F11 fullscreen, retaining WS_MAXIMIZE from the saved style.
            const auto maximizedStyle = GetWindowLongPtrW(window, GWL_STYLE);
            SetWindowLongPtrW(window, GWL_STYLE, maximizedStyle & ~(WS_CAPTION | WS_THICKFRAME));
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
            check(IsZoomed(window) != FALSE, "browser fullscreen fixture retains its real maximized state");
            const auto browserFullscreen = fullscreen::ObserveWindow(window, 0);
            check(browserFullscreen.has_value(), "maximized browser entering frameless fullscreen is observed");
            check(policy::BlocksReveal(P::FullProtection, S::AssociatedSurface, browserFullscreen.has_value(), true),
                "browser fullscreen still protects Dock interaction and focus");
            check(!fullscreen::GuardFocusRequest(false, [&]() { return fullscreen::ObserveWindow(window, 0); },
                [&]() { ++maximizedFocusCalls; }) && maximizedFocusCalls == 1,
                "browser fullscreen blocks activation even with WS_MAXIMIZE retained");
            const auto browserSample = fullscreen::SampleWindow(window, 0);
            const bool hidesBar = browserSample && fullscreen::IsFullscreen(*browserSample, rect);
            snowdesktop::StatusBarFullscreenState barState;
            barState.Observe(hidesBar ? window : nullptr, false, false, false);
            check(barState.ShouldHide(false), "browser fullscreen hides its monitor's independent status bar");
            RECT otherMonitor = rect;
            OffsetRect(&otherMonitor, rect.right - rect.left, 0);
            check(browserSample && !fullscreen::IsFullscreen(*browserSample, otherMonitor),
                "browser fullscreen does not hide the status bar on another monitor");

            SetWindowLongPtrW(window, GWL_STYLE, maximizedStyle);
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
            check(!fullscreen::ObserveWindow(window, 0) && !fullscreen::IsCandidate(
                GetWindowLongPtrW(window, GWL_STYLE), 0, IsZoomed(window) != FALSE, false),
                "exiting browser fullscreen restores ordinary maximized classification");
            ShowWindow(window, SW_RESTORE);
            SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
            check(fullscreen::ObserveWindow(window, 0).has_value(),
                "leaving maximized state restores protection for an actual borderless fullscreen client");
            SetWindowLongPtrW(window, GWL_EXSTYLE, WS_EX_NOACTIVATE);
            check(!fullscreen::ObserveWindow(window, 0), "fullscreen-sized passive overlays are excluded");
            SetWindowLongPtrW(window, GWL_EXSTYLE, 0);
            SetPropW(window, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
            check(!fullscreen::ObserveWindow(window, 0), "Shell overlay exclusion is respected");
            RemovePropW(window, L"NonRudeHWND");
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top - 80, SWP_NOACTIVATE | SWP_NOZORDER);
            check(!fullscreen::ObserveWindow(window, 0), "windowed or taskbar-sized clients are not fullscreen");
            ShowWindow(window, SW_HIDE);
            check(!fullscreen::ObserveWindow(window, 0), "hidden background fullscreen clients cannot block invocation");
            CheckNativeMinimizeCompletion(window, instance, rect, check);
            DestroyWindow(window);
        }
        UnregisterClassW(cls.lpszClassName, instance);
    });
    fixture.join();
    CloseDesktop(desktop);
    return failures;
}
