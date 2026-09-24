#include "taskbar_hook/taskbar_native.h"
#include "taskbar_hook/taskbar_classic_surface.h"
#include "dock_settings.h"
#include <dwmapi.h>
#include <iostream>
#include <string>

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
    DockSettings settings;
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
    check(unrelated && !native::Attach(unrelated, &shared, false),
        "the native controller must refuse non-taskbar windows");
    check(native::Attach(window, &shared, false) && cloaked(),
        "production suppression must cloak its target immediately");
    const BOOL reveal = FALSE;
    check(SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_CLOAK, &reveal, sizeof(reveal))) && cloaked(),
        "an explicit shell uncloak request cannot reveal a protected taskbar");
    if (unrelated)
    {
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

    // Preserve pre-existing app cloaking when no native uncloak is requested.
    const BOOL conceal = TRUE;
    DwmSetWindowAttribute(window, DWMWA_CLOAK, &conceal, sizeof(conceal));
    shared.suppressTaskbar = TRUE;
    check(native::Attach(window, &shared, false), "re-enter suppression on a previously cloaked window");
    shared.enabled = FALSE;
    SendMessageW(window, apply, 0, 0);
    check(cloaked(), "release must preserve an independently owned original cloak");
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
        check(native::Attach(window, &shared, false) && cloaked(), "takeover is bound to the live owner process handle");
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
    SetWindowPos(window, nullptr, 0, 0, 48, 320, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    check(SUCCEEDED(surface.Draw(window, style)), "classic surface follows a vertical taskbar resize");
    surface.Reset();
    shared.ownerProcessId = GetCurrentProcessId();
    shared.enabled = TRUE;
    shared.appearanceEnabled = TRUE;
    shared.defaultEnabled = TRUE;
    shared.suppressTaskbar = FALSE;
    shared.gradient = style.gradient;
    check(native::Attach(window, &shared, true) && shared.status == kStatusApplied,
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
    // Occupying the real lower composition target reproduces a backend failure
    // without mocking away native attachment, DWM or the restoration path.
    check(SUCCEEDED(surface.Draw(window, style)), "reserve the lower composition target for a competing owner");
    shared.enabled = TRUE;
    shared.appearanceEnabled = TRUE;
    shared.suppressTaskbar = TRUE;
    check(native::Attach(window, &shared, true) && cloaked() && shared.status == kStatusFailed,
        "appearance failure must report failure without revealing a suppressed taskbar");
    surface.Reset();
    shared.borderAlpha = .1f;
    SendMessageW(window, apply, 0, 0);
    check(cloaked() && shared.status == kStatusApplied,
        "editing appearance after releasing the competing target recovers rendering while staying hidden");
    shared.enabled = FALSE;
    SendMessageW(window, apply, 0, 0);
    check(!cloaked() && !GetPropW(window, native::kAttachedProperty),
        "release after a recovered material failure restores the taskbar");
    DestroyWindow(window);
    UnregisterClassW(registration.lpszClassName, instance);
    return failures;
}
