#include "shell/start_menu_position.h"
#include "shell/start_menu_protocol.h"
#include "shell/start_menu_window.h"
#include "taskbar_hook/taskbar_hook_protocol.h"
#include <filesystem>
#include <iostream>
#include <thread>

int RunDockStartMenuTests()
{
    using namespace snowdesktop::start_menu;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    const auto equals = [](const std::optional<RECT>& value, RECT expected) {
        return value && EqualRect(&*value, &expected);
    };

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
            "a matching class in another process is insufficient to acquire the Start bridge");
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

    Placement p{{0, 0, 1920, 1080}, {0, 0, 1920, 1080}, {500, 980, 1400, 1060}, Edge::Bottom, 8};
    check(equals(Position(p, {600, 700}), {660, 272, 1260, 972}),
        "bottom menu centers on the screen and clears the fully expanded island Dock");
    p.avoid = {30, 980, 430, 1060};
    check(equals(Position(p, {600, 700}), {660, 272, 1260, 972}),
        "an off-center Dock does not move the menu's screen-centered axis");
    p.edge = Edge::Top; p.avoid = {0, 0, 1920, 80};
    check(equals(Position(p, {600, 700}), {660, 88, 1260, 788}), "top merged bar reserves its full strip");
    p.edge = Edge::Left; p.avoid = {20, 100, 100, 900};
    check(equals(Position(p, {600, 700}), {108, 190, 708, 890}), "left Dock opens to the right, vertically centered");
    p.edge = Edge::Right; p.avoid = {1820, 100, 1900, 900};
    check(equals(Position(p, {600, 700}), {1212, 190, 1812, 890}), "right Dock opens to the left");
    p = {{-1920, -200, 0, 880}, {-1920, -160, 0, 880}, {-1920, 780, 0, 880}, Edge::Bottom, 12};
    check(equals(Position(p, {900, 750}), {-1410, 18, -510, 768}),
        "physical-pixel geometry supports negative monitor origins and scaled menu/gap sizes");
    check(!Position(p, {900, 1000}), "a menu that cannot fit falls back instead of overlapping or resizing");
    check(!Position(p, {2000, 400}), "an overly wide menu is not clipped into the monitor");
    p.work = {1, 1, 1, 1};
    check(!Position(p, {10, 10}), "stale work-area geometry is rejected");
    p = {{0, 0, 1920, 1080}, {600, 0, 1920, 1080}, {0, 1000, 1920, 1080}, Edge::Bottom, 8};
    check(!Position(p, {1000, 600}),
        "asymmetric reserved space falls back instead of shifting the screen-centered menu sideways");
    p = {{0, 0, 1920, 1080}, {0, 400, 1920, 1080}, {0, 0, 80, 1080}, Edge::Left, 8};
    check(!Position(p, {600, 500}),
        "asymmetric reserved space preserves vertical screen centering or falls back");

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

    // Exercise the shipped Shell-command receiver and real HWND restoration.
    // The fixture is hidden; no Start menu, Explorer or physical input is used.
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    auto dll = std::filesystem::path(executable).parent_path().parent_path() / L"SnowDesktopTaskbarHook.dll";
    if (!std::filesystem::is_regular_file(dll))
        dll = dll.parent_path() / L"SnowDesktop.Runtime" / dll.filename();
    const HMODULE module = LoadLibraryW(dll.c_str());
    const auto receiver = module ? reinterpret_cast<HOOKPROC>(GetProcAddress(module, "SnowDesktopStartPinHookProc")) : nullptr;
    check(receiver != nullptr, "load the production Start placement receiver");
    if (!receiver) { if (module) FreeLibrary(module); return failures; }
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Start positioning fixture",
        WS_POPUP, 0, 0, 240, 260, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create hidden native compact-menu fixture");
    if (!window) { FreeLibrary(module); return failures; }
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &info);
    const auto screen = info.rcMonitor;
    SetWindowPos(window, nullptr, screen.left, screen.top, 240, 260, SWP_NOZORDER | SWP_NOACTIVATE);
    RECT original{}; GetWindowRect(window, &original);
    const DWORD process = GetCurrentProcessId();
    const std::uint64_t token = GetTickCount64();
    const auto name = MappingName(process, token);
    const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Request), name.c_str());
    auto* request = mapping ? static_cast<Request*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Request))) : nullptr;
    check(request != nullptr, "create private session request mapping");
    if (request)
    {
        *request = Request{};
        request->owner = process; request->target = process; request->token = token;
        request->ownerWindow = reinterpret_cast<std::uintptr_t>(window);
        request->placement = {screen, screen, {screen.left, screen.bottom - 64, screen.right, screen.bottom}, Edge::Bottom, 8};
        InterlockedExchange64(&request->heartbeat, static_cast<LONG64>(GetTickCount64()));
        const UINT message = RegisterWindowMessageW(kMessage);
        CWPSTRUCT invocation{static_cast<LPARAM>(token), process, message, window};
        receiver(HC_ACTION, 0, reinterpret_cast<LPARAM>(&invocation));
        check(request->status == Prepared, "production receiver acknowledges only a validated owner/window request");
        check(PulsePlacement(window) == ERROR_SUCCESS,
            "production worker pulse advances a CoreWindow-style receiver without pumping WM_TIMER");
        RECT placed{}; GetWindowRect(window, &placed);
        const auto expected = Position(request->placement, {240, 260});
        check(request->status == Positioned && equals(expected, placed),
            "compact adapter positions the actual HWND through the production subclass");
        const RECT hitBounds = ReadBounds(*request);
        check(request->stage == PlacementApplied && request->ticks > 0 && EqualRect(&hitBounds, &placed),
            "receiver publishes the actual menu rectangle for local desktop hit testing");
        InterlockedExchange(&request->active, FALSE);
        SendMessageW(window, message, 0, 0);
        RECT restored{}; GetWindowRect(window, &restored);
        check(request->status == Closed && EqualRect(&restored, &original),
            "ending the invocation restores the exact original native window geometry");
        const RECT closedBounds = ReadBounds(*request);
        check(IsRectEmpty(&closedBounds), "closed receiver clears the fullscreen-host hit region");

        request->active = TRUE; request->status = Pending; request->target = process + 1;
        receiver(HC_ACTION, 0, reinterpret_cast<LPARAM>(&invocation));
        check(request->status == Pending, "another process identity cannot acquire the native adapter");
        request->target = process; request->status = Pending;
        InterlockedExchange64(&request->heartbeat, static_cast<LONG64>(GetTickCount64()));
        receiver(HC_ACTION, 0, reinterpret_cast<LPARAM>(&invocation));
        SendMessageW(window, message, 0, 0);
        InterlockedExchange64(&request->heartbeat, static_cast<LONG64>(GetTickCount64() - 4000));
        SendMessageW(window, message, 0, 0);
        GetWindowRect(window, &restored);
        check(request->status == Closed && EqualRect(&restored, &original),
            "an expired owner heartbeat restores geometry even without a graceful host close");

        request->active = TRUE; request->status = Pending; request->version = 1;
        receiver(HC_ACTION, 0, reinterpret_cast<LPARAM>(&invocation));
        check(request->status == Pending, "a previous protocol cannot acquire the new placement receiver");
        request->version = 2;
        InterlockedExchange64(&request->heartbeat, static_cast<LONG64>(GetTickCount64()));
        receiver(HC_ACTION, 0, reinterpret_cast<LPARAM>(&invocation));
        const HANDLE pulseFinished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        check(pulseFinished != nullptr, "create private pulse completion signal");
        if (pulseFinished)
        {
            DWORD pulseError = ERROR_SUCCESS;
            std::thread stalledPulse([&] {
                pulseError = PulsePlacement(window);
                SetEvent(pulseFinished);
            });
            // Intentionally do not dispatch this private window's messages:
            // the exact production send must time out rather than wait forever.
            const DWORD completed = WaitForSingleObject(pulseFinished, 1500);
            check(completed == WAIT_OBJECT_0, "an unresponsive receiver cannot indefinitely block the placement worker");
            if (completed != WAIT_OBJECT_0)
            {
                // Bound the fixture's failure path too, by servicing the send.
                MSG pending{};
                PeekMessageW(&pending, nullptr, 0, 0, PM_NOREMOVE);
            }
            stalledPulse.join();
            check(pulseError == ERROR_TIMEOUT && request->status == Prepared,
                "a stalled receiver reports timeout without claiming successful placement");
            CloseHandle(pulseFinished);
        }
        InterlockedExchange(&request->active, FALSE);
        PulsePlacement(window);
        UnmapViewOfFile(request);
    }
    if (mapping) CloseHandle(mapping);
    DestroyWindow(window);
    FreeLibrary(module);
    return failures;
}
