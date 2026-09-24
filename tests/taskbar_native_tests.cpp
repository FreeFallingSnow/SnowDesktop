#include "taskbar_hook/taskbar_native.h"
#include "taskbar_hook/taskbar_classic_surface.h"
#include "taskbar_hook/taskbar_classic_appearance.h"
#include "taskbar_hook/taskbar_connection.h"
#include "dock_settings.h"
#include "taskbar_monitor.h"
#include <dwmapi.h>
#include <iostream>
#include <string>

namespace
{
UINT_PTR appBarState = 0;
bool rejectAppBarChange = false;
snowdesktop::taskbar_hook::SharedState* connectionState = nullptr;
HANDLE firstConnectionReady = nullptr;
struct MenuProbe
{
    HWND taskbar = nullptr;
    ULONGLONG started = 0;
    ULONGLONG minimumDuration = 0;
    bool observed = false;
    bool cloaked = false;
    bool revealBlocked = false;
} menuProbe;

void CALLBACK InspectAndCloseMenu(HWND, UINT, UINT_PTR, DWORD)
{
    SendMessageW(menuProbe.taskbar,
        RegisterWindowMessageW(snowdesktop::taskbar_hook::kApplyMessageName), 0, 0);
    if (GetTickCount64() - menuProbe.started < menuProbe.minimumDuration) return;
    DWORD cloak = 0;
    menuProbe.observed = SUCCEEDED(DwmGetWindowAttribute(menuProbe.taskbar,
        DWMWA_CLOAKED, &cloak, sizeof(cloak)));
    menuProbe.cloaked = (cloak & DWM_CLOAKED_APP) != 0;
    const BOOL reveal = FALSE;
    DwmSetWindowAttribute(menuProbe.taskbar, DWMWA_CLOAK, &reveal, sizeof(reveal));
    DwmGetWindowAttribute(menuProbe.taskbar, DWMWA_CLOAKED, &cloak, sizeof(cloak));
    menuProbe.revealBlocked = (cloak & DWM_CLOAKED_APP) != 0;
    EndMenu();
}

bool ProbeMenu(HWND owner, HWND taskbar, ULONGLONG minimumDuration = 0)
{
    menuProbe = {taskbar, GetTickCount64(), minimumDuration};
    const HMENU menu = CreatePopupMenu();
    if (!menu) return false;
    AppendMenuW(menu, MF_STRING, 1, L"Isolated taskbar menu regression");
    const UINT_PTR timer = SetTimer(owner, 0x53444d50, 30, InspectAndCloseMenu);
    if (timer)
    {
        // Real Win32 menu loop on private test windows. No mouse movement,
        // Explorer interaction or substitution of the cloaking controller.
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, -32000, -32000, 0, owner, nullptr);
        KillTimer(owner, timer);
    }
    DestroyMenu(menu);
    SendMessageW(taskbar, RegisterWindowMessageW(snowdesktop::taskbar_hook::kApplyMessageName), 0, 0);
    return menuProbe.observed;
}
// Replace only the system preference boundary: tests must never change the
// user's real taskbar auto-hide setting. Native ownership/timers remain real.
UINT_PTR WINAPI TestAppBarMessage(DWORD message, PAPPBARDATA data)
{
    if (message == ABM_GETSTATE) return appBarState;
    if (message == ABM_SETSTATE && !rejectAppBarChange)
        appBarState = static_cast<UINT_PTR>(data->lParam);
    return TRUE;
}

