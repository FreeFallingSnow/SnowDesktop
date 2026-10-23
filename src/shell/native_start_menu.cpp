#include "native_start_menu.h"
#include "start_menu_protocol.h"
#include "start_menu_window.h"
#include "start_menu_session.h"
#include "dock/dock_settings.h"
#include "platform/deployment_context.h"
#include "diagnostics/diagnostic_log.h"

#include <dwmapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
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

void ReportFallback(DWORD error, const start_menu::Request* request, HWND window,
    const start_menu::WindowDiscovery& discovery)
{
    wchar_t text[1024]{};
    swprintf_s(text, L"[DockStartMenu] native fallback; error=%lu window=%p status=%ld stage=%ld ticks=%ld receiverError=%ld root=%.95ls frame=%.95ls size=%ld,%ld candidates=%u hosts=%u hidden=%u lastHost=%p processError=%lu cloakError=%08lx cloak=%lu scanLimited=%d",
        error, window, request ? request->status : -1, request ? request->stage : -1,
        request ? request->ticks : 0, request ? request->error : 0,
        request ? request->rootClass : L"", request ? request->frameClass : L"",
        request ? request->measuredSize.cx : 0, request ? request->measuredSize.cy : 0,
        discovery.candidates, discovery.hosts, discovery.hidden, discovery.lastHost,
        discovery.processError, static_cast<unsigned long>(discovery.cloakError),
        discovery.cloak, discovery.scanLimited ? 1 : 0);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, text, DiagnosticLogLevel::Warning);
}
}

struct NativeStartMenu::Impl
{
    start_menu::Invocation invocation;
    std::atomic_bool dismiss{false}, positioningDisabled{false};
    std::atomic<CancelReason> cancelReason{CancelReason::Requested};
    std::atomic<HMONITOR> monitor{nullptr};
    std::jthread worker;
    std::mutex waitMutex;
    std::condition_variable_any wake;
    mutable std::mutex boundsMutex;
    RECT bounds{};

    void PublishHitBounds(const RECT& value)
    {
        std::lock_guard lock(boundsMutex);
        bounds = value;
    }

    bool Pause(std::stop_token stop, int ms)
    {
        std::unique_lock lock(waitMutex);
        wake.wait_for(lock, stop, std::chrono::milliseconds(ms), [] { return false; });
        return !stop.stop_requested();
    }

