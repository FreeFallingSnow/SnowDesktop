// Included by taskbar_native_tests.cpp; runs the actual Hook export in an
// isolated explorer.exe fixture, never the user's Explorer or desktop host.
#include "../../src/taskbar_hook/taskbar_hook_lifecycle.h"
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <optional>
#include <vector>

namespace
{
using namespace snowdesktop::taskbar_hook;
constexpr wchar_t kLifetimeChildCommand[] = L"--taskbar-hook-lifetime-child";
constexpr wchar_t kLifetimeOnlyCommand[] = L"--taskbar-hook-lifetime-only";

UINT LifetimeRectMessage()
{
    return RegisterWindowMessageW(L"Windhawk_GetTaskbarRect_taskbar-auto-hide-when-maximized");
}

LRESULT CALLBACK LifetimeWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == LifetimeRectMessage() && lParam)
    {
        *reinterpret_cast<RECT*>(lParam) = RECT{11, 22, 111, 122};
        return 0x53444f4b;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool WaitForLifetimeWorkers(HMODULE module)
{
    // Ready is signalled just before FreeLibraryAndExitThread. Wait for the
    // real worker, rather than racing that release or using a fixed sleep.
    using QueryThread = LONG (WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    const auto query = reinterpret_cast<QueryThread>(GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!query) return false;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool stopped = true;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL found = Thread32First(snapshot, &entry); found; found = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() ||
            entry.th32ThreadID == GetCurrentThreadId()) continue;
        const HANDLE thread = OpenThread(SYNCHRONIZE | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread)
        {
            // A worker already reaped by the OS is finished.
            if (GetLastError() != ERROR_INVALID_PARAMETER) stopped = false;
            continue;
        }
        void* start = nullptr;
        MEMORY_BASIC_INFORMATION memory{};
        if (query(thread, 9, &start, sizeof(start), nullptr) >= 0 &&
            VirtualQuery(start, &memory, sizeof(memory)) && memory.AllocationBase == module)
            stopped = WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0 && stopped;
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return stopped;
}

int RunLifetimeChild(const std::filesystem::path& root,
    const wchar_t* mappingName, const wchar_t* readyName)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::ofstream report(root / L"result.txt");
    const auto fail = [&](const char* message) {
        report << "FAIL: " << message << '\n'; report.flush(); return 1;
    };
    // The sentinel substitutes only the external XAML ownership boundary.
    // No real XAML TAP, native appearance or auto-hide preference is touched.
    const HMODULE sentinel = LoadLibraryW((root / L"ExplorerTAP.dll").c_str());
    if (!sentinel) return fail("load XAML ownership sentinel");
    const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(SharedState), mappingName);
    if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) return fail("create isolated IPC mapping");
    auto* shared = static_cast<SharedState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shared) return fail("map isolated IPC");
    *shared = SharedState{};
    shared->ownerProcessId = GetCurrentProcessId();
    shared->enabled = TRUE;
    shared->appearanceEnabled = TRUE;
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName);
    if (!ready || GetLastError() == ERROR_ALREADY_EXISTS) return fail("create isolated ready event");
    WNDCLASSW registration{};
    registration.lpfnWndProc = LifetimeWindowProc;
    registration.hInstance = GetModuleHandleW(nullptr);
    registration.lpszClassName = L"Shell_TrayWnd";
    if (!RegisterClassW(&registration)) return fail("register private taskbar class");
    const HWND window = CreateWindowExW(0, registration.lpszClassName, L"Hook lifetime fixture",
        WS_POPUP, -32000, -32000, 120, 48, nullptr, nullptr, registration.hInstance, nullptr);
    if (!window) return fail("create private hidden taskbar");
    shared->targetCount = 1;
    shared->targets[0].taskbar = reinterpret_cast<std::uintptr_t>(window);
    const auto attach = [&](const std::filesystem::path& path, HMODULE& resident) -> bool {
        ResetEvent(ready);
        const HMODULE library = LoadLibraryW(path.c_str());
        if (!library) return false;
        const auto hookProc = reinterpret_cast<HOOKPROC>(GetProcAddress(library, "SnowDesktopTaskbarHookProc"));
        if (!hookProc) { FreeLibrary(library); return false; }
        // Deliver the real entry's WH_CALLWNDPROC payload directly. User32 can
        // cache a module after UnhookWindowsHookEx; that incidental reference
        // masked the old DLL's defect. Only caller/worker references are owned
        // here, so their release deterministically checks the subclass lifetime.
        CWPSTRUCT message{};
        message.hwnd = window;
        message.message = WM_NULL;
        hookProc(HC_ACTION, 0, reinterpret_cast<LPARAM>(&message));
        const bool failedSafely = WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0 &&
            shared->status == kStatusFailed && shared->lastError == ERROR_ALREADY_EXISTS &&
            GetPropW(window, kHookOwnerProperty) == library;
        const bool workerStopped = WaitForLifetimeWorkers(library);
        FreeLibrary(library);
        resident = GetModuleHandleW(path.c_str());
        report << "ATTACH " << path.filename().string() << ": failure_status_and_owner=" << failedSafely
            << ", worker_stopped=" << workerStopped << ", code_resident=" << (resident == library) << '\n';
        return failedSafely && workerStopped && resident == library;
    };
    HMODULE first = nullptr;
    if (!attach(root / L"SnowDesktopTaskbarHook.dll", first))
        return fail("failed TAP must retain owner callback code after caller/worker references are released");
    RECT rect{};
    const auto query = [&] {
        rect = {};
        return SendMessageW(window, LifetimeRectMessage(),
            reinterpret_cast<WPARAM>(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)),
            reinterpret_cast<LPARAM>(&rect)) == 0x53444f4b &&
            rect.left == 11 && rect.top == 22 && rect.right == 111 && rect.bottom == 122;
    };
    if (!query()) return fail("foreign taskbar rectangle query must pass through the failed TAP owner");
    // A fresh DLL instance must retire the failed one on its window thread.
    HMODULE replacement = nullptr;
    if (!attach(root / L"ReplacementTaskbarHook.dll", replacement) || replacement == first ||
        GetPropW(window, kHookOwnerProperty) != replacement || !query())
        return fail("replacement must safely retire a failed owner and handle the same query");
    shared->enabled = FALSE;
    SendMessageW(window, RegisterWindowMessageW(kApplyMessageName), 0, 0);
    if (!query()) return fail("host disable must preserve ordinary taskbar message delivery");
    if (SendMessageW(window, RegisterWindowMessageW(kRetireHookMessageName),
            reinterpret_cast<WPARAM>(replacement), 0) != 1 ||
        GetPropW(window, kHookOwnerProperty) || !query())
        return fail("retirement must remove ownership and preserve the original window procedure");
    DestroyWindow(window);
    UnregisterClassW(registration.lpszClassName, registration.hInstance);
    if (!GetModuleHandleW((root / L"SnowDesktopTaskbarHook.dll").c_str()) ||
        !GetModuleHandleW((root / L"ReplacementTaskbarHook.dll").c_str()))
        return fail("retired callback code must remain safe for calls already in flight");
    CloseHandle(ready);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    FreeLibrary(sentinel);
    report << "PASS: failed TAP release, foreign query, replacement, disable and retirement\n";
    return 0;
}

