#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include "taskbar_hook/taskbar_connection.h"
#include "taskbar_hook/tray_connection.h"

#include <filesystem>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

// Opt-in real-Shell diagnostic. The default CTest entry only sends messages
// to this process's own invisible observer. No application data is modified.
namespace
{
void Require(bool value, const char* operation)
{
    if (!value) throw std::runtime_error(std::string(operation) +
        " error=" + std::to_string(GetLastError()));
}

class Probe
{
public:
    explicit Probe(const std::filesystem::path& output = {})
    {
        Require(taskbarCreated_ >= 0xc000, "register TaskbarCreated message");
        if (!output.empty())
        {
            log_.open(output, std::ios::out | std::ios::trunc);
            Require(log_.is_open(), "open output");
        }
        WNDCLASSW definition{};
        definition.hInstance = GetModuleHandleW(nullptr);
        definition.lpfnWndProc = Procedure;
        definition.lpszClassName = L"SnowDesktop.WallpaperShellProbe";
        Require(RegisterClassW(&definition) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
            "register observer");
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            definition.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1,
            nullptr, nullptr, definition.hInstance, this);
        Require(window_ != nullptr, "create invisible observer");
        Event("observer-start");
    }
    ~Probe()
    {
        Cleanup();
        if (window_) DestroyWindow(window_);
    }
    void Event(std::string_view event, unsigned long long value = 0)
    {
        SYSTEMTIME time{};
        GetSystemTime(&time);
        char stamp[64]{};
        sprintf_s(stamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
            time.wYear, time.wMonth, time.wDay, time.wHour,
            time.wMinute, time.wSecond, time.wMilliseconds);
        const auto write = [&](std::ostream& stream) {
            stream << stamp << ' ' << event << " value=" << value << '\n';
            stream.flush();
        };
        write(std::cout);
        if (log_) write(log_);
    }
    void Pump(DWORD milliseconds)
    {
        const auto end = GetTickCount64() + milliseconds;
        do
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (tray_)
            {
                snowdesktop::tray::Event event;
                while (snowdesktop::tray::Consume(*tray_, event)) ++trayEvents_;
            }
            if (taskbar_ && taskbar_->diagnosticStage != previousStage_)
            {
                previousStage_ = taskbar_->diagnosticStage;
                Event("taskbar-stage", static_cast<unsigned>(previousStage_));
            }
            const auto now = GetTickCount64();
            if (now >= end) break;
            const DWORD remaining = static_cast<DWORD>((std::min)(end - now, ULONGLONG{100}));
            MsgWaitForMultipleObjects(0, nullptr, FALSE, remaining, QS_ALLINPUT);
        } while (true);
    }
    void SelectShell()
    {
        const HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        Require(processes != INVALID_HANDLE_VALUE, "check application ownership");
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool occupied = false;
        if (Process32FirstW(processes, &entry))
            do { occupied = occupied || _wcsicmp(entry.szExeFile, L"SnowDesktop.exe") == 0; }
            while (Process32NextW(processes, &entry));
        else
        { CloseHandle(processes); Require(false, "enumerate application ownership"); }
        CloseHandle(processes);
        Require(!occupied, "close SnowDesktop before isolated Shell scenarios");
        shell_ = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD expected = 0;
        GetWindowThreadProcessId(GetShellWindow(), &expected);
        Require(shell_ && GetWindowThreadProcessId(shell_, &explorer_) &&
            expected && expected == explorer_, "identify Explorer taskbar");
        // Do not steal a host's connection or mix an old resident probe with
        // this scenario. The caller must start a fresh Explorer between runs.
        Require(!GetPropW(shell_, snowdesktop::taskbar_hook::kHookOwnerProperty) &&
            !GetPropW(shell_, snowdesktop::tray::kResidentProperty),
            "fresh Explorer required: resident SnowDesktop Hook detected");
        Event("explorer-pid", explorer_);
    }
    void Load(const std::filesystem::path& library)
    {
        module_ = LoadLibraryExW(library.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(module_ != nullptr, "load isolated Hook");
    }
    void LoadInExplorer()
    {
        using namespace snowdesktop::taskbar_hook;
        // Without the production shared mapping this entry immediately passes
        // through, separating DLL load from persistent attachment/TAP setup.
        HANDLE existing = OpenFileMappingW(FILE_MAP_READ, FALSE, kSharedStateName);
        if (existing) CloseHandle(existing);
        Require(!existing, "taskbar mapping already exists");
        const auto callback = reinterpret_cast<HOOKPROC>(GetProcAddress(module_, "SnowDesktopTaskbarHookProc"));
        Require(callback != nullptr, "resolve taskbar entry");
        const auto thread = GetWindowThreadProcessId(shell_, nullptr);
        HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, callback, module_, thread);
        Require(hook != nullptr, "register load-only Hook");
        DWORD_PTR result = 0;
        const bool delivered = SendMessageTimeoutW(shell_, WM_NULL, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0;
        const bool removed = UnhookWindowsHookEx(hook) != FALSE;
        Require(delivered && removed, "complete load-only callback");
        Event("load-only-complete");
    }
    void AttachTray(const std::filesystem::path& library, bool notify)
    {
        using namespace snowdesktop::tray;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(SharedState), ObjectName(GetCurrentProcessId(), L"State").c_str());
        const DWORD mappingError = GetLastError();
        Require(mapping_ && mappingError != ERROR_ALREADY_EXISTS, "create tray mapping");
        view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
        Require(view_ != nullptr, "map tray state");
        tray_ = new(view_) SharedState;
        tray_->owner = GetCurrentProcessId();
        FILETIME created{}, exited{}, kernel{}, user{};
        Require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE,
            "read owner identity");
        tray_->ownerCreation = static_cast<std::uint64_t>(created.dwHighDateTime) << 32 | created.dwLowDateTime;
        InterlockedExchange64(&tray_->epoch, static_cast<LONG64>(GetTickCount64()) + 1);
        signal_ = CreateEventW(nullptr, FALSE, FALSE, ObjectName(tray_->owner, L"Signal").c_str());
        Require(signal_ != nullptr, "create tray signal");
        const wchar_t* stage = L"start";
        const DWORD result = AttachCollector(shell_, *tray_, module_, stage, [&] { return library.wstring(); });
        Event("tray-attach-result", result);
        Require(result == ERROR_SUCCESS, "attach production collector");
        const auto deadline = GetTickCount64() + 4000;
        while (!Read(tray_->ready) && GetTickCount64() < deadline) Pump(50);
        Require(Read(tray_->ready) != 0, "collector readiness");
        Event("tray-ready");
        if (notify)
        {
            DWORD_PTR armed = 0;
            Require(SendMessageTimeoutW(shell_, RegisterWindowMessageW(kReregisterMessage),
                tray_->owner, static_cast<LPARAM>(Read(tray_->epoch)),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &armed) && armed, "arm re-registration");
            DWORD recipients = BSM_APPLICATIONS;
            const auto broadcast = BroadcastSystemMessageW(BSF_POSTMESSAGE | BSF_IGNORECURRENTTASK,
                &recipients, taskbarCreated_, 0, 0);
            Require(broadcast > 0, "broadcast TaskbarCreated");
            Event("sent-TaskbarCreated", static_cast<unsigned long long>(broadcast));
        }
    }
    void AttachTaskbar(bool appearance)
    {
        using namespace snowdesktop::taskbar_hook;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(SharedState), kSharedStateName);
        const DWORD mappingError = GetLastError();
        Require(mapping_ && mappingError != ERROR_ALREADY_EXISTS, "create taskbar mapping");
        view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
        Require(view_ != nullptr, "map taskbar state");
        taskbar_ = new(view_) SharedState;
        taskbar_->ownerProcessId = GetCurrentProcessId();
        taskbar_->enabled = TRUE;
        taskbar_->appearanceEnabled = appearance ? TRUE : FALSE;
        taskbar_->targetCount = 1;
        taskbar_->targets[0].taskbar = reinterpret_cast<std::uintptr_t>(shell_);
        // TAP discovery can be exercised without requesting a material change.
        taskbar_->targets[0].enabled = FALSE;
        taskbar_->targets[0].protectAutoHideActivation = appearance ? FALSE : TRUE;
        signal_ = CreateEventW(nullptr, FALSE, FALSE, kReadyEventName);
        Require(signal_ != nullptr, "create taskbar signal");
        const auto callback = reinterpret_cast<HOOKPROC>(GetProcAddress(module_, "SnowDesktopTaskbarHookProc"));
        Require(callback != nullptr, "resolve taskbar entry");
        const HWND windows[]{shell_};
        ConnectionStage stage{};
        const DWORD result = ConnectTaskbarThreads(shell_, windows, explorer_, GetCurrentProcessId(),
            module_, callback, nullptr, nullptr, &stage);
        Event("taskbar-connect-result", result);
        Require(result == ERROR_SUCCESS, "connect production taskbar Hook");
    }
    void WorkerWindow()
    {
        const HWND progman = FindWindowW(L"Progman", nullptr);
        Require(progman != nullptr, "find Progman");
        DWORD owner = 0;
        GetWindowThreadProcessId(progman, &owner);
        Require(owner == explorer_, "Progman must belong to Explorer");
        DWORD_PTR result = 0;
        Require(SendMessageTimeoutW(progman, 0x052C, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &result) != 0, "request WorkerW once");
        Event("requested-WorkerW");
    }
    void Cleanup() noexcept
    {
        if (tray_)
        {
            InterlockedExchange(&tray_->stop, 1);
            DWORD_PTR ignored = 0;
            const auto result = SendMessageTimeoutW(shell_, RegisterWindowMessageW(snowdesktop::tray::kDetachMessage),
                tray_->owner, static_cast<LPARAM>(snowdesktop::tray::Read(tray_->epoch)),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored);
            Event("tray-detach-delivered", result != 0);
            Pump(1000);
            Event("tray-ready-after-detach", static_cast<unsigned>(snowdesktop::tray::Read(tray_->ready)));
            Event("tray-events", trayEvents_);
            Event("tray-resync", static_cast<unsigned>(snowdesktop::tray::Read(tray_->resync)));
            tray_ = nullptr;
        }
        if (taskbar_)
        {
            using namespace snowdesktop::taskbar_hook;
            InterlockedIncrement(&taskbar_->generation);
            Event("taskbar-status", static_cast<unsigned>(taskbar_->status));
            Event("taskbar-error", static_cast<unsigned>(taskbar_->lastError));
            taskbar_->enabled = FALSE;
            taskbar_->appearanceEnabled = FALSE;
            taskbar_->targets[0].protectAutoHideActivation = FALSE;
            MemoryBarrier();
            InterlockedIncrement(&taskbar_->generation);
            DWORD_PTR result = 0;
            SendMessageTimeoutW(shell_, RegisterWindowMessageW(kApplyMessageName), 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
            const HANDLE owner = GetPropW(shell_, kHookOwnerProperty);
            const bool delivered = owner && SendMessageTimeoutW(shell_, RegisterWindowMessageW(kRetireHookMessageName),
                reinterpret_cast<WPARAM>(owner), 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
            Event("taskbar-retired", delivered && result == 1);
            taskbar_ = nullptr;
        }
        if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
        if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        if (signal_) { CloseHandle(signal_); signal_ = nullptr; }
        if (module_) { FreeLibrary(module_); module_ = nullptr; }
    }
    int SelfTest()
    {
        SendMessageW(window_, WM_SETTINGCHANGE, SPI_SETDESKWALLPAPER, 0);
        SendMessageW(window_, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
        SendMessageW(window_, WM_DISPLAYCHANGE, 0, 0);
        SendMessageW(window_, taskbarCreated_, 0, 0);
        Require(wallpaperChanges_ == 1 && displayChanges_ == 1 && taskbarChanges_ == 1,
            "real observer dispatch must classify wallpaper, display and taskbar notifications");
        Event("self-test-passed");
        return 0;
    }
    void Summary()
    {
        Event("wallpaper-notifications", wallpaperChanges_);
        Event("display-notifications", displayChanges_);
        Event("TaskbarCreated-notifications", taskbarChanges_);
    }
private:
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Probe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Probe*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self)
        {
            if (message == WM_SETTINGCHANGE)
            {
                if (wp == SPI_SETDESKWALLPAPER) ++self->wallpaperChanges_;
                self->Event("WM_SETTINGCHANGE", wp);
            }
            else if (message == WM_DISPLAYCHANGE)
            { ++self->displayChanges_; self->Event("WM_DISPLAYCHANGE", wp); }
            else if (message == self->taskbarCreated_)
            { ++self->taskbarChanges_; self->Event("received-TaskbarCreated"); }
        }
        return DefWindowProcW(window, message, wp, lp);
    }
    HWND window_ = nullptr, shell_ = nullptr;
    DWORD explorer_ = 0;
    UINT taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    HMODULE module_ = nullptr;
    HANDLE mapping_ = nullptr, signal_ = nullptr;
    void* view_ = nullptr;
    snowdesktop::tray::SharedState* tray_ = nullptr;
    snowdesktop::taskbar_hook::SharedState* taskbar_ = nullptr;
    LONG previousStage_ = 0;
    unsigned long long wallpaperChanges_ = 0, displayChanges_ = 0, taskbarChanges_ = 0, trayEvents_ = 0;
    std::ofstream log_;
};
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test")
            return Probe().SelfTest();
        if (argc != 5)
        {
            std::cerr << "usage: <observe|load|tray|tray-notify|taskbar-native|taskbar-tap|workerw> <isolated-hook.dll|-> <output.log> <10..120 seconds>\n";
            return 2;
        }
        const std::wstring_view mode(argv[1]);
        Require(mode == L"observe" || mode == L"load" || mode == L"tray" || mode == L"tray-notify" ||
            mode == L"taskbar-native" || mode == L"taskbar-tap" || mode == L"workerw", "unknown scenario");
        const std::wstring duration(argv[4]);
        std::size_t used = 0;
        const int seconds = std::stoi(duration, &used);
        Require(used == duration.size() && seconds >= 10 && seconds <= 120, "duration range");
        Probe probe(argv[3]);
        probe.SelectShell();
        probe.Event("pre-action");
        probe.Pump(1000);
        const std::filesystem::path library = std::filesystem::absolute(argv[2]);
        if (mode == L"tray" || mode == L"tray-notify") probe.AttachTray(library, mode == L"tray-notify");
        else if (mode == L"load" || mode == L"taskbar-native" || mode == L"taskbar-tap")
        {
            probe.Load(library);
            if (mode == L"load") probe.LoadInExplorer();
            else probe.AttachTaskbar(mode == L"taskbar-tap");
        }
        else if (mode == L"workerw") probe.WorkerWindow();
        probe.Event("action-complete");
        probe.Pump(static_cast<DWORD>(seconds) * 1000);
        probe.Event("cleanup-begin");
        probe.Cleanup();
        probe.Event("post-cleanup-observation");
        probe.Pump(15000);
        probe.Summary();
        std::cout << "Observation completed; notification counts do not establish visual replay or a fix.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
