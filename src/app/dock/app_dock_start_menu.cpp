#include "app/app.h"
#include "diagnostics/diagnostic_log.h"
#include "shell/native_start_menu.h"
#include "shell/win_key_start_tap.h"
#include "dock/dock_settings.h"
#include "dock/dock_settings_rules.h"
#include "app/dock/persistent_dock_host.h"
#include "platform/runtime_module_policy.h"

#include <atomic>
#include <future>
#include <iterator>
#include <thread>

namespace snowdesktop
{
namespace
{
struct WinKeyStartTapState
{
    std::atomic<HWND> target{nullptr};
    std::atomic<bool> enabled{false};
    std::atomic<bool> pending{false};
    std::atomic<bool> running{false};
    HANDLE stop = nullptr;
    std::thread worker;
    DWORD tapCandidate = 0;
    bool failureLogged = false;
};

WinKeyStartTapState& TapState()
{
    static WinKeyStartTapState state;
    return state;
}

bool KeyDown(int key) noexcept
{
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

bool ModifierDown() noexcept
{
    return KeyDown(VK_CONTROL) || KeyDown(VK_MENU) || KeyDown(VK_SHIFT);
}

bool MaskNativeStartAndRelease(DWORD winKey) noexcept
{
    // A dummy key between Win down and up prevents the default Start toggle.
    // Queue it with the replacement release so it precedes the release.
    constexpr WORD menuMaskKey = 0xFF;
    INPUT input[3]{};
    for (auto& event : input) event.type = INPUT_KEYBOARD;
    input[0].ki.wVk = menuMaskKey;
    input[1].ki.wVk = menuMaskKey;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    input[2].ki.wVk = static_cast<WORD>(winKey);
    input[2].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    const UINT sent = SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT));
    if (sent == 1) SendInput(1, &input[1], sizeof(INPUT));
    return sent == std::size(input);
}

UINT TapMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(kWinKeyStartTapMessage);
    return message;
}