LRESULT CALLBACK TestConnectionHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0 && lParam && connectionState)
    {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (message->message == WM_NULL)
        {
            snowdesktop::taskbar_hook::native::Attach(
                message->hwnd, connectionState, false, TestAppBarMessage);
            // Only XAML discovery is substituted. The production connector,
            // thread hook, native subclass and DWM cloaking remain real.
            if (firstConnectionReady) SetEvent(firstConnectionReady);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
}

// These are isolated windows in the test process, never the real Explorer
// taskbar or SnowDesktop desktop host. DWM and the production subclass/hooks
// remain real; this tests ownership/recovery, not Win10 shell rendering parity.
int RunNativeTaskbarTests()
{
    using namespace snowdesktop::taskbar_hook;
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    // Win10 user regression: blur/acrylic switched but every solid tint was
    // clear. Exercise the production policy with independent ABGR constants;
    // DirectComposition HRESULTs alone cannot prove a native tint was sent.
    TargetAppearance tint;
    tint.red = 1; tint.green = 0.5f; tint.blue = 0.25f; tint.alpha = 0.5f;
    tint.style = 0;
    auto accent = native::MakeClassicAccentPolicy(tint);
    check(accent.state == 2 && accent.flags == 2 && accent.color == 0x804080ff,
        "transparent material receives the requested unpremultiplied ABGR tint");
    tint.style = kStyleGlassBackdrop;
    accent = native::MakeClassicAccentPolicy(tint);
    check(accent.state == 3 && accent.flags == 2 && accent.color == 0x804080ff,
        "blur keeps the selected color and opacity in the native material");
    tint.style |= kStyleAcrylicBackdrop;
    accent = native::MakeClassicAccentPolicy(tint);
    check(accent.state == 4 && accent.flags == 0 && accent.color == 0x804080ff,
        "acrylic keeps the selected color and opacity in the native material");
    accent = native::MakeClassicAccentPolicy(tint, false);
    check(accent.state == 3 && accent.flags == 2 && accent.color == 0x804080ff,
        "acrylic-to-blur fallback must retain the user's tint");
    tint.alpha = 0;
    check(native::MakeClassicAccentPolicy(tint).color == 0x014080ff &&
        native::MakeClassicAccentPolicy(tint, false).color == 0x004080ff,
        "only acrylic clamps fully transparent tint alpha to one");
    snowdesktop::PanelGradient tintGradient;
    tintGradient.enabled = true;
    tintGradient.stops = {{0, 0xff0000, .5}, {1, 0x0000ff, .5}};
    tint.gradient = EncodeGradient(tintGradient);
    check(native::MakeClassicAccentPolicy(tint).color == 0x01000000 &&
        native::MakeClassicAccentPolicy(tint, false).color == 0,
        "gradient material does not add a second solid tint below the gradient");
    accent = native::MakeClassicTaskbarPolicy(tint);
    check(accent.state == 2 && accent.flags == 2 && accent.color == 0,
        "Explorer is clear above the separate gradient material window");
    tint.gradient = {}; tint.borderAlpha = 0; tint.alpha = 0.5f;
    check(native::MakeClassicTaskbarPolicy(tint).color == 0x804080ff,
        "solid styles without extra drawing use the taskbar native tint directly");
    DockSettings settings;
    using snowdesktop::dock_settings_rules::ResolveClassicTaskbarSystemLightTheme;
    check(settings.classicTaskbarSystemTheme == -1,
        "new and legacy Win10 preferences default to automatic shell theme");
    // Exercise the same decision used after the primary taskbar's scene is
    // selected. Substitute only the registry boundary; never recolor the user's shell.
    check(ResolveClassicTaskbarSystemLightTheme(-1, true, 0) == false &&
        ResolveClassicTaskbarSystemLightTheme(-1, true, 1) == true,
        "automatic shell theme supplies light text on dark scenes and dark text on light scenes");
    check(!ResolveClassicTaskbarSystemLightTheme(-1, false, 1).has_value(),
        "automatic native appearance leaves the Windows theme unchanged");
    check(ResolveClassicTaskbarSystemLightTheme(0, true, 0) == true &&
        ResolveClassicTaskbarSystemLightTheme(1, true, 1) == false &&
        ResolveClassicTaskbarSystemLightTheme(0, false, 0) == true &&
        ResolveClassicTaskbarSystemLightTheme(1, false, 1) == false,
        "manual light or dark shell theme survives scene changes and disabled styling");
    check(settings.floatingEdgeSwipeBlockFullscreen, "new Dock preferences block edge swipes over fullscreen apps");
    settings.showWindowsButton = false;
    check(!ShowDockWindowsButton(settings), "Windows button follows base preference outside suppression");
    settings.suppressSystemTaskbar = true;
    check(ShowDockWindowsButton(settings) && !settings.showWindowsButton,
        "taskbar suppression must provide Start without overwriting the saved preference");
    settings.suppressSystemTaskbar = false;
    check(!ShowDockWindowsButton(settings), "leaving suppression restores the original Windows button preference");

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW registration{};
    registration.hInstance = instance;
    registration.lpfnWndProc = DefWindowProcW;
    registration.lpszClassName = L"Shell_TrayWnd";
    check(RegisterClassW(&registration) != 0, "test taskbar class is private to the test process");
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, registration.lpszClassName,
        L"SnowDesktop isolated taskbar test", WS_POPUP, -32000, -32000, 320, 48,
        nullptr, nullptr, instance, nullptr);
    check(window != nullptr, "create isolated taskbar window");
    if (!window) return failures;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    SharedState shared;
    shared.ownerProcessId = GetCurrentProcessId();
    shared.enabled = TRUE;
    shared.suppressTaskbar = TRUE;
    shared.targetCount = 1;
    shared.targets[0].taskbar = reinterpret_cast<std::uintptr_t>(window);
    shared.targets[0].suppressTaskbar = TRUE;
    const UINT apply = RegisterWindowMessageW(kApplyMessageName);
    const auto cloaked = [&] {
        DWORD value = 0;
        return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &value, sizeof(value))) &&
            (value & DWM_CLOAKED_APP) != 0;
    };
    check(!cloaked(), "uncontrolled window provides an independent visible baseline");
    HWND unrelated = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Unrelated test window", WS_POPUP, -32000, -32000, 32, 32,
        nullptr, nullptr, instance, nullptr);
    check(unrelated && !native::Attach(unrelated, &shared, false, TestAppBarMessage),
        "the native controller must refuse non-taskbar windows");
    check(native::Attach(window, &shared, false, TestAppBarMessage) && cloaked(),
        "production suppression must cloak its target immediately");
    check((appBarState & ABS_AUTOHIDE) && shared.autoHideRestore == FALSE,
        "suppression temporarily enables auto-hide to release the work-area reservation");
    const BOOL reveal = FALSE;
    check(SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_CLOAK, &reveal, sizeof(reveal))) && cloaked(),
        "an explicit shell uncloak request cannot reveal a protected taskbar");
    check(ProbeMenu(window, window) && !menuProbe.cloaked && !menuProbe.revealBlocked,
        "a real taskbar-owned popup releases its owner and permits Explorer's reveal during the menu loop");
    check(cloaked() && !GetPropW(window, native::kContextMenuProperty),
        "closing the taskbar popup resumes suppression and removes the scene exemption");
    HWND trayChild = CreateWindowExW(0, L"STATIC", L"Isolated tray child", WS_CHILD,
        0, 0, 16, 16, window, nullptr, instance, nullptr);
    check(trayChild != nullptr, "create a private notification-area child");
    if (trayChild)
    {
        native::ObserveMenuMessage(trayChild, WM_ENTERMENULOOP);
        check(!cloaked(), "the Explorer hook releases a child-owned menu before its window is created");
        native::ObserveMenuMessage(trayChild, WM_EXITMENULOOP);
        check(cloaked(), "closing a child-owned tray menu restores suppression");
        DestroyWindow(trayChild);
    }
    if (unrelated)
    {
        check(ProbeMenu(unrelated, window) && menuProbe.cloaked && menuProbe.revealBlocked,
            "ordinary application menus do not release taskbar suppression");
        SendMessageW(window, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window), -1);
        check(ProbeMenu(unrelated, window, 1650) && !menuProbe.cloaked && !menuProbe.revealBlocked,
            "a tray context request hands off to another menu owner beyond the opening grace period");
        check(cloaked() && !GetPropW(window, native::kContextMenuProperty),
            "closing a handed-off tray menu resumes suppression");
        HWND customPopup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"STATIC", L"Isolated custom tray popup", WS_POPUP, -32000, -32000, 80, 80,
            nullptr, nullptr, instance, nullptr);
        check(customPopup != nullptr, "create a private non-Win32 menu popup");
        if (customPopup)
        {
            ShowWindow(customPopup, SW_SHOWNOACTIVATE);
            SendMessageW(window, apply, 0, 0);
            check(cloaked(), "an ordinary custom popup without a tray origin does not release suppression");
            ShowWindow(customPopup, SW_HIDE);
            SendMessageW(window, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window), -1);
            ShowWindow(customPopup, SW_SHOWNOACTIVATE);
            SendMessageW(window, apply, 0, 0);
            const ULONGLONG menuDeadline = GetTickCount64() + 1650;
            while (GetTickCount64() < menuDeadline)
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                { TranslateMessage(&message); DispatchMessageW(&message); }
                MsgWaitForMultipleObjectsEx(0, nullptr, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            SendMessageW(window, apply, 0, 0);
            check(!cloaked(), "a custom tray popup remains exempt beyond the opening grace period");
            ShowWindow(customPopup, SW_HIDE);
            SendMessageW(window, apply, 0, 0);
            check(cloaked() && !GetPropW(window, native::kContextMenuProperty),
                "hiding a custom tray popup ends its exemption");
            DestroyWindow(customPopup);
        }
        DWORD value = 0;
        DwmSetWindowAttribute(unrelated, DWMWA_CLOAK, &reveal, sizeof(reveal));
        check(SUCCEEDED(DwmGetWindowAttribute(unrelated, DWMWA_CLOAKED, &value, sizeof(value))) &&
            !(value & DWM_CLOAKED_APP), "DWM calls for other windows remain outside the suppression gate");
        DestroyWindow(unrelated);
    }
    ShowWindow(window, SW_HIDE);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    check(cloaked(), "show and position requests preserve the DWM suppression");
    shared.suppressTaskbar = FALSE;
    SendMessageW(window, apply, 0, 0);
    check(!cloaked() && !GetPropW(window, native::kAttachedProperty),
        "turning the setting off releases the cloak and subclass");
    check(!(appBarState & ABS_AUTOHIDE) && shared.autoHideRestore == -1,
        "turning suppression off restores the original non-auto-hide preference");

    // Preserve pre-existing app cloaking when no native uncloak is requested.
    const BOOL conceal = TRUE;
    DwmSetWindowAttribute(window, DWMWA_CLOAK, &conceal, sizeof(conceal));
    appBarState = ABS_AUTOHIDE;
    shared.suppressTaskbar = TRUE;
    check(native::Attach(window, &shared, false, TestAppBarMessage), "re-enter suppression on a previously cloaked window");
    shared.enabled = FALSE;
    SendMessageW(window, apply, 0, 0);
    check(cloaked(), "release must preserve an independently owned original cloak");
    check((appBarState & ABS_AUTOHIDE) && shared.autoHideRestore == -1,
        "a pre-existing auto-hide preference survives suppression");
    appBarState = 0;
    DwmSetWindowAttribute(window, DWMWA_CLOAK, &reveal, sizeof(reveal));

    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    // Suspended private helper never enters main or touches user data.
    const bool created = CreateProcessW(executable, nullptr, nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
    check(created, "create an isolated owner process for crash recovery");
    if (created)
    {
        shared.enabled = TRUE;
        shared.ownerProcessId = child.dwProcessId;
        check(native::Attach(window, &shared, false, TestAppBarMessage) && cloaked(), "takeover is bound to the live owner process handle");
        TerminateProcess(child.hProcess, 0);
        WaitForSingleObject(child.hProcess, 2000);
        const ULONGLONG deadline = GetTickCount64() + 3000;
        while (GetPropW(window, native::kAttachedProperty) && GetTickCount64() < deadline)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
            if (GetPropW(window, native::kAttachedProperty))
                MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        check(!cloaked() && !GetPropW(window, native::kAttachedProperty),
            "owner death restores the taskbar without any host cleanup message");
        check(!(appBarState & ABS_AUTOHIDE) && shared.autoHideRestore == -1,
            "owner death also restores the original taskbar work-area policy");
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
    }

    // Exercise the actual classic DirectComposition surface creation, resize
    // and release on Win11 too. Win10 Explorer's child layering still needs a VM.
    native::ClassicSurface surface;
    TargetAppearance style;
    snowdesktop::PanelGradient gradient;
    gradient.enabled = true;
    gradient.stops = {{0, 0xff0000, .8}, {.4, 0x00ff00, .3}, {1, 0x0000ff, .7}};
    style.gradient = EncodeGradient(gradient);
    style.borderAlpha = .6f;
    check(SUCCEEDED(surface.Draw(window, style)), "classic surface accepts multistop gradient and border");
    const HWND backdrop = static_cast<HWND>(GetPropW(window, native::kClassicBackdropProperty));
    check(backdrop && backdrop != window && IsWindow(backdrop) &&
        (GetWindowLongPtrW(backdrop, GWL_EXSTYLE) &
            (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)) ==
            (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW),
        "classic color drawing owns a separate click-through, nonactivating background window");
    check(backdrop && SendMessageW(backdrop, WM_NCHITTEST, 0, 0) == HTTRANSPARENT &&
        SendMessageW(backdrop, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE,
        "background window cannot steal taskbar input or activation");
    SetWindowPos(window, nullptr, 0, 0, 48, 320, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    check(SUCCEEDED(surface.Draw(window, style)), "classic surface follows a vertical taskbar resize");
    surface.Reset();
    check(!IsWindow(backdrop) && !GetPropW(window, native::kClassicBackdropProperty),
        "releasing a classic surface destroys its background and diagnostic association");
    shared.ownerProcessId = GetCurrentProcessId();
    shared.enabled = TRUE;
    shared.appearanceEnabled = TRUE;
    shared.defaultEnabled = TRUE;
    shared.suppressTaskbar = FALSE;
    shared.gradient = style.gradient;
    shared.targets[0].enabled = TRUE;
    shared.targets[0].gradient = style.gradient;
    check(native::Attach(window, &shared, true, TestAppBarMessage) && shared.status == kStatusApplied,
        "the complete classic adapter applies a gradient through its production entry point");
    shared.suppressTaskbar = TRUE;
    SendMessageW(window, apply, 0, 0);
    check(cloaked(), "classic appearance and taskbar suppression can coexist");
    shared.appearanceEnabled = FALSE;
    SendMessageW(window, apply, 0, 0);
    check(cloaked(), "releasing classic appearance must not release an active suppression request");
    shared.enabled = FALSE;
    SendMessageW(window, apply, 0, 0);
    check(!cloaked() && !GetPropW(window, native::kAttachedProperty),
        "disabling the controller releases both classic appearance and suppression");
    // Reproduce Explorer owning the taskbar's lower composition layer. The
    // background must remain independent of that target and its child layout.
    Microsoft::WRL::ComPtr<IDCompositionDevice> competingDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget> competingTarget;
    check(SUCCEEDED(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&competingDevice))) &&
        SUCCEEDED(competingDevice->CreateTargetForHwnd(window, FALSE, &competingTarget)),
        "reserve the taskbar composition target for a competing shell owner");
    shared.enabled = TRUE;
    shared.appearanceEnabled = TRUE;
    shared.suppressTaskbar = TRUE;
    check(native::Attach(window, &shared, true, TestAppBarMessage) && cloaked() && shared.status == kStatusApplied,
        "Explorer composition ownership does not block the separate classic backdrop or suppression");
    const HWND suppressedBackdrop = static_cast<HWND>(GetPropW(window, native::kClassicBackdropProperty));
    check(suppressedBackdrop && !IsWindowVisible(suppressedBackdrop),
        "suppressed taskbars cannot leave their separate background visible");
    shared.borderAlpha = .1f;
    shared.targets[0].borderAlpha = .1f;
    SendMessageW(window, apply, 0, 0);
    check(cloaked() && shared.status == kStatusApplied,
        "editing appearance remains independent of the occupied taskbar target while staying hidden");
    shared.enabled = FALSE;
    SendMessageW(window, apply, 0, 0);
    check(!cloaked() && !GetPropW(window, native::kAttachedProperty),
        "releasing classic appearance restores the taskbar");
    check(!IsWindow(suppressedBackdrop), "controller shutdown destroys its separate background window");
    competingTarget.Reset(); competingDevice.Reset();
    // Start/Search/sidebars use one monitor; Task View uses every monitor.
    // Feed the real host policy into the private mapping, with all appearance
    // rules disabled, then drive the production subclass and DWM detour.
    registration.lpszClassName = L"Shell_SecondaryTrayWnd";
    check(RegisterClassW(&registration) != 0, "register isolated secondary taskbar class");
    const HWND secondary = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        registration.lpszClassName, L"Isolated secondary taskbar", WS_POPUP,
        -32000, -32000, 320, 48, nullptr, nullptr, instance, nullptr);
    check(secondary != nullptr, "create isolated secondary taskbar");
    if (secondary)
    {
        ShowWindow(secondary, SW_SHOWNOACTIVATE);
        const auto secondaryCloaked = [&] {
            DWORD value = 0;
            return SUCCEEDED(DwmGetWindowAttribute(secondary, DWMWA_CLOAKED, &value, sizeof(value))) &&
                (value & DWM_CLOAKED_APP) != 0;
        };
        shared.enabled = TRUE;
        shared.appearanceEnabled = FALSE;
        shared.suppressTaskbar = TRUE;
        shared.targetCount = 2;
        shared.targets[0] = {};
        shared.targets[1] = {};
        shared.targets[0].taskbar = reinterpret_cast<std::uintptr_t>(window);
        shared.targets[0].suppressTaskbar = TRUE;
        shared.targets[1].taskbar = reinterpret_cast<std::uintptr_t>(secondary);
        check(native::Attach(window, &shared, false, TestAppBarMessage),
            "attach only the display that has Dock");
        Snapshot connectionSnapshot;
        check(ReadSharedSnapshot(&shared, connectionSnapshot) &&
            AreSuppressedTaskbarsControlled(connectionSnapshot) &&
            !GetPropW(secondary, native::kAttachedProperty),
            "an untouched display without Dock cannot keep suppression status connecting");
        connectionSnapshot.targets[0].shellPanelVisible = TRUE;
        shared.targets[0].shellPanelVisible = TRUE;
        SendMessageW(window, apply, 0, 0);
        check(!cloaked() && AreSuppressedTaskbarsControlled(connectionSnapshot),
            "revealing a system panel keeps suppression connected");
        connectionSnapshot.targets[0].shellPanelVisible = FALSE;
        check(!AreSuppressedTaskbarsControlled(connectionSnapshot),
            "a requested but uncloaked target must not report active");
        connectionSnapshot.targets[0].suppressTaskbar = FALSE;
        check(!AreSuppressedTaskbarsControlled(connectionSnapshot),
            "an empty Dock target set is not a completed suppression attachment");
        shared.enabled = FALSE;
        SendMessageW(window, apply, 0, 0);

        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        check(ready && cancel, "create private connection events");
        if (ready && cancel)
        {
            const std::array<HWND, 2> windows{window, secondary};
            connectionState = &shared;
            shared.enabled = TRUE;
            shared.targets[0].shellPanelVisible = FALSE;
            shared.targets[1].suppressTaskbar = TRUE;
            firstConnectionReady = ready;
            check(ConnectTaskbarThreads(window, windows, GetCurrentProcessId(),
                nullptr, TestConnectionHook, ready, nullptr, cancel, false, 0) == ERROR_SUCCESS &&
                cloaked() && secondaryCloaked(),
                "initial XAML notification completes native attachment on both taskbars");
            shared.enabled = FALSE;
            SendMessageW(window, apply, 0, 0);
            SendMessageW(secondary, apply, 0, 0);

            // Exact reconnect condition: personalization is already connected,
            // suppression was disabled, and the one-time Ready event is reset.
            ResetEvent(ready);
            firstConnectionReady = nullptr;
            shared.enabled = TRUE;
            check(ConnectTaskbarThreads(window, windows, GetCurrentProcessId(),
                nullptr, TestConnectionHook, ready, nullptr, cancel, true, 0) == ERROR_SUCCESS &&
                WaitForSingleObject(ready, 0) == WAIT_TIMEOUT && cloaked() && secondaryCloaked(),
                "enabling suppression after personalization must not wait for a second XAML notification");
            check(ReadSharedSnapshot(&shared, connectionSnapshot) &&
                AreSuppressedTaskbarsControlled(connectionSnapshot),
                "reconnected Dock displays immediately report completed suppression");
            shared.enabled = FALSE;
            SendMessageW(window, apply, 0, 0);
            SendMessageW(secondary, apply, 0, 0);
            shared.enabled = TRUE;
            SetEvent(cancel);
            check(ConnectTaskbarThreads(window, windows, GetCurrentProcessId(),
                nullptr, TestConnectionHook, ready, nullptr, cancel, true, 0) == ERROR_CANCELLED &&
                !cloaked() && !secondaryCloaked(),
                "cancelled reconnect cannot reattach native taskbars");
            connectionState = nullptr;
            shared.enabled = FALSE;
        }
        if (ready) CloseHandle(ready);
        if (cancel) CloseHandle(cancel);
        using snowdesktop::dock_settings_rules::ShouldRevealTaskbarForShellPanel;
        for (bool classic : {false, true})
        {
            shared.enabled = TRUE;
            shared.suppressTaskbar = TRUE;
            shared.appearanceEnabled = FALSE;
            shared.targetCount = 2;
            shared.targets[0] = {};
            shared.targets[1] = {};
            shared.targets[0].taskbar = reinterpret_cast<std::uintptr_t>(window);
            shared.targets[1].taskbar = reinterpret_cast<std::uintptr_t>(secondary);
            shared.targets[0].suppressTaskbar = TRUE;
            shared.targets[1].suppressTaskbar = TRUE;
            check(native::Attach(window, &shared, classic, TestAppBarMessage) && native::Attach(secondary, &shared, classic, TestAppBarMessage) &&
                cloaked() && secondaryCloaked(), "both taskbars start hidden without custom appearance");

            shared.targets[0].suppressTaskbar = FALSE;
            SendMessageW(window, apply, 0, 0);
            SendMessageW(secondary, apply, 0, 0);
            DwmSetWindowAttribute(window, DWMWA_CLOAK, &reveal, sizeof(reveal));
            check(!cloaked() && secondaryCloaked(),
                "moving Dock to the secondary display releases the old taskbar and protects the new one");
            shared.targets[0].suppressTaskbar = TRUE;
            shared.targets[1].suppressTaskbar = FALSE;
            SendMessageW(window, apply, 0, 0);
            SendMessageW(secondary, apply, 0, 0);
            check(cloaked() && !secondaryCloaked(),
                "moving Dock back updates suppression without restarting or reinjecting");
            shared.targets[1].suppressTaskbar = TRUE;
            shared.targetCount = 1;
            SendMessageW(secondary, apply, 0, 0);
            check(!secondaryCloaked(), "a taskbar absent from the new Dock scope is not hidden by a global fallback");
            shared.targetCount = 2;
            SendMessageW(secondary, apply, 0, 0);

            // The original auto-hide preference was OFF. The real isolated
            // taskbar is outside the virtual desktop, as after auto-hide moves
            // it past an edge; its panel must still match a valid display.
            const HMONITOR panelMonitor = MonitorFromPoint(POINT{-32000, -32000}, MONITOR_DEFAULTTONEAREST);
            check(MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == nullptr,
                "panel regression fixture has no on-screen taskbar intersection");
            shared.targets[0].shellPanelVisible = ShouldRevealTaskbarForShellPanel(false, true,
                snowdesktop::taskbar_monitor::Resolve(window) == panelMonitor);
            shared.targets[1].shellPanelVisible = ShouldRevealTaskbarForShellPanel(false, true, false);
            DwmSetWindowAttribute(window, DWMWA_CLOAK, &reveal, sizeof(reveal));
            check(!cloaked(), "panel reveal is permitted before the posted apply message reaches the taskbar");
            SendMessageW(window, apply, 0, 0);
            SendMessageW(secondary, apply, 0, 0);
            check(!cloaked() && secondaryCloaked() && GetPropW(window, native::kAttachedProperty) &&
                (appBarState & ABS_AUTOHIDE), "panel suspends only its monitor's hiding without restoring work-area reservation");
            shared.targets[0].shellPanelVisible = ShouldRevealTaskbarForShellPanel(false, false, true);
            SendMessageW(window, apply, 0, 0);
            check(cloaked() && secondaryCloaked(), "panel close or an ordinary app resumes hiding without reinjection");
            for (int target = 0; target != 2; ++target)
                shared.targets[target].shellPanelVisible = ShouldRevealTaskbarForShellPanel(true, false, false);
            SendMessageW(window, apply, 0, 0);
            SendMessageW(secondary, apply, 0, 0);
            check(!cloaked() && !secondaryCloaked(), "Task View temporarily releases every taskbar");
            shared.enabled = FALSE;
            SendMessageW(window, apply, 0, 0);
            SendMessageW(secondary, apply, 0, 0);
            check(!(appBarState & ABS_AUTOHIDE) && !GetPropW(window, native::kAttachedProperty) &&
                !GetPropW(secondary, native::kAttachedProperty), "disable during a panel restores the original auto-hide setting");
        }
        DestroyWindow(secondary);
    }
    UnregisterClassW(registration.lpszClassName, instance);
    // A replacement Explorer resumes the original preference from the shared
    // mapping rather than mistaking the forced ON state for the user's choice.
    shared.targetCount = 1;
    shared.targets[0] = {};
    shared.targets[0].taskbar = reinterpret_cast<std::uintptr_t>(window);
    shared.targets[0].suppressTaskbar = TRUE;
    shared.enabled = TRUE;
    shared.suppressTaskbar = TRUE;
    shared.autoHideRestore = FALSE;
    appBarState = ABS_AUTOHIDE;
    check(native::Attach(window, &shared, false, TestAppBarMessage), "reconnect after Explorer replacement");
    shared.enabled = FALSE;
    SendMessageW(window, apply, 0, 0);
    check(!(appBarState & ABS_AUTOHIDE), "reconnection preserves the original non-auto-hide preference");
    shared.enabled = TRUE;
    rejectAppBarChange = true;
    check(native::Attach(window, &shared, false, TestAppBarMessage) && shared.autoHideStatus == kStatusFailed,
        "ABM_SETSTATE success return alone must not hide a rejected work-area change");
    rejectAppBarChange = false;
    SendMessageW(window, apply, 0, 0);
    check(shared.autoHideStatus == kStatusApplied && (appBarState & ABS_AUTOHIDE),
        "work-area override recovers when the system accepts the change");
    shared.enabled = FALSE;
    rejectAppBarChange = true;
    SendMessageW(window, apply, 0, 0);
    check(!cloaked() && GetPropW(window, native::kAttachedProperty) && shared.autoHideStatus == kStatusFailed,
        "a rejected auto-hide restore keeps a recovery watcher but releases the cloak");
    rejectAppBarChange = false;
    SendMessageW(window, apply, 0, 0);
    check(!(appBarState & ABS_AUTOHIDE) && !GetPropW(window, native::kAttachedProperty),
        "pending auto-hide restoration completes without leaving a watcher");
    DestroyWindow(window);
    registration.lpszClassName = L"Shell_TrayWnd";
    UnregisterClassW(registration.lpszClassName, instance);
    return failures;
}
