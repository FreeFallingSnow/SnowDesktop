#include "dock/dock_window_pin.h"
#include "dock/dock_window_preview.h"
#include "dock/dock_window_preview_layout.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <iostream>

namespace
{
constexpr wchar_t kPinProperty[] = L"SnowDesktop.DockWindowPin";

HWND MakeWindow()
{
    // Native module fixtures stay off-screen and never activate the desktop host.
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"pin-fixture",
        WS_POPUP | WS_THICKFRAME, -32000, -32000, 300, 180,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void MovePreviewOffScreen(DockWindowPreview& preview)
{
    SetWindowPos(preview.GetWindow(), nullptr, -32000, -32000, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
    check(aspectCards.size() == 2 && aspectLayout.cardWidth == 210 &&
            aspectCards[0].bottom - aspectCards[0].top == 152 &&
            aspectCards[1].bottom - aspectCards[1].top == 339,
        "equal-width landscape and portrait thumbnails retain their source ratio with five-pixel insets");
    const DockWindowPreviewLayout boundedLayout = CalculateDockWindowPreviewLayout(
        {{1600, 900}, {800, 1200}, {1200, 800}}, 600, 300, 96);
    check(boundedLayout.panelWidth <= 600 && boundedLayout.panelHeight <= 300 &&
            boundedLayout.cardHeights.size() == 3,
        "aspect-preserving previews fit the work area without imposing a common card height");
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
    check(pins.Toggle(target) && DockWindowPin::IsPinned(target),
        "pinning promotes the selected native window into TOPMOST");
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
        SetWindowPos(target, nullptr, -31000, -31500, 380, 240,
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
    }
    check(pins.Toggle(target), "prepare destroyed-window cleanup fixture");
    border = static_cast<HWND>(GetPropW(target, kPinProperty));
    DestroyWindow(target);
    pins.Refresh();
    check(!IsWindow(border), "destroyed application windows retire their border");
    pins.Clear();
    DestroyWindow(other);
    return failures;
}
