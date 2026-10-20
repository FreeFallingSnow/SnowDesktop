#include "system/tray/tray_focus.h"
#include "app/popup/popup_window_pair_z_order.h"

#include <windows.h>
#include <cstdlib>
#include <iostream>

namespace
{
namespace tray = snowdesktop::tray;
namespace pair = snowdesktop::popup_window_pair_z_order;

void Require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL native tray menu: " << message << '\n'; std::exit(1); }
}

struct NativeMenuProbe
{
    HWND owner = nullptr, panel = nullptr, menuWindow = nullptr;
    tray::MenuRetentionSession session;
    tray::MenuPopupBinding shown;
    DWORD started = 0;
    bool sawShow = false, checked = false, menuLoopConfirmed = false;
    bool animationBindingAccepted = false, shownBindingRetained = false, fallbackFound = false;
    bool layerModesPassed = false, missingAnchorPreserved = false, otherStylesRejected = false;
    LONG_PTR shownExtendedStyle = 0, currentExtendedStyle = 0;
};
thread_local NativeMenuProbe* activeProbe = nullptr;

void CALLBACK MenuShown(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD)
{
    if (!activeProbe || event != EVENT_OBJECT_SHOW || !window || object != OBJID_WINDOW || child != CHILDID_SELF) return;
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    if (wcscmp(name, L"#32768") != 0) return;
    auto& probe = *activeProbe;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    probe.shown = {reinterpret_cast<std::uint64_t>(window), reinterpret_cast<std::uint64_t>(GetWindow(window, GW_OWNER)),
        process, thread, GetWindowLongPtrW(window, GWL_STYLE), GetWindowLongPtrW(window, GWL_EXSTYLE), true};
    probe.sawShow = true;
    probe.shownExtendedStyle = probe.shown.extendedStyle;
    tray::MenuPopupBindings observed{};
    observed[0] = probe.shown;
    probe.session.Active(probe.owner, observed);
}

BOOL CALLBACK FindNativeMenu(HWND window, LPARAM parameter)
{
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    if (wcscmp(name, L"#32768") == 0 && IsWindowVisible(window))
    {
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }
    return TRUE;
}

bool CheckLayerModes(NativeMenuProbe& probe, const RECT& bounds)
{
    bool passed = true;
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        const auto create = [&] {
            const auto window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
                bounds.left, bounds.top, 160, 80, nullptr, nullptr, nullptr, nullptr);
            if (window) ShowWindow(window, SW_SHOWNOACTIVATE);
            return window;
        };
        HWND content = create();
        HWND backdrop = mode != 1 ? create() : nullptr;
        HWND separator = mode == 3 ? create() : nullptr;
        passed = content && (mode == 1 || backdrop) && (mode != 3 || separator) && passed;
        const bool topmost = mode != 2;
        passed = pair::Apply(content, backdrop, topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            topmost, {bounds.left, bounds.top}, {160, 80}) && passed;
        SetWindowPos(probe.menuWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (separator) SetWindowPos(separator, content, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        const bool before = pair::IsAbove(probe.menuWindow, content);
        const auto placement = probe.session.LayerPlacement(bounds);
        passed = placement.above == probe.menuWindow && placement.source == 2 && !placement.preserveOrder && passed;
        if (!placement.preserveOrder)
            passed = pair::Apply(content, backdrop, placement.above, pair::IsTopmost(placement.above),
                {bounds.left, bounds.top}, {160, 80}, placement.above) && passed;
        passed = before && pair::IsAbove(probe.menuWindow, content) && passed;
        if (separator) DestroyWindow(separator);
        if (backdrop) DestroyWindow(backdrop);
        if (content) DestroyWindow(content);
    }
    return passed;
}

