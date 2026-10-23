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

HWND FindStartWindow()
{
    HWND result = nullptr;
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        if (!IsStartWindow(window)) return TRUE;
        *reinterpret_cast<HWND*>(data) = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

void ReportFallback(DWORD error)
{
    const std::wstring text = L"[DockStartMenu] native fallback; error=" + std::to_wstring(error);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, text.c_str(), DiagnosticLogLevel::Warning);
}
}

struct NativeStartMenu::Impl
{
    std::atomic_bool active{false}, dismiss{false};
    std::atomic<HMONITOR> monitor{nullptr};
    std::atomic<HWND> menu{nullptr};
    std::jthread worker;
    std::mutex waitMutex;
    std::condition_variable_any wake;

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
            window = FindStartWindow();
            if (window && IsVisible(window))
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
                                if (!thread || !IsStartWindow(window)) return false;
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
                            bool attached = window && attach();
                            if (window && !attached) failure = ERROR_NOT_SUPPORTED;
                            else if (!stop.stop_requested())
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
                                        window = FindStartWindow();
                                        if (window)
                                        {
                                            attached = attach();
                                            if (!attached) { failure = ERROR_NOT_SUPPORTED; break; }
                                        }
                                    }
                                    menu.store(window, std::memory_order_release);
                                    if (attached && !IsWindow(window))
                                    { failure = ERROR_INVALID_WINDOW_HANDLE; break; }
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
        if (failure) ReportFallback(failure);
        if (!invoked && !stop.stop_requested()) NativeStartMenu::Toggle();
        if (request) UnmapViewOfFile(request);
        if (mapping) CloseHandle(mapping);
        if (module) FreeLibrary(module);
        menu.store(nullptr, std::memory_order_release);
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
    if (Active() || !owner || !monitor || !start_menu::Position(placement, SIZE{1, 1})) return false;
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
    const HWND window = impl_->menu.load(std::memory_order_acquire);
    if (!Active() || !window) return false;
    // Use the real hit window, not the potentially fullscreen transparent host.
    const HWND hit = GetAncestor(WindowFromPoint(screen), GA_ROOT);
    if (hit == window) return true;
    DWORD hitProcess = 0, menuProcess = 0;
    GetWindowThreadProcessId(hit, &hitProcess);
    GetWindowThreadProcessId(window, &menuProcess);
    return hitProcess && hitProcess == menuProcess;
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
