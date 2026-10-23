#include "native_start_menu.h"
#include "start_menu_window.h"
#include "start_menu_session.h"
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
}

struct NativeStartMenu::Impl
{
    start_menu::Invocation invocation;
    std::atomic_bool dismiss{false};
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
        bool invoked = false, opened = false, observed = false, protection = false;
        const ULONGLONG started = GetTickCount64();
        unsigned protectionAttempts = 0;
        DWORD failure = ERROR_SUCCESS;
        Dismissal result = Dismissal::Cancelled;
        WindowDiscovery discovery;
        try
        {
            // A pre-existing native menu belongs to its original input session.
            // This click closes it; completion never dispatches another open.
            window = FindVisibleStartWindow(discovery);
            if (window && !stop.stop_requested())
            {
                invoked = NativeStartMenu::Toggle();
                if (!invoked) failure = ERROR_ACCESS_DENIED;
            }
            else if (!stop.stop_requested())
            {
                // Establish the taskbar cloak before sending Win. Attachment
                // may still be finishing; only this worker waits for it.
                do
                {
                    ++protectionAttempts;
                    protection = UpdateSystemTaskbarStartMenuProtection(true);
                    if (protection || GetTickCount64() - started >= 750) break;
                } while (Pause(stop, 75));
                if (!protection && !stop.stop_requested()) failure = ERROR_NOT_READY;
                if (protection && !stop.stop_requested())
                {
                    invoked = NativeStartMenu::Toggle();
                    if (!invoked) failure = ERROR_ACCESS_DENIED;
                    else
                    {
                        opened = true;
                        WriteDiagnosticLogEntry(L"[DockStartMenu] opened native menu with taskbar protection");
                        // Only observe the native host's visibility. No Hook,
                        // XAML access, relocation or foreign UI message.
                        result = WaitForDismissal(false, [&] {
                            if (!window || !IsWindow(window)) window = FindVisibleStartWindow(discovery);
                            const bool visible = IsVisible(window);
                            observed |= visible;
                            PublishHitBounds(visible ? window : nullptr);
                            return visible;
                        }, [] {
                            return runtime_modules::Allowed(runtime_modules::Module::StartMenu) &&
                                runtime_modules::Allowed(runtime_modules::Module::Taskbar) &&
                                UpdateSystemTaskbarStartMenuProtection(true);
                        },
                            [&](int ms) { return Pause(stop, ms); }, [] { return GetTickCount64(); });
                        if (result == Dismissal::ProtectionLost) failure = ERROR_NOT_READY;
                        else if (result == Dismissal::NotFound) failure = ERROR_NOT_FOUND;
                    }
                }
            }
            if (dismiss.load() && opened)
            {
                // A rapid second click can precede the first visible host.
                // Keep the lease while allowing that queued open to surface,
                // then close only a visible Start host that owns foreground.
                const auto closingStarted = GetTickCount64();
                do
                {
                    if (!window || !IsWindow(window)) window = FindVisibleStartWindow(discovery);
                    if (IsVisible(window) || observed || GetTickCount64() - closingStarted >= 350) break;
                } while (Pause(std::stop_token{}, 25));
                if (IsVisible(window))
                {
                    DWORD foregroundProcess = 0, startProcess = 0;
                    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
                    GetWindowThreadProcessId(window, &startProcess);
                    // The physical down can already have dismissed Start.
                    // Never send a blind Win key that would reopen it.
                    if (foregroundProcess && foregroundProcess == startProcess)
                        NativeStartMenu::Toggle();
                }
            }
        }
        catch (...) { failure = ERROR_UNHANDLED_EXCEPTION; }
        try
        {
            PublishHitBounds(nullptr);
            if (protection) UpdateSystemTaskbarStartMenuProtection(false);
            wchar_t ended[320]{};
            swprintf_s(ended, L"[DockStartMenu] native session finished; elapsed=%llu invoked=%d cancelled=%d cancelReason=%d dismiss=%d error=%lu protectionAttempts=%u endReason=%d deferredFallback=%d",
                GetTickCount64() - started, invoked ? 1 : 0, stop.stop_requested() ? 1 : 0,
                static_cast<int>(cancelReason.load()), dismiss.load() ? 1 : 0, failure,
                protectionAttempts, static_cast<int>(result), !invoked && !stop.stop_requested() ? 1 : 0);
            WriteDiagnosticLogEntry(ended);
        }
        catch (...) {} // The lease also expires if a cleanup boundary fails.
        invocation.Complete(!invoked && !stop.stop_requested());
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
    impl_->dismiss.store(false);
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
    if (dismiss) impl_->dismiss.store(true);
    impl_->worker.request_stop();
    impl_->wake.notify_all();
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
