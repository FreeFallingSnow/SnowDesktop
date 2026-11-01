#include "native_start_menu.h"
#include "start_menu_window.h"
#include "start_menu_session.h"
#include "start_menu_launcher.h"
#include "settings/settings_process.h"
#include "dock/dock_settings.h"
#include "diagnostics/diagnostic_log.h"
#include "platform/runtime_module_policy.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace snowdesktop
{
namespace
{
bool IsVisible(HWND window) noexcept
{
    DWORD cloak = 0;
    return window && IsWindowVisible(window) &&
        SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloak, sizeof(cloak))) && !cloak;
}

void LogInputState(const wchar_t* phase) noexcept
{
    const HWND foreground = GetForegroundWindow();
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(foreground, &process);
    GUITHREADINFO info{sizeof(info)};
    const bool known = thread && GetGUIThreadInfo(thread, &info);
    wchar_t line[320]{};
    swprintf_s(line, L"[DockStartMenu] input state %ls foreground=%p process=%lu known=%d capture=%p menu=%p flags=%lu winDown=%d",
        phase, foreground, process, known ? 1 : 0, info.hwndCapture, info.hwndMenuOwner, info.flags,
        ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) ? 1 : 0);
    WriteDiagnosticLogEntry(line);
}

struct Apartment
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

