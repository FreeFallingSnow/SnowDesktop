#include "native_start_menu.h"
#include "start_menu_protocol.h"
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

bool IsStartWindow(HWND window) noexcept
{
    wchar_t type[96]{};
    GetClassNameW(window, type, static_cast<int>(std::size(type)));
    if (wcscmp(type, L"Windows.UI.Core.CoreWindow") != 0) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) return false;
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool queried = QueryFullProcessImageNameW(handle, 0, path, &length) != FALSE;
    CloseHandle(handle);
    if (!queried) return false;
    const wchar_t* filename = wcsrchr(path, L'\\');
    // Windows 10 1903+ and Windows 11 share this host. Earlier ShellExperienceHost
    // versions also host unrelated panels; leave those on the native path.
    return filename && _wcsicmp(filename + 1, L"StartMenuExperienceHost.exe") == 0;
}

HWND FindVisibleStartWindow()
{
    HWND result = nullptr;
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        if (!IsVisible(window) || !IsStartWindow(window)) return TRUE;
        *reinterpret_cast<HWND*>(data) = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

void ReportFallback(DWORD error, const start_menu::Request* request, HWND window)
{
    wchar_t text[768]{};
    swprintf_s(text, L"[DockStartMenu] native fallback; error=%lu window=%p status=%ld stage=%ld ticks=%ld receiverError=%ld root=%.95ls frame=%.95ls size=%ld,%ld",
        error, window, request ? request->status : -1, request ? request->stage : -1,
        request ? request->ticks : 0, request ? request->error : 0,
        request ? request->rootClass : L"", request ? request->frameClass : L"",
        request ? request->measuredSize.cx : 0, request ? request->measuredSize.cy : 0);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, text, DiagnosticLogLevel::Warning);
}
}

struct NativeStartMenu::Impl
{
    std::atomic_bool active{false}, dismiss{false}, positioningDisabled{false};
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
        DWORD failure = ERROR_SUCCESS;
        try
        {
            // A pre-existing native menu belongs to its own input session.
            // This button press only toggles it closed, without relocating it.
            window = FindVisibleStartWindow();
            if (window)
            {
                NativeStartMenu::Toggle();
                invoked = true;
            }
            else if (!stop.stop_requested())
            {
                protection = UpdateSystemTaskbarStartMenuProtection(true);
                if (!protection) failure = ERROR_NOT_READY;
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
                                if (!thread || !IsVisible(window) || !IsStartWindow(window)) return false;
                                request->target = target;
                                MemoryBarrier();
                                HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, hookProc, module, thread);
                                if (!hook) return false;
                                DWORD_PTR ignored = 0;
                                const bool delivered = SendMessageTimeoutW(window, RegisterWindowMessageW(kMessage),
                                    process, static_cast<LPARAM>(token), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                    300, &ignored) != 0;
                                UnhookWindowsHookEx(hook);
                                return delivered && request->status != Pending && request->status != Unsupported;
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
                                        window = FindVisibleStartWindow();
                                        if (window)
                                        {
                                            attached = attach();
                                            if (!attached) { failure = ERROR_NOT_SUPPORTED; break; }
                                        }
                                    }
                                    if (attached && !IsWindow(window))
                                    { failure = ERROR_INVALID_WINDOW_HANDLE; break; }
                                    if (attached)
                                    {
                                        failure = PulsePlacement(window);
                                        if (failure) break;
                                        PublishHitBounds(ReadBounds(*request));
                                    }
                                    const LONG status = request->status;
                                    if (status == Unsupported)
                                    { failure = static_cast<DWORD>(request->error); break; }
                                    if (status == Closed) break;
                                    if (status != Positioned && now - started >= 2000)
                                    { failure = ERROR_TIMEOUT; break; }
                                    // Search is a separate system surface. Unsupported transitions
                                    // restore native placement and release only this invocation's hold.
                                    if (status == Positioned && window && IsVisible(window))
                                    {
                                        DWORD foregroundProcess = 0, startProcess = 0;
                                        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
                                        GetWindowThreadProcessId(window, &startProcess);
                                        if (foregroundProcess && foregroundProcess != startProcess &&
                                            foregroundProcess != process)
                                            break;
                                    }
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
            ReportFallback(failure, request, window);
            // Do not repeatedly enter an unresponsive Shell bridge in this run.
            if (failure == ERROR_TIMEOUT) positioningDisabled.store(true);
        }
        PublishHitBounds(RECT{});
        if (request)
        {
            InterlockedExchange(&request->active, FALSE);
            if (window && IsWindow(window))
            {
                DWORD_PTR ignored = 0;
                SendMessageTimeoutW(window, RegisterWindowMessageW(start_menu::kMessage),
                    0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &ignored);
            }
        }
        if (protection) UpdateSystemTaskbarStartMenuProtection(false);
        if (dismiss.load() && window && IsVisible(window))
        {
            DWORD foregroundProcess = 0, startProcess = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
            GetWindowThreadProcessId(window, &startProcess);
            if (foregroundProcess == startProcess) NativeStartMenu::Toggle();
        }
        if (!invoked && !stop.stop_requested()) NativeStartMenu::Toggle();
        if (request) UnmapViewOfFile(request);
        if (mapping) CloseHandle(mapping);
        if (module) FreeLibrary(module);
        active.store(false, std::memory_order_release);
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
        !start_menu::Position(placement, SIZE{1, 1})) return false;
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->monitor.store(monitor);
    impl_->dismiss.store(false);
    impl_->active.store(true);
    try
    {
        impl_->worker = std::jthread([state = impl_.get(), owner, placement](std::stop_token stop) {
            state->Run(stop, owner, placement);
        });
    }
    catch (...) { impl_->active.store(false); return false; }
    return true;
}
void NativeStartMenu::Cancel(bool dismiss) noexcept
{
    if (dismiss) impl_->dismiss.store(true);
    impl_->worker.request_stop();
    impl_->wake.notify_all();
}
bool NativeStartMenu::Active() const noexcept { return impl_->active.load(std::memory_order_acquire); }
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