    void Run(std::stop_token stop, HWND owner, start_menu::Placement placement) noexcept
    {
        using namespace start_menu;
        HANDLE mapping = nullptr;
        Request* request = nullptr;
        HMODULE module = nullptr;
        HWND window = nullptr;
        bool invoked = false, protection = false;
        bool bridgeTimedOut = false, reportedPlacement = false;
        const ULONGLONG sessionStarted = GetTickCount64();
        unsigned protectionAttempts = 0;
        WindowDiscovery discovery;
        DWORD failure = ERROR_SUCCESS;
        try
        {
            // A pre-existing native menu belongs to its own input session.
            // This button press only toggles it closed, without relocating it.
            window = FindVisibleStartWindow(discovery);
            if (window && !stop.stop_requested())
            {
                NativeStartMenu::Toggle();
                invoked = true;
            }
            else if (!stop.stop_requested())
            {
                // Taskbar attachment is asynchronous. Give it a bounded chance
                // to finish without forcing Shell COM scans on the input thread.
                do
                {
                    ++protectionAttempts;
                    protection = UpdateSystemTaskbarStartMenuProtection(true);
                    if (protection || GetTickCount64() - sessionStarted >= 750) break;
                } while (Pause(stop, 75));
                if (!protection) failure = stop.stop_requested() ? ERROR_SUCCESS : ERROR_NOT_READY;
                else
                {
                    const auto path = deployment::GetTaskbarHookPath();
                    module = path.empty() ? nullptr : LoadLibraryW(path.c_str());
                    const auto hookProc = module ? reinterpret_cast<HOOKPROC>(
                        GetProcAddress(module, "SnowDesktopStartPinHookProc")) : nullptr;
                    if (!hookProc) failure = ERROR_PROC_NOT_FOUND;
                    else
                    {
                        static std::atomic<std::uint64_t> sequence{0};
                        const auto token = (GetTickCount64() << 16) ^ (++sequence);
                        const DWORD process = GetCurrentProcessId();
                        const auto name = MappingName(process, token);
                        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                            0, sizeof(Request), name.c_str());
                        if (mapping && GetLastError() != ERROR_ALREADY_EXISTS)
                            request = static_cast<Request*>(MapViewOfFile(mapping,
                                FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Request)));
                        if (!request) failure = ERROR_NOT_ENOUGH_MEMORY;
                        else
                        {
                            *request = Request{};
                            request->owner = process;
                            request->token = token;
                            request->ownerWindow = reinterpret_cast<std::uintptr_t>(owner);
                            request->placement = placement;
                            InterlockedExchange64(&request->heartbeat, static_cast<LONG64>(GetTickCount64()));
                            const auto attach = [&]() -> bool {
                                DWORD target = 0;
                                const DWORD thread = GetWindowThreadProcessId(window, &target);
                                if (!thread || !IsVisible(window) || !IsStartWindow(window))
                                { failure = ERROR_INVALID_WINDOW_HANDLE; return false; }
                                request->target = target;
                                MemoryBarrier();
                                HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, hookProc, module, thread);
                                if (!hook)
                                {
                                    failure = GetLastError();
                                    if (!failure) failure = ERROR_NOT_SUPPORTED;
                                    return false;
                                }
                                DWORD_PTR ignored = 0;
                                SetLastError(ERROR_SUCCESS);
                                const bool delivered = SendMessageTimeoutW(window, RegisterWindowMessageW(kMessage),
                                    process, static_cast<LPARAM>(token), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                    300, &ignored) != 0;
                                const DWORD sendError = delivered ? ERROR_SUCCESS : GetLastError();
                                bridgeTimedOut = !delivered && (!sendError || sendError == ERROR_TIMEOUT);
                                UnhookWindowsHookEx(hook);
                                if (!delivered) failure = sendError ? sendError : ERROR_TIMEOUT;
                                else if (request->status == Pending) failure = ERROR_NOT_SUPPORTED;
                                else if (request->status == Unsupported)
                                    failure = request->error ? static_cast<DWORD>(request->error) : ERROR_NOT_SUPPORTED;
                                return !failure;
                            };
                            // A hidden CoreWindow can belong to a cached or different
                            // Start surface. Bind only the menu shown by this press.
                            bool attached = false;
                            if (!stop.stop_requested())
                            {
                                invoked = NativeStartMenu::Toggle();
                                if (!invoked) failure = ERROR_ACCESS_DENIED;
                                const ULONGLONG started = GetTickCount64();
                                ULONGLONG renewed = started;
                                while (invoked && !failure && !stop.stop_requested())
                                {
                                    const ULONGLONG now = GetTickCount64();
                                    InterlockedExchange64(&request->heartbeat, static_cast<LONG64>(now));
                                    if (now - renewed >= 500)
                                    {
                                        if (!UpdateSystemTaskbarStartMenuProtection(true))
                                        { failure = ERROR_NOT_READY; break; }
                                        renewed = now;
                                    }
                                    if (!attached)
                                    {
                                        window = FindVisibleStartWindow(discovery);
                                        if (window)
                                        {
                                            attached = attach();
                                            if (!attached) break;
                                        }
                                    }
                                    if (attached && !IsWindow(window))
                                    { failure = ERROR_INVALID_WINDOW_HANDLE; break; }
                                    if (attached)
                                    {
                                        failure = PulsePlacement(window);
                                        if (failure)
                                        { bridgeTimedOut = failure == ERROR_TIMEOUT; break; }
                                        PublishHitBounds(ReadBounds(*request));
                                    }
                                    const LONG status = request->status;
                                    if (status == Positioned && !reportedPlacement)
                                    {
                                        const RECT placed = ReadBounds(*request);
                                        wchar_t text[512]{};
                                        swprintf_s(text, L"[DockStartMenu] positioned; window=%p root=%.95ls frame=%.95ls bounds=%ld,%ld,%ld,%ld",
                                            window, request->rootClass, request->frameClass,
                                            placed.left, placed.top, placed.right, placed.bottom);
                                        WriteDiagnosticLogEntry(text);
                                        reportedPlacement = true;
                                    }
                                    if (status == Unsupported)
                                    { failure = static_cast<DWORD>(request->error); break; }
                                    if (status == Closed) break;
                                    if (status != Positioned && now - started >= 2000)
                                    { failure = ERROR_TIMEOUT; break; }
                                    // Start can transiently yield focus to Explorer during
                                    // opening/animation. The receiver observes its actual
                                    // visibility; a foreign foreground process is not closure.
                                    if (!Pause(stop, 50)) break;
                                }
                            }
                        }
                    }
                }
            }
        }
        catch (...) { failure = ERROR_UNHANDLED_EXCEPTION; }

        // Preserve the failing phase before cleanup changes the receiver status.
        if (failure)
        {
            ReportFallback(failure, request, window, discovery);
            // A failed positioning attempt is a circuit breaker for this run.
            // Repeated clicks must use native input, never repeatedly re-enter
            // the failing Shell path. This is not a diagnosis of a hung thread.
            positioningDisabled.store(true);
        }
        PublishHitBounds(RECT{});
        if (request)
        {
            InterlockedExchange(&request->active, FALSE);
            if (window && IsWindow(window) && !bridgeTimedOut)
            {
                DWORD_PTR ignored = 0;
                SetLastError(ERROR_SUCCESS);
                if (!SendMessageTimeoutW(window, RegisterWindowMessageW(start_menu::kMessage),
                        0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &ignored))
                {
                    const DWORD cleanupError = GetLastError();
                    if (!failure) failure = cleanupError ? cleanupError : ERROR_TIMEOUT;
                    positioningDisabled.store(true);
                    PostMessageW(window, RegisterWindowMessageW(start_menu::kMessage), 0, 0);
                }
            }
            else if (window) PostMessageW(window, RegisterWindowMessageW(start_menu::kMessage), 0, 0);
        }
        if (protection) UpdateSystemTaskbarStartMenuProtection(false);
        if (dismiss.load() && window && IsVisible(window))
        {
            DWORD foregroundProcess = 0, startProcess = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
            GetWindowThreadProcessId(window, &startProcess);
            if (foregroundProcess == startProcess) NativeStartMenu::Toggle();
        }
        wchar_t ended[384]{};
        swprintf_s(ended, L"[DockStartMenu] finished; elapsed=%llu invoked=%d positioned=%d cancelled=%d cancelReason=%d dismiss=%d error=%lu receiverStatus=%ld protectionAttempts=%u deferredFallback=%d",
            GetTickCount64() - sessionStarted, invoked ? 1 : 0, reportedPlacement ? 1 : 0,
            stop.stop_requested() ? 1 : 0, static_cast<int>(cancelReason.load()), dismiss.load() ? 1 : 0, failure,
            request ? request->status : -1, protectionAttempts, !invoked && !stop.stop_requested() ? 1 : 0);
        WriteDiagnosticLogEntry(ended);
        if (request) UnmapViewOfFile(request);
        if (mapping) CloseHandle(mapping);
        if (module) FreeLibrary(module);
        invocation.Complete(!invoked && !stop.stop_requested());
    }
};

NativeStartMenu::NativeStartMenu() : impl_(std::make_unique<Impl>()) {}
NativeStartMenu::~NativeStartMenu()
{
    Cancel();
    // Join before Impl's condition variable and mutex are destroyed.
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool NativeStartMenu::Open(HWND owner, HMONITOR monitor, const start_menu::Placement& placement)
{
    if (Active() || impl_->positioningDisabled.load() || !owner || !monitor ||
        !start_menu::Position(placement, SIZE{1, 1}) || !impl_->invocation.Begin()) return false;
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->monitor.store(monitor);
    impl_->dismiss.store(false);
    impl_->cancelReason.store(CancelReason::Requested);
    try
    {
        impl_->worker = std::jthread([state = impl_.get(), owner, placement](std::stop_token stop) {
            state->Run(stop, owner, placement);
        });
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
    // Desktop input handling reads a local copy of the validated visible frame.
    // It does not interrogate an external fullscreen window during dismissal.
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