struct LauncherClient
{
    Apartment apartment;
    settings_ipc::Channel channel;
    settings_ipc::SettingsProcess process;
    bool healthy = true;
    ~LauncherClient()
    {
        if (healthy && process.Running())
        {
            try { channel.Notify("start.exit"); } catch (...) {}
            const auto deadline = GetTickCount64() + 250;
            while (process.Running() && GetTickCount64() < deadline) Sleep(5);
        }
        process.Stop(); channel.Close();
    }
    HRESULT Start(bool& visible)
    {
        if (FAILED(apartment.result)) return apartment.result;
        try
        {
            process.Start(channel, start_menu::kHelperCommand);
            return State(visible, 1500);
        }
        catch (...) { healthy = false; return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED); }
    }
    HRESULT State(bool& visible, DWORD timeout = 500)
    {
        try
        {
            const auto state = channel.CallWithTimeout<std::pair<LONG, bool>>(timeout, "start.state");
            visible = state.second;
            return state.first;
        }
        catch (...) { healthy = false; return HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
    }
    HRESULT Dismiss()
    {
        try { return channel.CallWithTimeout<LONG>(500, "start.dismiss"); }
        catch (...) { healthy = false; return HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
    }
    HRESULT Show()
    {
        try { return channel.CallWithTimeout<LONG>(500, "start.show"); }
        catch (...) { healthy = false; return HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
    }
};
}

std::optional<int> start_menu::TryRunLauncherHelper()
{
    if (!settings_ipc::IsSettingsProcessCommand(kHelperCommand)) return {};
    Apartment apartment;
    if (FAILED(apartment.result)) return static_cast<int>(apartment.result);
    try
    {
        settings_ipc::Channel channel;
        settings_ipc::OpenInheritedSettingsChannel(channel, kHelperCommand);
        Microsoft::WRL::ComPtr<IServiceProvider> provider;
        Launcher launcher;
        HRESULT ready = E_PENDING;
        ULONGLONG touched = GetTickCount64();
        const auto initialize = [&] {
            touched = GetTickCount64();
            if (ready == E_PENDING)
            {
                ready = CoCreateInstance(kImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER,
                    IID_PPV_ARGS(&provider));
                if (SUCCEEDED(ready)) ready = launcher.Bind(provider.Get());
            }
            return ready;
        };
        channel.Bind<std::pair<LONG, bool>>("start.state", [&] {
            bool visible = false;
            HRESULT result = initialize();
            if (SUCCEEDED(result)) result = launcher.Visible(visible);
            return std::pair<LONG, bool>{result, visible};
        });
        channel.Bind<LONG>("start.dismiss", [&] {
            const HRESULT result = initialize();
            return FAILED(result) ? result : launcher.Dismiss();
        });
        channel.Bind<LONG>("start.show", [&] {
            const HRESULT result = initialize();
            if (FAILED(result)) return result;
            DWORD shellProcess = 0;
            GetWindowThreadProcessId(GetShellWindow(), &shellProcess);
            if (shellProcess) AllowSetForegroundWindow(shellProcess);
            return launcher.Show();
        });
        channel.Bind<void>("start.exit", [] { PostQuitMessage(0); });
        channel.SetDisconnected([] { PostQuitMessage(0); });
        MSG message{};
        while (GetTickCount64() - touched < 30000)
        {
            MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT) return 0;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return 0;
    }
    catch (...) { return ERROR_INVALID_DATA; }
}

struct NativeStartMenu::Impl
{
    start_menu::Invocation invocation;
    start_menu::CloseRequest close;
    start_menu::PointerDismissal pointer;
    std::atomic<CancelReason> cancelReason{CancelReason::Requested};
    std::atomic<HMONITOR> monitor{nullptr};
    std::jthread worker;
    std::mutex waitMutex;
    std::condition_variable_any wake;
    mutable std::mutex boundsMutex;
    RECT bounds{};

    void PublishHitBounds(HWND window)
    {
        RECT value{};
        if (window) GetWindowRect(window, &value);
        std::lock_guard lock(boundsMutex);
        bounds = value;
    }

    bool Pause(std::stop_token stop, int ms)
    {
        std::unique_lock lock(waitMutex);
        wake.wait_for(lock, stop, std::chrono::milliseconds(ms), [] { return false; });
        return !stop.stop_requested();
    }

    void Run(std::stop_token stop) noexcept
    {
        using namespace start_menu;
        const runtime_modules::Activity activity{runtime_modules::Module::StartMenu};
        HWND window = nullptr;
        bool invoked = false, attempted = false, protection = false, closingExisting = false;
        const ULONGLONG started = GetTickCount64();
        unsigned protectionAttempts = 0;
        DWORD failure = ERROR_SUCCESS;
        Dismissal result = Dismissal::Cancelled;
        WindowDiscovery discovery;
        try
        {
            LauncherClient launcher;
            const HRESULT connected = stop.stop_requested() ? E_ABORT : launcher.Start(closingExisting);
            if (FAILED(connected)) failure = static_cast<DWORD>(connected);
            else if (!stop.stop_requested())
            {
                // Establish the taskbar cloak before sending Win. Attachment
                // may still be finishing; only this worker waits for it.
                do
                {
                    ++protectionAttempts;
                    protection = UpdateSystemTaskbarStartMenuProtection(true, true);
                    if (protection || GetTickCount64() - started >= 750) break;
                } while (Pause(stop, 75));
                if (!protection && !stop.stop_requested()) failure = ERROR_NOT_READY;
                if (protection && !stop.stop_requested() &&
                    ShouldDispatchInitial(closingExisting, close.Requested()))
                {
                    attempted = true;
                    if (closingExisting) close.Request(GetTickCount64());
                    const HRESULT dispatched = closingExisting ? launcher.Dismiss() : launcher.Show();
                    invoked = SUCCEEDED(dispatched);
                    LogInputState(closingExisting ? L"after dismiss" : L"after show");
                    if (invoked && closingExisting) close.Dispatched(GetTickCount64());
                    if (!invoked) failure = static_cast<DWORD>(dispatched);
                    else
                    {
                        WriteDiagnosticLogEntry(closingExisting ?
                            L"[DockStartMenu] closing existing native menu with taskbar protection" :
                            L"[DockStartMenu] opened native menu with taskbar protection");
                        bool healthy = true;
                        // Shell state is authoritative; foreground-process
                        // ownership and the host HWND's cloak can lag or differ.
                        result = WaitForDismissal(closingExisting, [&] {
                            bool visible = false;
                            HRESULT state = launcher.State(visible);
                            if (FAILED(state))
                            {
                                failure = static_cast<DWORD>(state);
                                healthy = false;
                                return true; // Transport failure is not confirmed dismissal.
                            }
                            if (visible && (!window || !IsWindow(window) || !IsVisible(window)))
                                window = FindVisibleStartWindow(discovery);
                            if (!closingExisting && close.Take(visible))
                            {
                                const HRESULT dismissed = launcher.Dismiss();
                                LogInputState(L"after dismiss");
                                if (SUCCEEDED(dismissed)) close.Dispatched(GetTickCount64());
                                if (FAILED(dismissed))
                                { failure = static_cast<DWORD>(dismissed); healthy = false; }
                                WriteDiagnosticLogEntry(SUCCEEDED(dismissed) ?
                                    L"[DockStartMenu] Shell dismiss acknowledged; retaining protection until dismissal" :
                                    L"[DockStartMenu] Shell dismiss failed; no toggle fallback");
                            }
                            PublishHitBounds(visible ? window : nullptr);
                            return visible;
                        }, [] {
                            return runtime_modules::Allowed(runtime_modules::Module::StartMenu) &&
                                runtime_modules::Allowed(runtime_modules::Module::Taskbar) &&
                                UpdateSystemTaskbarStartMenuProtection(true);
                        },
                            [&](int ms) {
                                if (!healthy) return false;
                                if (close.Expired(GetTickCount64()))
                                { failure = ERROR_TIMEOUT; return false; }
                                return Pause(stop, ms);
                            }, [] { return GetTickCount64(); });
                        if (result == Dismissal::ProtectionLost) failure = ERROR_NOT_READY;
                        else if (result == Dismissal::NotFound) failure = ERROR_NOT_FOUND;
                    }
                }
            }
        }
        catch (...) { failure = ERROR_UNHANDLED_EXCEPTION; }
        try
        {
            PublishHitBounds(nullptr);
            LogInputState(L"session finished");
            if (protection) UpdateSystemTaskbarStartMenuProtection(false);
            wchar_t ended[320]{};
            swprintf_s(ended, L"[DockStartMenu] native session finished; elapsed=%llu invoked=%d cancelled=%d cancelReason=%d dismiss=%d error=%lu protectionAttempts=%u endReason=%d deferredFallback=%d",
                GetTickCount64() - started, invoked ? 1 : 0, stop.stop_requested() ? 1 : 0,
                static_cast<int>(cancelReason.load()), close.Requested() ? 1 : 0, failure,
                protectionAttempts, static_cast<int>(result), !attempted && !stop.stop_requested() ? 1 : 0);
            WriteDiagnosticLogEntry(ended);
        }
        catch (...) {} // The lease also expires if a cleanup boundary fails.
        invocation.Complete(!attempted && !stop.stop_requested(),
            !stop.stop_requested() && ((!attempted && !closingExisting) || result == Dismissal::Closed));
    }
};

NativeStartMenu::NativeStartMenu() : impl_(std::make_unique<Impl>()) {}
NativeStartMenu::~NativeStartMenu()
{
    Cancel();
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool NativeStartMenu::Open(HMONITOR monitor)
{
    if (!runtime_modules::Allowed(runtime_modules::Module::StartMenu) ||
        !runtime_modules::Allowed(runtime_modules::Module::Taskbar) ||
        Active() || !monitor || !impl_->invocation.Begin()) return false;
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->monitor.store(monitor);
    impl_->close.Reset();
    impl_->pointer.Reset();
    impl_->cancelReason.store(CancelReason::Requested);
    try
    {
        impl_->worker = std::jthread([state = impl_.get()](std::stop_token stop) { state->Run(stop); });
    }
    catch (...) { impl_->invocation.AbortBegin(); return false; }
    return true;
}
void NativeStartMenu::Cancel(bool dismiss, CancelReason reason) noexcept
{
    impl_->invocation.Cancel();
    if (!impl_->worker.get_stop_token().stop_requested())
        impl_->cancelReason.store(dismiss ? CancelReason::Dismissed : reason);
    if (dismiss) impl_->close.Request(GetTickCount64());
    else
    {
        impl_->invocation.DiscardContinuation();
        impl_->worker.request_stop();
    }
    impl_->wake.notify_all();
}
bool NativeStartMenu::DismissThen(std::function<void()> action)
{
    if (!impl_->invocation.Defer(std::move(action))) return false;
    Cancel(true);
    return true;
}
bool NativeStartMenu::Finish(const std::function<void()>& releaseHold,
    const std::function<void()>& nativeFallback)
{
    return impl_->invocation.Finish(releaseHold, nativeFallback);
}
bool NativeStartMenu::Active() const noexcept { return impl_->invocation.Pending(); }
HMONITOR NativeStartMenu::Monitor() const noexcept { return Active() ? impl_->monitor.load() : nullptr; }
bool NativeStartMenu::ContainsPoint(POINT screen) const noexcept
{
    if (!Active()) return false;
    // Retain the Dock while the native host owns interaction. Its HWND can
    // span the screen; native outside-click dismissal ends this hold shortly.
    std::lock_guard lock(impl_->boundsMutex);
    return PtInRect(&impl_->bounds, screen) != FALSE;
}
void NativeStartMenu::ObservePointer(bool down, bool pressed, bool ownedSurface) noexcept
{
    if (pressed && !ownedSurface) impl_->invocation.DiscardContinuation();
    if (Active() && impl_->pointer.Observe(down, pressed, ownedSurface))
    {
        WriteDiagnosticLogEntry(L"[DockStartMenu] later owned-surface pointer press requested dismissal");
        Cancel(true);
    }
}
bool NativeStartMenu::Toggle() noexcept
{
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_LWIN;
    input[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    input[1] = input[0];
    input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    return SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT)) == std::size(input);
}
}
