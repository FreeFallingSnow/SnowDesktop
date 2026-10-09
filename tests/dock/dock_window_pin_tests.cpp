#include "dock/dock_window_pin.h"
#include "dock/dock_window_preview.h"
#include "dock/dock_window_preview_layout.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <iostream>

namespace
{
constexpr wchar_t kPinProperty[] = L"SnowDesktop.DockWindowPin";
constexpr wchar_t kFixtureClass[] = L"SnowDesktopDockPinFixture";

struct ScopedDpiContext
{
    explicit ScopedDpiContext(DPI_AWARENESS_CONTEXT context)
        : previous(SetThreadDpiAwarenessContext(context)) {}
    ~ScopedDpiContext()
    {
        if (previous) SetThreadDpiAwarenessContext(previous);
    }
    DPI_AWARENESS_CONTEXT previous;
};
thread_local HWND observedWindow = nullptr;
thread_local unsigned observedLocations = 0;
thread_local LONG observedObject = 0;

void CALLBACK ObserveLocation(HWINEVENTHOOK, DWORD, HWND window,
    LONG object, LONG, DWORD, DWORD)
{
    if (window == observedWindow)
    {
        ++observedLocations;
        observedObject = object;
    }
}

void PrintRect(const char* name, const RECT& rect)
{
    std::cerr << name << '=' << rect.left << ',' << rect.top << ','
        << rect.right << ',' << rect.bottom << ' ';
}

HWND MakeWindow()
{
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    cls.lpszClassName = kFixtureClass;
    RegisterClassW(&cls);
    // Fixtures default off-screen. The aspect test briefly composes only its
    // own nonactivating windows; it never operates the desktop host.
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kFixtureClass, L"pin-fixture",
        WS_OVERLAPPEDWINDOW, -12000, -12000, 300, 180,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void MovePreviewOffScreen(DockWindowPreview& preview)
{
    const RECT group = preview.GetBounds();
    for (const HWND window : preview.GetWindows())
    {
        RECT rect{};
        if (!GetWindowRect(window, &rect)) continue;
        SetWindowPos(window, nullptr,
            -12000 + rect.left - group.left, -12000 + rect.top - group.top,
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

POINT Center(const RECT& rect)
{
    return {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
}

bool IsAbove(HWND window, HWND target)
{
    for (HWND previous = GetWindow(target, GW_HWNDPREV); previous;
            previous = GetWindow(previous, GW_HWNDPREV))
        if (previous == window) return true;
    return false;
}

HWND FindFixtureManager()
{
    HWND manager = nullptr;
    while ((manager = FindWindowExW(HWND_MESSAGE, manager,
                L"SnowDesktopDockWindowPinManager", nullptr)) != nullptr)
    {
        DWORD process = 0;
        if (GetWindowThreadProcessId(manager, &process) == GetCurrentThreadId() &&
                process == GetCurrentProcessId()) return manager;
    }
    return nullptr;
}
}

int RunDockWindowPinTests()
{
    const ScopedDpiContext physicalCoordinates(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed)
        {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };
    const DockWindowPreviewLayout aspectLayout = CalculateDockWindowPreviewLayout(
        {{1600, 900}, {800, 1200}}, 1200, 700, 96);
    const auto aspectCards = CalculateDockWindowPreviewLayoutCardRects(aspectLayout, 96);
    check(aspectCards.size() == 2 &&
            aspectCards[0].bottom - aspectCards[0].top == 152 &&
            aspectCards[1].bottom - aspectCards[1].top == 152 &&
            aspectCards[0].right - aspectCards[0].left == 211 &&
            aspectCards[1].right - aspectCards[1].left == 85,
        "equal-height landscape and portrait previews retain proportional widths with five-pixel insets");
    for (const UINT actionDpi : {96U, 120U, 144U})
    {
        const RECT portrait{0, 0, MulDiv(55, actionDpi, 96), MulDiv(152, actionDpi, 96)};
        const RECT close = CalculateDockWindowPreviewCloseButtonRect(portrait, actionDpi);
        const RECT pin = CalculateDockWindowPreviewPinButtonRect(portrait, actionDpi);
        check(!IsRectEmpty(&pin) && !IsRectEmpty(&close) &&
                pin.left >= MulDiv(6, actionDpi, 96) && close.right < portrait.right &&
                pin.right < close.left && pin.top == close.top && pin.bottom == close.bottom &&
                pin.right - pin.left == pin.bottom - pin.top &&
                close.right - close.left == close.bottom - close.top &&
                !IsPointInDockWindowPreviewCloseButton(Center(pin), portrait, actionDpi),
            "narrow portrait cards keep separate square pin and close hit regions without widening the card");
    }
    const DockWindowPreviewLayout boundedLayout = CalculateDockWindowPreviewLayout(
        {{1600, 900}, {800, 1200}, {1200, 800}}, 600, 300, 96);
    check(boundedLayout.panelWidth <= 600 && boundedLayout.panelHeight <= 300 &&
            boundedLayout.cardWidths.size() == 3,
        "aspect-preserving previews fit the work area without imposing a common card width");
    const auto wrappedLayout = CalculateDockWindowPreviewLayout(
        {{1600, 900}, {800, 1200}, {1200, 800}}, 300, 400, 96);
    check(wrappedLayout.rows == 2 && wrappedLayout.cardHeight == 152 &&
            wrappedLayout.cardRects.size() == 3 &&
            wrappedLayout.cardRects[1].top > wrappedLayout.cardRects[0].bottom &&
            wrappedLayout.cardRects[2].left > wrappedLayout.cardRects[1].right,
        "variable-width independent cards wrap into separated rows while retaining their common height");
    HWND target = MakeWindow();
    HWND other = MakeWindow();
    check(target && other, "native window pin fixtures are created");
    if (!target || !other)
    {
        if (target) DestroyWindow(target);
        if (other) DestroyWindow(other);
        return failures;
    }

    ShowWindow(target, SW_SHOWNOACTIVATE);
    const HWND foreground = GetForegroundWindow();
    DockWindowPin pins;
    check(!pins.Toggle(nullptr), "invalid handles cannot create a pin");
    {
        const ScopedDpiContext unawareCaller(DPI_AWARENESS_CONTEXT_UNAWARE);
        check(pins.Toggle(target) && DockWindowPin::IsPinned(target),
            "pinning promotes TOPMOST even when invoked by a DPI-unaware caller");
    }
    HWND border = static_cast<HWND>(GetPropW(target, kPinProperty));
    check(border && IsWindowVisible(border), "a pinned visible window has a live border");
    check(!DockWindowPin::IsPinned(other) && GetForegroundWindow() == foreground,
        "pinning leaves other windows and foreground focus unchanged");
    if (border)
    {
        const LONG_PTR style = GetWindowLongPtrW(border, GWL_EXSTYLE);
        check((style & (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) ==
                (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE),
            "the border passes clicks through without activating");
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        RECT bounds{};
        GetWindowRect(border, &bounds);
        check(GetWindowRgn(border, region) != ERROR &&
                !PtInRegion(region, (bounds.right - bounds.left) / 2,
                    (bounds.bottom - bounds.top) / 2),
            "the border region excludes the application content");
        DeleteObject(region);
        const HWND manager = FindFixtureManager();
        check(manager != nullptr, "the pin session has its own native event dispatcher");
        // Disable the safety poll so a missing LOCATIONCHANGE subscription
        // cannot pass this regression after the next timer tick.
        if (manager) KillTimer(manager, 1);
        observedWindow = target;
        observedLocations = 0;
        const HWINEVENTHOOK observation = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,
            EVENT_OBJECT_LOCATIONCHANGE, nullptr, ObserveLocation, 0, 0, WINEVENT_OUTOFCONTEXT);
        SetWindowPos(target, nullptr, -11000, -11500, 380, 240,
            SWP_NOZORDER | SWP_NOACTIVATE);
        RECT frame{};
        DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame));
        const ULONGLONG deadline = GetTickCount64() + 500;
        bool followed = false;
        do
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            GetWindowRect(border, &bounds);
            followed = bounds.left < frame.left && bounds.top < frame.top &&
                bounds.right > frame.right && bounds.bottom > frame.bottom &&
                bounds.left > frame.left - 100 && bounds.top > frame.top - 100;
            if (!followed) MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (!followed && GetTickCount64() < deadline);
        check(followed, "native location events move and resize the ring with timer fallback disabled");
        if (!followed)
        {
            RECT native{};
            GetWindowRect(target, &native);
            PrintRect("native", native);
            PrintRect("dwm", frame);
            PrintRect("ring", bounds);
            std::cerr << "events=" << observedLocations << " object=" << observedObject << '\n';
        }
        // Deliver native WinEvents without dispatching the posted application
        // backlog. A second PostMessage hop must not gate each motion sample.
        constexpr UINT backlogMessage = WM_APP + 42;
        bool backlogCreated = manager != nullptr;
        if (manager)
            for (int index = 0; index < 256; ++index)
                backlogCreated = PostMessageW(manager, backlogMessage, 0, 0) != FALSE && backlogCreated;
        check(backlogCreated, "the native follow regression creates a real posted-message backlog");
        int samplesFollowed = 0;
        unsigned slowestSampleMs = 0;
        for (int sample = 1; sample <= 8; ++sample)
        {
            const unsigned locationsBefore = observedLocations;
            SetWindowPos(target, nullptr, -11000 + sample * 37, -11500 + sample * 19,
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT expected{};
            DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &expected, sizeof(expected));
            const int stroke = MulDiv(6, static_cast<int>(GetDpiForWindow(target)), 96);
            InflateRect(&expected, stroke, stroke);
            const ULONGLONG started = GetTickCount64();
            bool aligned = false;
            do
            {
                MSG pending{};
                PeekMessageW(&pending, nullptr, WM_NULL, WM_NULL, PM_NOREMOVE);
                RECT actual{};
                GetWindowRect(border, &actual);
                aligned = observedLocations > locationsBefore && EqualRect(&actual, &expected);
                if (!aligned) MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            } while (!aligned && GetTickCount64() - started < 500);
            if (aligned) ++samplesFollowed;
            slowestSampleMs = std::max(slowestSampleMs,
                static_cast<unsigned>(GetTickCount64() - started));
        }
        MSG backlog{};
        check(PeekMessageW(&backlog, manager, backlogMessage, backlogMessage, PM_NOREMOVE) != FALSE,
            "motion checks leave ordinary posted application work queued");
        check(samplesFollowed == 8,
            "each native motion sample aligns the ring before posted application work is dispatched");
        std::cout << "Pin motion samples before posted dispatch: " << samplesFollowed
            << "/8; maximum native sample wait: " << slowestSampleMs << " ms\n";
        while (PeekMessageW(&backlog, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&backlog);
            DispatchMessageW(&backlog);
        }
        if (observation) UnhookWinEvent(observation);
        observedWindow = nullptr;
        if (manager) SetTimer(manager, 1, 100, nullptr);

        ShowWindow(target, SW_SHOWMINNOACTIVE);
        pins.Refresh();
        check(!IsWindowVisible(border), "minimization hides the ring while retaining the pin");
        ShowWindow(target, SW_SHOWNOACTIVATE);
        pins.Refresh();
        check(DockWindowPin::IsPinned(target) && IsWindowVisible(border),
            "restoring a minimized window restores its border");
        ShowWindow(target, SW_HIDE);
        pins.Refresh();
        check(!IsWindowVisible(border), "hidden windows do not leave a detached border");
    }
    check(pins.Toggle(target) && !DockWindowPin::IsPinned(target) && !IsWindow(border),
        "unpinning demotes the window and destroys the ring");
    check(!GetPropW(target, kPinProperty), "unpinning releases the target identity marker");

    check(pins.Toggle(target) && pins.Toggle(other), "multiple windows can be pinned independently");
    border = static_cast<HWND>(GetPropW(target, kPinProperty));
    // An app can cancel TOPMOST itself; the Dock must not fight that change.
    SetWindowPos(target, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    pins.Refresh();
    check(!DockWindowPin::IsPinned(target) && !IsWindow(border) && DockWindowPin::IsPinned(other),
        "external unpinning retires only the affected window's border");
    check(pins.Toggle(target), "a window can be pinned again after external cancellation");
    border = static_cast<HWND>(GetPropW(target, kPinProperty));
    RemovePropW(target, kPinProperty);
    pins.Refresh();
    check(DockWindowPin::IsPinned(target) && !IsWindow(border),
        "lost ownership must not demote an unrelated or reused target handle");
    SetWindowPos(target, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    pins.Clear();
    check(!DockWindowPin::IsPinned(other), "session teardown restores only pins it still owns");
    SetWindowPos(other, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    {
        DockWindowPin independentSession;
        check(independentSession.Toggle(target), "an independent session owns its new pin");
    }
    check(!DockWindowPin::IsPinned(target) && DockWindowPin::IsPinned(other),
        "session destruction does not remove pre-existing TOPMOST from other windows");
    check(pins.Toggle(other) && !DockWindowPin::IsPinned(other),
        "the native TOPMOST state can also be cancelled from the pin toggle");

    // Exercise the production message entry points, with callbacks as the app
    // boundary. These do not claim actual Dock hover or visual acceptance.
    HWND closed = nullptr;
    HWND activated = nullptr;
    {
        DockWindowPreview preview;
        check(preview.Initialize(GetModuleHandleW(nullptr),
            [&](HWND window) { activated = window; },
            [&](HWND window) { closed = window; }), "standalone preview fixture initializes");
        const auto show = [&] {
            preview.Show({{target, L"fixture"}}, {0, 0, 20, 20}, DockPosition::Bottom, false);
            MovePreviewOffScreen(preview);
        };
        show();
        RECT card{};
        GetClientRect(preview.GetWindow(), &card);
        const UINT dpi = GetDpiForWindow(preview.GetWindow());
        const POINT pin = Center(CalculateDockWindowPreviewPinButtonRect(card, dpi));
        SendMessageW(preview.GetWindow(), WM_LBUTTONUP, 0, MAKELPARAM(pin.x, pin.y));
        check(DockWindowPin::IsPinned(target) && preview.IsVisible() && !closed && !activated,
            "left-clicking the pin changes TOPMOST without closing or activating the thumbnail");
        check(IsAbove(preview.GetWindow(), target),
            "pinning must keep the preview above its target so the unpin button remains available");
        // Negative control: recreating promotion without the panel handoff
        // must expose the obscured-controls failure to the same native oracle.
        SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        check(!IsAbove(preview.GetWindow(), target),
            "the layer regression detects native target promotion without a preview handoff");
        SendMessageW(preview.GetWindow(), WM_LBUTTONUP, 0, MAKELPARAM(pin.x, pin.y));
        SendMessageW(preview.GetWindow(), WM_LBUTTONUP, 0, MAKELPARAM(pin.x, pin.y));
        check(DockWindowPin::IsPinned(target) && IsAbove(preview.GetWindow(), target),
            "repeated unpin/pin through the production entry restores accessible controls");
        preview.Hide();
        check(DockWindowPin::IsPinned(target) && preview.IsCleared(),
            "closing the thumbnail panel retains the pinned application");
        show();
        SendMessageW(preview.GetWindow(), WM_LBUTTONUP, 0, MAKELPARAM(pin.x, pin.y));
        check(!DockWindowPin::IsPinned(target) && preview.IsVisible(),
            "clicking the active pin cancels it while leaving the panel open");
        SendMessageW(preview.GetWindow(), WM_MBUTTONUP, 0, MAKELPARAM(pin.x, pin.y));
        check(closed == target && !activated && preview.IsCleared() && !DockWindowPin::IsPinned(target),
            "middle-click even on the pin closes the target through the native close callback");
        closed = nullptr;
        show();
        const POINT close = Center(CalculateDockWindowPreviewCloseButtonRect(card, dpi));
        SendMessageW(preview.GetWindow(), WM_LBUTTONUP, 0, MAKELPARAM(close.x, close.y));
        check(closed == target && !activated, "the existing close button retains its close action");
        closed = nullptr;
        show();
        SendMessageW(preview.GetWindow(), WM_LBUTTONUP, 0, MAKELPARAM(20, card.bottom - 20));
        check(activated == target && !closed, "left-clicking the thumbnail content still activates it");

        closed = nullptr;
        activated = nullptr;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        SetWindowPos(target, nullptr, work.left + 16, work.top + 16, 380, 240,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(other, nullptr, work.left + 420, work.top + 16, 120, 300,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(target);
        UpdateWindow(other);
        DwmFlush();
        const auto showGroup = [&] {
            preview.Show({{target, L"landscape"}, {other, L"portrait"}},
                {0, 0, 20, 20}, DockPosition::Bottom, false);
            MovePreviewOffScreen(preview);
        };
        showGroup();
        const auto groupWindows = preview.GetWindows();
        check(groupWindows.size() == 2 && groupWindows[0] != groupWindows[1] &&
                GetAncestor(groupWindows[0], GA_ROOT) == groupWindows[0] &&
                GetAncestor(groupWindows[1], GA_ROOT) == groupWindows[1],
            "two application windows have separate top-level thumbnail popups");
        if (groupWindows.size() == 2)
        {
            RECT first{}, second{}, firstScreen{}, secondScreen{};
            GetClientRect(groupWindows[0], &first);
            GetClientRect(groupWindows[1], &second);
            GetWindowRect(groupWindows[0], &firstScreen);
            GetWindowRect(groupWindows[1], &secondScreen);
            check(first.bottom == second.bottom && first.right > second.right &&
                    secondScreen.left > firstScreen.right,
                "independent landscape and portrait cards have equal heights, distinct widths and a background-free gap");
            if (!(first.bottom == second.bottom && first.right > second.right &&
                    secondScreen.left > firstScreen.right))
            {
                PrintRect("first-client", first);
                PrintRect("second-client", second);
                PrintRect("first-screen", firstScreen);
                PrintRect("second-screen", secondScreen);
                std::cerr << '\n';
                for (const HWND source : {target, other})
                {
                    RECT native{};
                    GetWindowRect(source, &native);
                    PrintRect("source", native);
                    HTHUMBNAIL thumbnail = nullptr;
                    SIZE size{};
                    if (SUCCEEDED(DwmRegisterThumbnail(groupWindows[0], source, &thumbnail)))
                    {
                        DwmQueryThumbnailSourceSize(thumbnail, &size);
                        DwmUnregisterThumbnail(thumbnail);
                    }
                    std::cerr << "dwm-source=" << size.cx << 'x' << size.cy << '\n';
                }
            }
            check(preview.ContainsInteractionPoint(Center(secondScreen)),
                "the second popup participates in preview interaction retention");
            const POINT secondPin = Center(CalculateDockWindowPreviewPinButtonRect(second, dpi));
            SendMessageW(groupWindows[1], WM_LBUTTONUP, 0, MAKELPARAM(secondPin.x, secondPin.y));
            check(DockWindowPin::IsPinned(other) && !DockWindowPin::IsPinned(target) &&
                    IsAbove(groupWindows[0], other) && IsAbove(groupWindows[1], other),
                "pinning the second popup targets only its source and keeps all preview controls above it");
            show();
            check(!IsWindow(groupWindows[1]) && DockWindowPin::IsPinned(other),
                "shrinking the preview group retires surplus popups without dropping their application's pin");
            showGroup();
            const HWND secondPopup = preview.GetWindows()[1];
            SendMessageW(secondPopup, WM_LBUTTONUP, 0, MAKELPARAM(secondPin.x, secondPin.y));
            check(!DockWindowPin::IsPinned(other), "a recreated second popup can cancel the retained source pin");
            SendMessageW(secondPopup, WM_MBUTTONUP, 0, MAKELPARAM(20, second.bottom - 20));
            check(closed == other && !activated && preview.IsCleared(),
                "middle-click coordinates in the second popup close its source and hide the complete group");
        }
    }
    check(pins.Toggle(target), "prepare destroyed-window cleanup fixture");
    border = static_cast<HWND>(GetPropW(target, kPinProperty));
    DestroyWindow(target);
    pins.Refresh();
    check(!IsWindow(border), "destroyed application windows retire their border");
    pins.Clear();
    DestroyWindow(other);
    UnregisterClassW(kFixtureClass, GetModuleHandleW(nullptr));
    return failures;
}