bool ReplaceLifetimeName(std::vector<char>& image, const wchar_t* original, const std::wstring& replacement)
{
    const std::size_t bytes = (std::wcslen(original) + 1) * sizeof(wchar_t);
    if (replacement.size() != std::wcslen(original)) return false;
    const auto* begin = reinterpret_cast<const char*>(original);
    std::size_t replaced = 0;
    for (auto at = image.begin(); (at = std::search(at, image.end(), begin, begin + bytes)) != image.end(); at += bytes)
    {
        std::memcpy(&*at, replacement.c_str(), bytes);
        ++replaced;
    }
    return replaced != 0;
}

int RunTaskbarOwnerLifetimeTests()
{
    if (native::IsClassicTaskbarPlatform())
    {
        std::cout << "SKIP: XAML owner lifetime fixture requires the modern taskbar platform\n";
        return 0;
    }
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return 1;
    const std::filesystem::path exe(executable);
    wchar_t overridePath[32768]{};
    const DWORD overrideLength = GetEnvironmentVariableW(L"SNOWDESKTOP_TEST_HOOK_LIFETIME_DLL",
        overridePath, static_cast<DWORD>(std::size(overridePath)));
    auto library = overrideLength && overrideLength < std::size(overridePath)
        ? std::filesystem::path(overridePath) : exe.parent_path().parent_path() / L"SnowDesktopTaskbarHook.dll";
    // Standard output arrangement moves Hook DLLs into the runtime directory;
    // a direct test build can still leave the target at the configuration root.
    if (!overrideLength && !std::filesystem::is_regular_file(library))
        library = exe.parent_path().parent_path() / L"SnowDesktop.Runtime" / L"SnowDesktopTaskbarHook.dll";
    const auto tag = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    const auto root = std::filesystem::temp_directory_path() / (L"SnowDesktopHookLifetime_" + tag);
    std::error_code error;
    if (!std::filesystem::create_directory(root, error)) return 1;
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup{root};
    std::ifstream source(library, std::ios::binary);
    std::vector<char> image((std::istreambuf_iterator<char>(source)), {});
    const auto privateName = [&](const wchar_t* original, wchar_t kind) {
        std::wstring name = L"Local\\SDHookLifetime_" + tag + kind;
        name.resize(std::wcslen(original), L'_');
        return name;
    };
    const auto mappingName = privateName(kSharedStateName, L'S');
    const auto readyName = privateName(kReadyEventName, L'R');
    // Relocate only named IPC data in a temporary DLL copy. All production
    // callback/loader code remains unchanged; real host IPC stays untouched.
    if (image.empty() || !ReplaceLifetimeName(image, kSharedStateName, mappingName) ||
        !ReplaceLifetimeName(image, kReadyEventName, readyName)) return 1;
    const auto hookCopy = root / L"SnowDesktopTaskbarHook.dll";
    { std::ofstream file(hookCopy, std::ios::binary); file.write(image.data(), static_cast<std::streamsize>(image.size())); if (!file) return 1; }
    std::filesystem::copy_file(hookCopy, root / L"ReplacementTaskbarHook.dll", error);
    if (error) return 1;
    std::filesystem::copy_file(library, root / L"ExplorerTAP.dll", error);
    if (error) return 1;
    const auto child = root / L"explorer.exe";
    std::filesystem::copy_file(exe, child, error);
    if (error) return 1;
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return 1;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { CloseHandle(job); return 1; }
    std::wstring command = L"\"" + child.wstring() + L"\" " + kLifetimeChildCommand +
        L" \"" + root.wstring() + L"\" \"" + mappingName + L"\" \"" + readyName + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process)) { CloseHandle(job); return 1; }
    const bool assigned = AssignProcessToJobObject(job, process.hProcess) != FALSE;
    if (assigned) ResumeThread(process.hThread);
    else TerminateProcess(process.hProcess, ERROR_GEN_FAILURE); // Only this fixture's suspended child.
    const DWORD waited = WaitForSingleObject(process.hProcess, assigned ? 20000 : 5000);
    DWORD result = ERROR_TIMEOUT;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(job); // Reap only our bounded child on timeout.
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    std::ifstream report(root / L"result.txt");
    std::cout << report.rdbuf();
    if (result != 0) std::cerr << "FAILED: taskbar owner DLL lifetime, child exit=" << result << '\n';
    return result == 0 ? 0 : 1;
}
}

std::optional<int> TryRunTaskbarOwnerLifetimeHelper()
{
    int count = 0;
    auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return {};
    struct Free { wchar_t** args; ~Free() { LocalFree(args); } } free{args};
    if (count == 2 && !std::wcscmp(args[1], kLifetimeOnlyCommand)) return RunTaskbarOwnerLifetimeTests();
    if (count < 2 || std::wcscmp(args[1], kLifetimeChildCommand)) return {};
    if (count != 5) return 1;
    return RunLifetimeChild(args[2], args[3], args[4]);
}