LRESULT CALLBACK WinKeyStartProc(int code, WPARAM message, LPARAM data)
{
    if (code != HC_ACTION || !data)
        return CallNextHookEx(nullptr, code, message, data);
    const auto& input = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    auto& state = TapState();
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    const bool win = input.vkCode == VK_LWIN || input.vkCode == VK_RWIN;
    const bool injected = (input.flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0;
    if (down)
    {
        if (!win || injected || ModifierDown() || KeyDown(input.vkCode == VK_LWIN ? VK_RWIN : VK_LWIN))
            state.tapCandidate = 0;
        else if (!KeyDown(static_cast<int>(input.vkCode)))
            state.tapCandidate = state.enabled.load(std::memory_order_acquire) ? input.vkCode : 0;
    }
    if (win && up && !injected && state.tapCandidate == input.vkCode)
    {
        state.tapCandidate = 0;
        const HWND target = state.target.load(std::memory_order_acquire);
        const UINT id = TapMessage();
        // Physical chords always go to Windows. If UIPI blocks the lone-tap
        // replacement, let Windows receive the original key-up as well.
        if (state.enabled.load(std::memory_order_acquire) && target && id &&
            !ModifierDown() && MaskNativeStartAndRelease(input.vkCode))
        {
            state.pending.store(true, std::memory_order_release);
            if (!PostMessageW(target, id, 0, 0) &&
                state.pending.exchange(false, std::memory_order_acq_rel))
                NativeStartMenu::Toggle();
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

void TapThread(WinKeyStartTapState* state, std::promise<DWORD>* started)
{
    HHOOK hook = nullptr;
    try
    {
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        state->tapCandidate = 0;
        hook = SetWindowsHookExW(WH_KEYBOARD_LL, WinKeyStartProc, GetModuleHandleW(nullptr), 0);
        if (!hook)
        {
            const DWORD error = GetLastError();
            started->set_value(error ? error : ERROR_DLL_INIT_FAILED);
        }
        else
        {
            state->running.store(true, std::memory_order_release);
            started->set_value(ERROR_SUCCESS);
            while (WaitForSingleObject(state->stop, 0) == WAIT_TIMEOUT &&
                MsgWaitForMultipleObjects(1, &state->stop, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
            {
                while (WaitForSingleObject(state->stop, 0) == WAIT_TIMEOUT &&
                    PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    if (message.message == WM_QUIT) break;
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (message.message == WM_QUIT) break;
            }
        }
    }
    catch (...)
    {
        try { started->set_value(ERROR_UNHANDLED_EXCEPTION); } catch (...) {}
    }
    if (hook) UnhookWindowsHookEx(hook);
    state->running.store(false, std::memory_order_release);
}

void StartTap(WinKeyStartTapState& state)
{
    if (state.running.load(std::memory_order_acquire)) return;
    if (state.worker.joinable()) state.worker.join();
    state.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state.stop) return;
    std::promise<DWORD> started;
    auto ready = started.get_future();
    try
    {
        state.worker = std::thread(TapThread, &state, &started);
    }
    catch (...)
    {
        CloseHandle(state.stop);
        state.stop = nullptr;
        return;
    }
    const DWORD error = ready.get();
    if (!error) { state.failureLogged = false; return; }
    if (state.worker.joinable()) state.worker.join();
    CloseHandle(state.stop);
    state.stop = nullptr;
    if (!state.failureLogged)
    {
        wchar_t line[160]{};
        swprintf_s(line, L"[DockStartMenu] Win key hook failed error=%lu", error);
        ::WriteDiagnosticLogEntry(line);
        state.failureLogged = true;
    }
}

void StopTap(WinKeyStartTapState& state)
{
    if (state.worker.joinable())
    {
        if (state.stop) SetEvent(state.stop);
        state.worker.join();
    }
    if (state.stop) { CloseHandle(state.stop); state.stop = nullptr; }
    state.running.store(false, std::memory_order_release);
    state.tapCandidate = 0;
}
}

void SyncWinKeyStartTap(HWND target, bool enabled)
{
    auto& state = TapState();
    state.target.store(target, std::memory_order_release);
    const bool active = enabled && target;
    state.enabled.store(active, std::memory_order_release);
    if (active) StartTap(state);
    else if (state.running.load(std::memory_order_acquire) || state.worker.joinable()) StopTap(state);
}

void StopWinKeyStartTap()
{
    auto& state = TapState();
    state.enabled.store(false, std::memory_order_release);
    StopTap(state);
}

bool ConsumeWinKeyStartTap() noexcept
{
    return TapState().pending.exchange(false, std::memory_order_acq_rel);
}
}

namespace
{
bool startMenuOpenedFromWinKey = false;
}

bool DesktopApp::IsDockStartMenuActive(HMONITOR monitor) const
{
    return dockStartMenuHeldMonitor_ && dockStartMenu_ && dockStartMenu_->Active() &&
        (!monitor || dockStartMenu_->Monitor() == monitor);
}

void DesktopApp::ToggleWindowsStartMenu(DockContainer* dock)
{
    wchar_t mode[192]{};
    swprintf_s(mode, L"[DockStartMenu] Dock click; keepHidden=%d alwaysHidden=%d autoHideMirror=%d active=%d",
        dockSettings_.startMenuKeepTaskbarHidden ? 1 : 0,
        dockSettings_.suppressSystemTaskbar ? 1 : 0,
        dockSettings_.systemTaskbarAutoHide ? 1 : 0,
        dockStartMenu_ && dockStartMenu_->Active() ? 1 : 0);
    WriteDiagnosticLogEntry(mode);
    if (dockStartMenu_ && dockStartMenu_->Active())
    {
        // The physical down may already have dismissed the native window.
        // A close request uses one-way Shell dismissal, never another Win key.
        dockStartMenu_->DismissThen({});
        UpdateDockStartMenu();
        return;
    }
    const auto native = [this] {
        CloseFloatingDockThen([] { snowdesktop::NativeStartMenu::Toggle(); },
            FloatingDockCloseFocusPolicy::PreserveCurrent);
    };
    if (!dock || !generalSettings_.dockEnabled || !dockSettings_.startMenuKeepTaskbarHidden ||
        (!dockSettings_.suppressSystemTaskbar && !IsSystemTaskbarAutoHideEnabled()))
    { native(); return; }
    auto* host = FindPersistentDockHost(dock);
    if (!host || !host->active || !host->monitor) { native(); return; }
    // The periodic taskbar controller owns discovery/attachment. Never force
    // synchronous Shell visibility COM calls from the Start button handler.
    if (!dockStartMenu_) dockStartMenu_ = std::make_unique<snowdesktop::NativeStartMenu>();
    if (!dockStartMenu_->Open(host->monitor))
    { native(); return; }
    startMenuOpenedFromWinKey = false;
    dockStartMenuHeldMonitor_ = host->monitor;
    dockStartMenuWasPromoted_ = host->promoted;
    host->promoted = true;
    host->mergedCloseAfterInteraction = false;
    HideDockWindowPreview();
    EndFloatingDockKeyboardSession(FloatingDockCloseFocusPolicy::PreserveCurrent);
    RefreshFloatingDockVisibilityState();
    if (statusBar_) statusBar_->RefreshDockState(host->monitor);
    UpdatePersistentDockHostVisibility(*host);
}

void DesktopApp::UpdateDockStartMenu()
{
    const auto winKeyProtected = [this] {
        namespace modules = snowdesktop::runtime_modules;
        return modules::Allowed(modules::Module::StartMenu) &&
            modules::Allowed(modules::Module::Taskbar) &&
            snowdesktop::dock_settings_rules::ProtectWinKeyStart(
                generalSettings_.dockEnabled,
                ShowDockWindowsButton(dockSettings_),
                dockSettings_.startMenuKeepTaskbarHidden,
                dockSettings_.startMenuKeepTaskbarHiddenOnWinKey,
                dockSettings_.suppressSystemTaskbar || IsSystemTaskbarAutoHideEnabled(),
                desktopPassthroughActive_);
    };
    if (snowdesktop::ConsumeWinKeyStartTap())
    {
        if (dockStartMenu_ && dockStartMenu_->Active())
        {
            dockStartMenu_->DismissThen({});
        }
        else if (winKeyProtected())
        {
            POINT cursor{};
            if (!GetCursorPos(&cursor)) cursor = {};
            const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
            if (!dockStartMenu_) dockStartMenu_ = std::make_unique<snowdesktop::NativeStartMenu>();
            if (monitor && dockStartMenu_->Open(monitor))
            {
                startMenuOpenedFromWinKey = true;
                dockStartMenuHeldMonitor_ = monitor;
                dockStartMenuWasPromoted_ = false;
                WriteDiagnosticLogEntry(L"[DockStartMenu] Win key opened Start with taskbar protection");
            }
            else
            {
                WriteDiagnosticLogEntry(L"[DockStartMenu] Win key protected open failed; using native toggle");
                snowdesktop::NativeStartMenu::Toggle();
            }
        }
        else snowdesktop::NativeStartMenu::Toggle();
    }
    if (!dockStartMenu_ || !dockStartMenuHeldMonitor_) return;
    const HMONITOR monitor = dockStartMenuHeldMonitor_;
    if (startMenuOpenedFromWinKey)
    {
        MONITORINFO info{sizeof(info)};
        if (!winKeyProtected() || !GetMonitorInfoW(monitor, &info))
            dockStartMenu_->Cancel(false, snowdesktop::NativeStartMenu::CancelReason::ConfigurationChanged);
        dockStartMenu_->Finish([&] {
            dockStartMenuHeldMonitor_ = nullptr;
            startMenuOpenedFromWinKey = false;
        }, [this] {
            startMenuOpenedFromWinKey = false;
            CloseFloatingDockThen([] { snowdesktop::NativeStartMenu::Toggle(); },
                FloatingDockCloseFocusPolicy::PreserveCurrent);
        });
        return;
    }
    auto found = std::find_if(persistentDockHosts_.begin(), persistentDockHosts_.end(),
        [monitor](const auto& host) { return host && host->active && host->monitor == monitor; });
    MONITORINFO info{sizeof(info)};
    if (!generalSettings_.dockEnabled || !dockSettings_.startMenuKeepTaskbarHidden ||
        !dockSettings_.showWindowsButton || desktopPassthroughActive_ ||
        !GetMonitorInfoW(monitor, &info) ||
        found == persistentDockHosts_.end() ||
        (!dockSettings_.suppressSystemTaskbar && !IsSystemTaskbarAutoHideEnabled()))
        dockStartMenu_->Cancel(false, snowdesktop::NativeStartMenu::CancelReason::ConfigurationChanged);
    dockStartMenu_->Finish([&] {
        dockStartMenuHeldMonitor_ = nullptr;
        startMenuOpenedFromWinKey = false;
        if (found != persistentDockHosts_.end())
        {
            auto& host = **found;
            // An outside click may already have explicitly closed the Dock.
            if (!dockStartMenuWasPromoted_ && host.promoted)
                CloseFloatingDock(host, FloatingDockCloseFocusPolicy::PreserveCurrent);
            if (statusBar_) statusBar_->RefreshDockState(monitor);
            UpdatePersistentDockHostVisibility(host);
        }
    }, [this] {
        startMenuOpenedFromWinKey = false;
        CloseFloatingDockThen([] { snowdesktop::NativeStartMenu::Toggle(); },
            FloatingDockCloseFocusPolicy::PreserveCurrent);
    });
}
