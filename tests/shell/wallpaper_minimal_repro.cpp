// Standalone Win32 diagnostic: cl /std:c++20 /EHsc /W4 /MT this-file.cpp user32.lib
// No SnowDesktop headers, injected DLL, taskbar collector, or application settings.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void Check(bool value, const char* operation)
{
    if (!value) throw std::runtime_error(std::string(operation) +
        " error=" + std::to_string(GetLastError()));
}

struct Observer
{
    HWND window = nullptr;
    DWORD explorer = 0, wallpaper = 0;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    unsigned wallpaperUpdates = 0, taskbarUpdates = 0, workerRequests = 0;
    std::ofstream output;

    explicit Observer(const std::filesystem::path& path = {})
    {
        if (!path.empty()) { output.open(path); Check(output.is_open(), "open log"); }
        Check(taskbarCreated >= 0xc000, "register notification");
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = Procedure;
        type.lpszClassName = L"WallpaperEngineMinimalRepro.Observer";
        Check(RegisterClassW(&type) != 0, "register observer");
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            type.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1,
            nullptr, nullptr, type.hInstance, this);
        Check(window != nullptr, "create hidden observer");
    }
    ~Observer() { if (window) DestroyWindow(window); }

    void Log(const char* event, unsigned long long value = 0)
    {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        char stamp[48]{};
        sprintf_s(stamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
            now.wSecond, now.wMilliseconds);
        const auto write = [&](std::ostream& stream) {
            stream << stamp << ' ' << event << " value=" << value << '\n';
            stream.flush();
        };
        write(std::cout);
        if (output) write(output);
    }

    void Identify()
    {
        const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        Check(snapshot != INVALID_HANDLE_VALUE, "process snapshot");
        PROCESSENTRY32W process{sizeof(process)};
        bool snowdesktopRunning = false;
        unsigned wallpaperProcesses = 0;
        if (Process32FirstW(snapshot, &process)) do
        {
            snowdesktopRunning |= _wcsicmp(process.szExeFile, L"SnowDesktop.exe") == 0;
            if (_wcsicmp(process.szExeFile, L"wallpaper64.exe") == 0 ||
                _wcsicmp(process.szExeFile, L"wallpaper32.exe") == 0)
            {
                wallpaper = process.th32ProcessID;
                ++wallpaperProcesses;
            }
        } while (Process32NextW(snapshot, &process));
        CloseHandle(snapshot);
        Check(!snowdesktopRunning, "exit SnowDesktop before isolated experiment");
        Check(wallpaperProcesses == 1, "exactly one Wallpaper Engine process required");
        const auto shell = FindWindowW(L"Shell_TrayWnd", nullptr);
        Check(shell && GetWindowThreadProcessId(shell, &explorer) && explorer,
            "identify Explorer");
        // These properties indicate a resident collector from a previous run.
        Check(!GetPropW(shell, L"SnowDesktop.Tray.Resident.v2") &&
            !GetPropW(shell, L"SnowDesktop.Taskbar.HookOwner.v1"), "fresh Explorer required");
        Log("explorer-pid", explorer);
        Log("wallpaper-pid", wallpaper);
        CheckCleanModules(explorer);
        CheckCleanModules(wallpaper);
    }

    void CheckCleanModules(DWORD process)
    {
        HANDLE snapshot = INVALID_HANDLE_VALUE;
        for (unsigned attempt = 0; attempt != 5; ++attempt)
        {
            snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process);
            if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
            Sleep(10);
        }
        Check(snapshot != INVALID_HANDLE_VALUE, "inspect loaded modules");
        MODULEENTRY32W module{sizeof(module)};
        const bool enumerated = Module32FirstW(snapshot, &module) != FALSE;
        bool contaminated = false;
        if (enumerated) do
        {
            if (_wcsnicmp(module.szModule, L"SnowDesktop", 11) == 0)
            {
                contaminated = true;
                std::wcout << L"existing-SnowDesktop-module pid=" << process
                    << L" name=" << module.szModule << '\n';
            }
        } while (Module32NextW(snapshot, &module));
        CloseHandle(snapshot);
        Check(enumerated, "enumerate loaded modules");
        Log("existing-SnowDesktop-modules", contaminated ? process : 0);
        Check(!contaminated, "restart this process without SnowDesktop before a clean reproduction");
    }

    void WorkerW()
    {
        const auto progman = FindWindowW(L"Progman", nullptr);
        DWORD owner = 0;
        Check(progman && GetWindowThreadProcessId(progman, &owner) && owner == explorer,
            "identify Progman");
        DWORD_PTR result = 0;
        Check(SendMessageTimeoutW(progman, 0x052C, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &result) != 0,
            "one WorkerW request");
        Log("requested-WorkerW", ++workerRequests);
    }

    void Notify(std::wstring_view target, std::wstring_view windowClass)
    {
        struct Context
        {
            Observer* observer;
            std::wstring_view target, windowClass;
            unsigned posted = 0, failed = 0, wallpaperWindows = 0;
        } context{this, target, windowClass};
        Check(EnumWindows([](HWND recipient, LPARAM parameter) -> BOOL
        {
            auto& state = *reinterpret_cast<Context*>(parameter);
            auto& self = *state.observer;
            DWORD owner = 0;
            GetWindowThreadProcessId(recipient, &owner);
            if (!owner || owner == self.explorer || owner == GetCurrentProcessId() ||
                (state.target == L"wallpaper" && owner != self.wallpaper)) return TRUE;
            wchar_t name[256]{};
            GetClassNameW(recipient, name, static_cast<int>(std::size(name)));
            if (!state.windowClass.empty() && state.windowClass != name) return TRUE;
            if (owner == self.wallpaper)
            {
                ++state.wallpaperWindows;
                std::wcout << L"wallpaper-recipient class=" << name << L" hwnd=" << recipient << '\n';
            }
            if (PostMessageW(recipient, self.taskbarCreated, 0, 0)) ++state.posted;
            else ++state.failed;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context)) != FALSE, "enumerate recipients");
        Log("notification-round-posted", context.posted);
        Log("notification-round-failed", context.failed);
        Log("wallpaper-recipients", context.wallpaperWindows);
        Check(context.posted != 0, "nonempty recipient set");
        Check(target != L"wallpaper" || context.failed == 0, "Wallpaper Engine delivery");
    }

    void Pump(DWORD milliseconds, bool repeatWorker = false)
    {
        const auto end = GetTickCount64() + milliseconds;
        auto nextWorker = GetTickCount64() + 1000;
        for (;;)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
            const auto now = GetTickCount64();
            if (now >= end) break;
            if (repeatWorker && now >= nextWorker)
            { WorkerW(); nextWorker = now + 1000; }
            MsgWaitForMultipleObjects(0, nullptr, FALSE,
                static_cast<DWORD>(std::min(end - now, ULONGLONG{100})), QS_ALLINPUT);
        }
    }

    static LRESULT CALLBACK Procedure(HWND targetWindow, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Observer*>(GetWindowLongPtrW(targetWindow, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Observer*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(targetWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_SETTINGCHANGE && wp == SPI_SETDESKWALLPAPER)
        { ++self->wallpaperUpdates; self->Log("wallpaper-update", self->wallpaperUpdates); }
        if (self && message == self->taskbarCreated)
        { ++self->taskbarUpdates; self->Log("TaskbarCreated-received", self->taskbarUpdates); }
        return DefWindowProcW(targetWindow, message, wp, lp);
    }
};
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test")
        {
            Observer self;
            SendMessageW(self.window, WM_SETTINGCHANGE, SPI_SETDESKWALLPAPER, 0);
            SendMessageW(self.window, self.taskbarCreated, 0, 0);
            Check(self.wallpaperUpdates == 1 && self.taskbarUpdates == 1,
                "observer dispatch");
            self.Log("self-test-passed");
            return 0;
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--check-environment")
        {
            Observer probe;
            probe.Identify();
            probe.Log("clean-environment-confirmed");
            return 0;
        }
        if (argc != 5 && argc != 6)
        {
            std::cerr << "Usage: <observe|workerw|notify|workerw-notify|notify-workerw|repeat-workerw> "
                "<none|wallpaper|apps> <10..180 seconds> <output.log> [exact-window-class]\n"
                "Close SnowDesktop and the Wallpaper Engine browser; start a fresh Explorer before each case.\n"
                "This deliberately reproduces desktop interference. It does not modify saved settings.\n";
            return 2;
        }
        const std::wstring_view mode(argv[1]), target(argv[2]);
        Check(mode == L"observe" || mode == L"workerw" || mode == L"notify" ||
            mode == L"workerw-notify" || mode == L"notify-workerw" ||
            mode == L"repeat-workerw", "scenario");
        const bool notify = mode == L"notify" || mode == L"workerw-notify" || mode == L"notify-workerw";
        Check(notify ? target == L"wallpaper" || target == L"apps" : target == L"none", "target");
        std::size_t consumed = 0;
        const std::wstring duration(argv[3]);
        const int seconds = std::stoi(duration, &consumed);
        Check(consumed == duration.size() && seconds >= 10 && seconds <= 180, "duration");
        Observer probe(argv[4]);
        probe.Identify();
        const std::wstring_view windowClass(argc == 6 ? argv[5] : L"");
        probe.Log("actions-begin");
        if (mode == L"notify-workerw") { probe.Notify(target, windowClass); probe.Pump(1000); }
        if (mode == L"workerw" || mode == L"workerw-notify" ||
            mode == L"notify-workerw" || mode == L"repeat-workerw") probe.WorkerW();
        if (mode == L"workerw-notify") probe.Pump(1000);
        if (mode == L"notify" || mode == L"workerw-notify") probe.Notify(target, windowClass);
        probe.Log("actions-complete");
        probe.Pump(static_cast<DWORD>(seconds) * 1000, mode == L"repeat-workerw");
        probe.Log("all-actions-stopped");
        probe.Pump(20000);
        probe.Log("wallpaper-updates-total", probe.wallpaperUpdates);
        probe.Log("WorkerW-requests-total", probe.workerRequests);
        std::cout << "No DLL was loaded into Explorer. Notification counts are not visual replay counts.\n";
        return 0;
    }
    catch (const std::exception& error)
    { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
