#include "shell/start_menu_window.h"
#include "shell/start_menu_session.h"
#include "shell/start_menu_launcher.h"
#include "taskbar_hook/taskbar_hook_protocol.h"
#include "dock/dock_settings_rules.h"
#include <cstdint>
#include <iostream>
#include <thread>

namespace
{
// Replace only the external COM server. Production version negotiation and
// vtable calls remain real; closing must never call the Show slot.
struct LauncherFixture final : snowdesktop::start_menu::LauncherCurrent,
    snowdesktop::start_menu::LauncherLegacy, IServiceProvider
{
    ULONG references = 1;
    bool current = true, legacy = true, visible = true;
    HRESULT currentError = S_OK, visibilityError = S_OK, syncError = S_OK;
    unsigned currentQueries = 0, legacyQueries = 0, shows = 0, closes = 0, syncCloses = 0;
    int closeMethod = 0, showMethod = 0, showFlags = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(snowdesktop::start_menu::LauncherCurrent))
            *result = static_cast<snowdesktop::start_menu::LauncherCurrent*>(this);
        else if (iid == __uuidof(snowdesktop::start_menu::LauncherLegacy))
            *result = static_cast<snowdesktop::start_menu::LauncherLegacy*>(this);
        else if (iid == IID_IServiceProvider) *result = static_cast<IServiceProvider*>(this);
        if (!*result) return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID iid, void** result) override
    {
        *result = nullptr;
        if (service != snowdesktop::start_menu::kLauncherService) return E_NOINTERFACE;
        if (iid == __uuidof(snowdesktop::start_menu::LauncherCurrent))
        {
            ++currentQueries;
            if (FAILED(currentError)) return currentError;
            if (!current) return E_NOINTERFACE;
        }
        else if (iid == __uuidof(snowdesktop::start_menu::LauncherLegacy))
        { ++legacyQueries; if (!legacy) return E_NOINTERFACE; }
        else return E_NOINTERFACE;
        return QueryInterface(iid, result);
    }
    HRESULT STDMETHODCALLTYPE Show(int method, int flags) override
    { ++shows; showMethod = method; showFlags = flags; visible = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE Dismiss(int method) override
    { ++closes; closeMethod = method; visible = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE IsVisible(BOOL* value) override
    { *value = visible ? TRUE : FALSE; return visibilityError; }
    HRESULT STDMETHODCALLTYPE ReservedDismiss2(int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DismissWithoutTransition() override
    {
        ++syncCloses;
        if (FAILED(syncError)) return syncError;
        ++closes; closeMethod = 0; visible = false; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reserved2() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Reserved3() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Reserved4() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Reserved5() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Reserved7() override { return E_NOTIMPL; }
};
}

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
    check(!close.Take(false), "a hidden menu does not need another close request");
    close.Request(1100);
    check(close.Take(true), "a visible Start dispatches one-way dismissal without a foreground-process gate");
    check(!close.Take(true), "repeat clicks cannot duplicate the pending native dismissal");
    check(!close.Expired(2499) && close.Expired(2500), "a close that never completes has a bounded deadline");
    close.Reset(); close.Request(100);
    check(close.Take(true), "a nonactivating click dispatches native dismissal immediately");
    close.Dispatched(1000);
    check(!close.Expired(2499) && close.Expired(2500),
        "a slow initial handshake leaves a bounded observation window after actual native dispatch");
    close.Dispatched(1500);
    check(close.Expired(2500), "repeat clicks cannot extend the close deadline after dispatch");
    check(ShouldDispatchInitial(true, true) && ShouldDispatchInitial(true, false) &&
            ShouldDispatchInitial(false, false) && !ShouldDispatchInitial(false, true),
        "cancelling a pending open suppresses new input but still dismisses a pre-existing native menu");

    {
        LauncherFixture server; server.visible = false;
        Launcher launcher;
        check(launcher.Bind(&server) == S_OK && launcher.Show() == S_OK &&
                server.shows == 1 && server.showMethod == 11 && server.showFlags == 4 && server.visible,
            "Dock opening uses the native Start-button show method without injecting a Win key");
        check(launcher.Dismiss() == S_OK && !server.visible && server.shows == 1,
            "a repeated button closes the native opening without a second show");
    }
    {
        LauncherFixture server;
        Launcher launcher;
        bool visible = false;
        check(launcher.Bind(&server) == S_OK && server.currentQueries == 1 && server.legacyQueries == 0,
            "Win10RS1/Win11 launcher is selected without probing another ABI");
        check(launcher.Visible(visible) == S_OK && visible, "Shell visibility is read from the native launcher");
        close.Reset(); close.Request(1);
        if (close.Take(visible)) check(launcher.Dismiss() == S_OK, "owned-area dismissal reaches the native COM close method");
        check(server.closes == 1 && server.shows == 0 && server.syncCloses == 1 &&
                launcher.Visible(visible) == S_OK && !visible,
            "second click closes Start without opening anything or requiring its process to own foreground");
        check(launcher.Dismiss() == S_OK && !server.visible && server.shows == 0,
            "one-way native dismissal cannot reopen an already closed menu");
        server.visible = true; server.visibilityError = E_FAIL;
        check(launcher.Visible(visible) == E_FAIL && !visible,
            "a failed Shell visibility query is not a successful stale observation");
    }
    {
        LauncherFixture server; server.current = false;
        Launcher launcher;
        check(launcher.Bind(&server) == S_OK && server.currentQueries == 1 && server.legacyQueries == 1 &&
                launcher.Dismiss() == S_OK && server.closes == 1 && server.closeMethod == 7 && server.shows == 0,
            "early Win10 uses legacy one-way nonanimating dismissal, never a toggle");
        check(launcher.Show() == S_OK && server.shows == 1 && server.showMethod == 11 && server.visible,
            "early Win10 also opens through its native Start-button interface");
    }
    {
        LauncherFixture server; server.syncError = E_NOTIMPL;
        Launcher launcher;
        check(launcher.Bind(&server) == S_OK && launcher.Dismiss() == S_OK &&
                server.syncCloses == 1 && server.closes == 1 && server.closeMethod == 7 && server.shows == 0,
            "an unavailable synchronous method falls back only to one-way nonanimating dismissal");
    }
    {
        LauncherFixture server; server.currentError = E_ACCESSDENIED;
        Launcher launcher;
        check(launcher.Bind(&server) == E_ACCESSDENIED && server.legacyQueries == 0 &&
                launcher.Dismiss() == E_NOINTERFACE && server.closes == 0 && server.shows == 0,
            "a non-ABI Shell error cannot dispatch a guessed method or open fallback during close");
        server.currentError = E_NOINTERFACE; server.legacy = false;
        check(launcher.Bind(&server) == E_NOINTERFACE && server.shows == 0,
            "unsupported Shell interfaces are reported before a protected menu is opened");
    }
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

    // This is the production UI handoff used by Dock search and bar menus.
    // Only native completion and surface-opening callbacks are substituted.
    unsigned surfaces = 0;
    held = true;
    check(lifecycle.Begin(), "start a menu to be replaced by an owned surface");
    check(lifecycle.Defer([&] {
        check(!held && !lifecycle.Pending(), "replacement opens only after Dock cleanup and invocation release");
        ++surfaces;
    }), "a surface request is deferred during native Start");
    check(!lifecycle.Finish(release, fallback) && surfaces == 0,
        "a surface cannot steal foreground while native dismissal is still running");
    lifecycle.Complete(false);
    check(lifecycle.Finish(release, fallback) && surfaces == 1 && fallbacks == 1,
        "native dismissal opens the requested surface once without reopening Start");
    check(!lifecycle.Finish(release, fallback) && surfaces == 1,
        "later UI ticks cannot replay a completed surface handoff");
    held = true;
    check(lifecycle.Begin(), "start another native session for superseded actions");
    lifecycle.Defer([&] { ++surfaces; });
    lifecycle.Defer([&] { surfaces += 10; });
    lifecycle.Complete(false);
    check(lifecycle.Finish(release, fallback) && surfaces == 11,
        "a later bar action replaces the first pending surface rather than opening both");
    check(lifecycle.Begin(), "start a session whose pending handoff is cancelled by outside input");
    lifecycle.Defer([&] { ++surfaces; });
    lifecycle.DiscardContinuation();
    lifecycle.Complete(false);
    check(lifecycle.Finish(release, fallback) && surfaces == 11,
        "an outside click cannot be followed by a delayed popup opening");
    check(lifecycle.Begin(), "start a native session whose dismissal times out");
    lifecycle.Defer([&] { ++surfaces; });
    lifecycle.Complete(false, false);
    check(lifecycle.Finish(release, fallback) && surfaces == 11,
        "unconfirmed native dismissal cannot open a competing surface");

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
    // A replacement surface can already own foreground while Start is visible.
    // Only the one-way close operation and clock are substituted here.
    now = 0; renewals = 0; close.Reset();
    unsigned closeInputs = 0;
    const auto closeResult = WaitForDismissal(false, [&] {
        if (now == 200) close.Request(now);
        const bool visible = now < 500;
        if (close.Take(visible)) ++closeInputs;
        check(now < leaseUntil, "the close request cannot release protection before native Start disappears");
        return visible;
    }, renew, wait, clock);
    check(closeResult == Dismissal::Closed && now == 550 && closeInputs == 1 && renewals == 2,
        "the close request sends one native dismissal and retains protection until Shell confirms it closed");

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
    namespace dockRules = snowdesktop::dock_settings_rules;
    check(!dockRules::ShellPanelVisibility(true, true, false) &&
        dockRules::ShellPanelVisibility(true, false, false) &&
        dockRules::ShellPanelVisibility(true, true, true),
        "Dock Start cannot leave a stale reveal reason, while keyboard Start and other panels remain accessible");
    check(dockRules::ProtectDockStart(true, true, true) &&
        !dockRules::ProtectDockStart(false, true, true) && !dockRules::ProtectDockStart(true, false, true),
        "automatic-hide Start protection is independent of minimize enhancement");
    Snapshot snapshot;
    snapshot.enabled = true;
    snapshot.targetCount = 1;
    snapshot.targets[0].taskbar = 42;
    snapshot.targets[0].protectAutoHideActivation = TRUE;
    snapshot.targets[0].shellPanelVisible = TRUE;
    snapshot.dockStartMenuUntil = 4000;
    check(ShouldSuppressTaskbar(snapshot, 42, 2000), "Dock Start lease retains an auto-hidden bar while Start is visible");
    check(!ShouldSuppressTaskbar(snapshot, 42, 4000), "expired Start protection cannot leave a taskbar hidden");
    snapshot.dockStartMenuUntil = dockRules::StartMenuLeaseDeadline(false, 5000);
    check(ShouldSuppressTaskbar(snapshot, 42, 5001) && !ShouldSuppressTaskbar(snapshot, 42, 5500),
        "Start closing suppression lasts through native settling but expires without another host request");
    check(!ShouldSuppressTaskbar(snapshot, 43, 2000), "a stale unknown taskbar does not inherit a lease");
    snapshot.targets[0].protectAutoHideActivation = FALSE;
    check(!ShouldSuppressTaskbar(snapshot, 42, 2000), "always-visible taskbars do not gain Start suppression");
    snapshot.suppressTaskbar = true;
    snapshot.targets[0].suppressTaskbar = TRUE;
    snapshot.targets[0].shellPanelVisible = FALSE;
    check(ShouldSuppressTaskbar(snapshot, 42, 5000), "lease expiration preserves the independent permanent-hide setting");

    return failures;
}
