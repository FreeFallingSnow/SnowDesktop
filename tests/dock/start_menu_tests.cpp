#include "shell/start_menu_window.h"
#include "shell/start_menu_session.h"
#include "taskbar_hook/taskbar_hook_protocol.h"
#include <cstdint>
#include <iostream>
#include <thread>

int RunDockStartMenuTests()
{
    using namespace snowdesktop::start_menu;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    PointerDismissal pointer;
    check(!pointer.Observe(true, true, true), "the opening down sampled after Open cannot dismiss its own menu");
    check(!pointer.Observe(false, false, true), "the first physical release arms later owned-surface dismissal");
    check(!pointer.Observe(true, true, false), "a native menu click is left to native input handling");
    check(!pointer.Observe(false, false, false) && pointer.Observe(true, true, true),
        "a later status bar/fused bar/Dock down requests dismissal");
    check(!pointer.Observe(true, false, true), "holding a pointer button is not another dismissal request");
    pointer.Reset();
    check(!pointer.Observe(false, true, true), "an opening down/up between samples still belongs to the opening click");

    CloseRequest close;
    close.Request(1000);
    check(!close.Take(1000, false, false), "a hidden menu cannot be reopened by a close request");
    check(!close.Take(1100, true, false), "an activating click has a bounded native dismissal grace");
    close.Request(1100);
    check(close.Take(1150, true, false),
        "loss of Start foreground cannot abandon an owned Dock/bar dismissal after the grace");
    check(!close.Take(1200, true, true), "repeated clicks cannot send a second Win key while native close is pending");
    check(!close.Expired(2499) && close.Expired(2500), "a close that never completes has a bounded deadline");
    close.Reset(); close.Request(100);
    check(close.Take(100, true, true), "a nonactivating click can immediately close the foreground native menu");
    // Production worker/UI handoff, with only the Shell input and Dock window
    // callbacks replaced. Completion cannot permit another open until the old
    // promotion has been released, even if a click arrives before the UI timer.
    Invocation lifecycle;
    bool held = true;
    unsigned releases = 0, fallbacks = 0;
    const auto release = [&] {
        check(!lifecycle.Begin(), "reentrant UI cleanup cannot start another invocation");
        held = false;
        ++releases;
    };
    const auto fallback = [&] {
        check(!held, "native fallback only runs after the temporary Dock hold is released");
        check(!lifecycle.Begin(), "fallback input cannot start an overlapping protected invocation");
        ++fallbacks;
    };
    check(lifecycle.Begin(), "begin the first protected invocation");
    check(!lifecycle.Finish(release, fallback) && held, "UI cannot release a running worker's hold");
    std::thread failedWorker([&] { lifecycle.Complete(true); });
    failedWorker.join();
    check(lifecycle.Pending() && !lifecycle.Begin(),
        "worker failure remains reserved until UI cleanup, closing the rapid-click gap");
    check(lifecycle.Finish(release, fallback) && releases == 1 && fallbacks == 1 && !lifecycle.Pending(),
        "failure releases the hold and falls back exactly once");
    check(!lifecycle.Finish(release, fallback) && releases == 1 && fallbacks == 1,
        "a later UI timer cannot duplicate completed fallback input");
    held = true;
    check(lifecycle.Begin(), "a cleaned invocation allows the next click");
    lifecycle.Complete(true);
    lifecycle.Cancel(); // A second click after worker failure but before UI drain.
    check(lifecycle.Finish(release, fallback) && !held && fallbacks == 1,
        "a repeated click cancels pending fallback instead of sending a delayed extra Win press");
    held = true;
    check(lifecycle.Begin(), "cancellation does not poison a later invocation");
    lifecycle.Cancel(); // Cancellation can also precede worker completion.
    lifecycle.Complete(true);
    check(lifecycle.Finish(release, fallback) && !held && fallbacks == 1,
        "worker completion cannot resurrect input after cancellation");
    held = true;
    check(lifecycle.Begin(), "start an invocation cancelled reentrantly by Dock cleanup");
    lifecycle.Complete(true);
    check(lifecycle.Finish([&] { release(); lifecycle.Cancel(); }, fallback) && !held && fallbacks == 1,
        "cancellation during UI cleanup suppresses fallback before it dispatches input");
    held = true;
    check(lifecycle.Begin(), "start a successfully opened invocation");
    lifecycle.Complete(false);
    check(lifecycle.Finish(release, fallback) && !held && fallbacks == 1,
        "an already invoked menu is never toggled again by normal cleanup");
    check(lifecycle.Begin(), "start an invocation whose worker cannot be created");
    lifecycle.AbortBegin();
    check(!lifecycle.Pending() && !lifecycle.Finish(release, fallback),
        "thread creation failure leaves no deferred input or UI hold");

    // Production native visibility wait: the lease must outlive the visible
    // menu, including a transient hidden observation.
    std::uint64_t now = 0, leaseUntil = 0;
    unsigned renewals = 0;
    const auto wait = [&](int milliseconds) { now += milliseconds; return true; };
    const auto clock = [&] { return now; };
    const auto renew = [&] { leaseUntil = now + 3000; ++renewals; return true; };
    const auto dismissal = WaitForDismissal(false, [&] {
        check(now < leaseUntil, "native session does not let taskbar protection expire while Start is visible");
        return now >= 250 && (now < 3500 || now == 3550);
        // Delayed opening and one transient hidden sample are not dismissal.
    }, renew, wait, clock);
    check(dismissal == Dismissal::Closed && now == 3650 && renewals >= 7,
        "native session retains and renews the lease beyond its initial lifetime until actual dismissal");
    now = 0; renewals = 0;
    check(WaitForDismissal(false, [] { return false; }, renew, wait, clock) == Dismissal::NotFound && now == 2000,
        "a native invocation whose Start host never appears releases protection at a bounded deadline");
    now = 0; renewals = 0;
    check(WaitForDismissal(true, [] { return true; }, [&] { return ++renewals == 1; }, wait, clock) ==
        Dismissal::ProtectionLost && now == 500,
        "lost taskbar protection ends the native session without an unbounded renewal retry");
    now = 0;
    check(WaitForDismissal(true, [] { return true; }, renew, [](int) { return false; }, clock) ==
        Dismissal::Cancelled && now == 0,
        "cancellation exits the native hold without waiting for another native action");

    // Run the production close decision inside the production visibility loop.
    // Focus may already belong to the Dock, but protection ends on native
    // disappearance, not on the request. Only Win input/clock are substituted.
    now = 0; renewals = 0; close.Reset();
    unsigned closeInputs = 0;
    const auto closeResult = WaitForDismissal(false, [&] {
        if (now == 200) close.Request(now);
        const bool visible = now < 500;
        if (close.Take(now, visible, false)) ++closeInputs;
        check(now < leaseUntil, "the close request cannot release protection before native Start disappears");
        return visible;
    }, renew, wait, clock);
    check(closeResult == Dismissal::Closed && now == 550 && closeInputs == 1 && renewals == 2,
        "a Dock/bar close with different foreground sends once and retains protection until native dismissal");

    // Exercise the production traversal against a private hidden parent;
    // never enumerate or open the user's actual Start menu for this test.
    WNDCLASSW coreClass{};
    coreClass.lpfnWndProc = DefWindowProcW;
    coreClass.hInstance = GetModuleHandleW(nullptr);
    coreClass.lpszClassName = kCoreWindowClass;
    const ATOM coreAtom = RegisterClassW(&coreClass);
    check(coreAtom != 0, "register isolated CoreWindow discovery fixture");
    const HWND parent = CreateWindowW(L"STATIC", L"Start discovery fixture", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, coreClass.hInstance, nullptr);
    HWND first = nullptr, second = nullptr;
    if (coreAtom && parent)
    {
        first = CreateWindowW(kCoreWindowClass, L"cached", WS_CHILD,
            0, 0, 1, 1, parent, nullptr, coreClass.hInstance, nullptr);
        second = CreateWindowW(kCoreWindowClass, L"current", WS_CHILD,
            0, 0, 1, 1, parent, nullptr, coreClass.hInstance, nullptr);
    }
    check(parent && first && second, "create multiple hidden lookup fixtures");
    if (first && second)
    {
        const auto next = [parent](HWND after) {
            return FindWindowExW(parent, after, kCoreWindowClass, nullptr);
        };
        const HWND initial = next(nullptr);
        const HWND wanted = next(initial);
        WindowDiscovery discovery;
        check(wanted && FindVisibleStartWindow(discovery, next,
            [wanted](HWND window, WindowDiscovery&) { return window == wanted; }) == wanted &&
            discovery.candidates == 2 && !discovery.scanLimited,
            "class lookup continues past a rejected cached/unrelated CoreWindow");
        check(!IsStartWindow(first) && !IsStartWindow(second),
            "a matching class in another process is insufficient to be observed as Start");
        check(!FindVisibleStartWindow(discovery, next,
            [](HWND window, WindowDiscovery&) { return IsStartWindow(window); }) &&
            discovery.candidates == 2 && !discovery.scanLimited,
            "unrelated same-class windows cannot be selected as the Start host");
        unsigned inspections = 0;
        check(!FindVisibleStartWindow(discovery,
            [first, second](HWND after) { return after == first ? second : first; },
            [&inspections](HWND, WindowDiscovery&) { ++inspections; return false; }) &&
            discovery.scanLimited && inspections == 2,
            "reordered/recycled HWNDs terminate discovery without a worker hang");
        std::uintptr_t sequence = 0;
        check(!FindVisibleStartWindow(discovery,
            [&sequence](HWND) { return reinterpret_cast<HWND>(++sequence); },
            [](HWND, WindowDiscovery&) { return false; }) &&
            discovery.scanLimited && sequence == 128,
            "continuous window churn cannot extend the lookup deadline indefinitely");
    }
    if (parent) DestroyWindow(parent);
    if (coreAtom) UnregisterClassW(kCoreWindowClass, coreClass.hInstance);

    using namespace snowdesktop::taskbar_hook;
    Snapshot snapshot;
    snapshot.enabled = true;
    snapshot.targetCount = 1;
    snapshot.targets[0].taskbar = 42;
    snapshot.targets[0].protectAutoHideActivation = TRUE;
    snapshot.targets[0].shellPanelVisible = TRUE;
    snapshot.dockStartMenuUntil = 4000;
    check(ShouldSuppressTaskbar(snapshot, 42, 2000), "Dock Start lease retains an auto-hidden bar while Start is visible");
    check(!ShouldSuppressTaskbar(snapshot, 42, 4000), "expired Start protection cannot leave a taskbar hidden");
    check(!ShouldSuppressTaskbar(snapshot, 43, 2000), "a stale unknown taskbar does not inherit a lease");
    snapshot.targets[0].protectAutoHideActivation = FALSE;
    check(!ShouldSuppressTaskbar(snapshot, 42, 2000), "always-visible taskbars do not gain Start suppression");
    snapshot.suppressTaskbar = true;
    snapshot.targets[0].suppressTaskbar = TRUE;
    snapshot.targets[0].shellPanelVisible = FALSE;
    check(ShouldSuppressTaskbar(snapshot, 42, 5000), "lease expiration preserves the independent permanent-hide setting");

    return failures;
}