LRESULT CALLBACK ProbeProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_TIMER && activeProbe)
    {
        auto& probe = *activeProbe;
        const DWORD elapsed = GetTickCount() - probe.started;
        if (elapsed < 250) return 0;
        EnumThreadWindows(GetCurrentThreadId(), FindNativeMenu, reinterpret_cast<LPARAM>(&probe.menuWindow));
        if (probe.menuWindow)
        {
            DWORD process = 0;
            const DWORD thread = GetWindowThreadProcessId(probe.menuWindow, &process);
            probe.currentExtendedStyle = GetWindowLongPtrW(probe.menuWindow, GWL_EXSTYLE);
            // The actual menu supplies HWND, PID, thread, owner, class and style.
            // Toggle only the recorded animation bit so this also regresses on
            // systems whose menu fade is disabled or has a different duration.
            tray::MenuPopupBinding animated{reinterpret_cast<std::uint64_t>(probe.menuWindow),
                reinterpret_cast<std::uint64_t>(GetWindow(probe.menuWindow, GW_OWNER)), process, thread,
                GetWindowLongPtrW(probe.menuWindow, GWL_STYLE), probe.currentExtendedStyle ^ WS_EX_LAYERED, true};
            probe.animationBindingAccepted = probe.session.LivePopup(animated);
            auto changedBand = animated;
            changedBand.extendedStyle = probe.currentExtendedStyle ^ WS_EX_TOPMOST;
            probe.otherStylesRejected = !probe.session.LivePopup(changedBand);
            probe.session.Active(probe.owner, {});
            probe.shownBindingRetained = !probe.sawShow || (probe.session.LivePopup(probe.shown) &&
                probe.session.popups[0].window == probe.shown.window);
            probe.menuLoopConfirmed = probe.session.menuOwner == probe.owner;
            RECT bounds{};
            GetWindowRect(probe.menuWindow, &bounds);
            probe.fallbackFound = probe.session.NativeMenuOverlapping(bounds) == probe.menuWindow;
            probe.session.popups = {};
            probe.layerModesPassed = CheckLayerModes(probe, bounds);
            const DWORD menuThread = probe.session.menuThread;
            probe.session.menuThread = 0;
            const auto missing = probe.session.LayerPlacement(bounds);
            probe.missingAnchorPreserved = !missing.above && missing.preserveOrder;
            probe.session.menuThread = menuThread;
            probe.checked = true;
            KillTimer(window, 1);
            EndMenu();
        }
        else if (elapsed >= 2000) { KillTimer(window, 1); EndMenu(); }
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}

// The existing menu-test driver already joined a private desktop. These are
// genuine User32 menu HWNDs; the user's input desktop is never switched.
void RunNativeTrayMenuLayerTests()
{
    NativeMenuProbe probe;
    WNDCLASSW definition{};
    definition.lpfnWndProc = ProbeProc;
    definition.hInstance = GetModuleHandleW(nullptr);
    definition.lpszClassName = L"SnowDesktop.NativeTrayMenuLayerTests";
    Require(RegisterClassW(&definition) != 0, "register native menu owner");
    probe.owner = CreateWindowW(definition.lpszClassName, L"", WS_POPUP, 0, 0, 320, 240,
        nullptr, nullptr, definition.hInstance, nullptr);
    probe.panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
        100, 100, 180, 180, nullptr, nullptr, definition.hInstance, nullptr);
    Require(probe.owner && probe.panel, "create test-owned panel and notification owner");
    ShowWindow(probe.owner, SW_SHOWNOACTIVATE);
    ShowWindow(probe.panel, SW_SHOWNOACTIVATE);
    probe.session.Arm(probe.owner, GetCurrentProcessId(), probe.panel, probe.panel);
    const HMENU menu = CreatePopupMenu();
    Require(menu && AppendMenuW(menu, MF_STRING, 1, L"Native tray menu"), "create genuine native popup menu");
    activeProbe = &probe;
    const auto hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, MenuShown,
        GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    Require(hook != nullptr, "observe the real native menu SHOW event");
    probe.started = GetTickCount();
    SetTimer(probe.owner, 1, 50, nullptr);
    TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY, 120, 120, probe.owner, nullptr);
    KillTimer(probe.owner, 1);
    UnhookWinEvent(hook);
    activeProbe = nullptr;
    DestroyMenu(menu);
    DestroyWindow(probe.panel);
    DestroyWindow(probe.owner);
    UnregisterClassW(definition.lpszClassName, definition.hInstance);
    std::cout << "Native tray menu trace: show=" << probe.sawShow << " checked=" << probe.checked
        << " animationBinding=" << probe.animationBindingAccepted << " fallback=" << probe.fallbackFound
        << " ex=" << std::hex << probe.shownExtendedStyle << "->" << probe.currentExtendedStyle << std::dec << '\n';
    Require(probe.checked && probe.menuLoopConfirmed, "inspect the actual active native menu loop");
    Require(probe.animationBindingAccepted, "native animation style changes must retain the menu binding");
    Require(probe.shownBindingRetained, "the actual SHOW binding survives the system's animation style transition");
    Require(probe.otherStylesRejected, "native menu style tolerance must not ignore topmost changes");
    Require(probe.fallbackFound, "a confirmed native menu without GW_OWNER must remain a layer anchor");
    Require(probe.layerModesPassed, "native menu stays above intact, absent, demoted and separated backdrop pairs");
    Require(probe.missingAnchorPreserved, "temporary menu anchor loss cannot request panel promotion");
    probe.session.Reset();
    Require(!probe.session.LayerPlacement({}).preserveOrder, "ending the gesture restores ordinary panel layer policy");
}
