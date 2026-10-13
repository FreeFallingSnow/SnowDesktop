// Included by taskbar_native_tests.cpp; runs the actual Hook export in an
// isolated explorer.exe fixture, never the user's Explorer or desktop host.
#include "../../src/taskbar_hook/taskbar_hook_lifecycle.h"
#include "../../src/taskbar_hook/taskbar_connection.h"
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
    // End the synthetic activation at our private window. DefWindowProc would
    // change focus and reenter activation, involving unrelated desktop windows.
    if (message == WM_ACTIVATE) return 0;
    if (message == LifetimeRectMessage() && lParam)
    {
        *reinterpret_cast<RECT*>(lParam) = RECT{11, 22, 111, 122};
        return 0x53444f4b;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool WaitForLifetimeWorkers(HMODULE module)
{
    // Ready precedes FreeLibraryAndExitThread; the independent owner monitor
    // also stays alive until its owner exits. Call only after ending the private
    // owner process, then wait for actual worker release instead of sleeping.
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
    struct OwnerProcess
    {
        PROCESS_INFORMATION value{};
        bool Stop() const
        {
            if (!value.hProcess) return true;
            if (WaitForSingleObject(value.hProcess, 0) == WAIT_OBJECT_0) return true;
            // Only our never-resumed fixture child, not a desktop application.
            return TerminateProcess(value.hProcess, 0) &&
                WaitForSingleObject(value.hProcess, 5000) == WAIT_OBJECT_0;
        }
        ~OwnerProcess()
        {
            Stop();
            if (value.hThread) CloseHandle(value.hThread);
            if (value.hProcess) CloseHandle(value.hProcess);
        }
    } owner;
    STARTUPINFOW startup{sizeof(startup)};
    const auto ownerExecutable = root / L"explorer.exe";
    if (!CreateProcessW(ownerExecutable.c_str(), nullptr, nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &owner.value))
        return fail("create private suspended owner process");
    const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(SharedState), mappingName);
    if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) return fail("create isolated IPC mapping");
    auto* shared = static_cast<SharedState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shared) return fail("map isolated IPC");
    *shared = SharedState{};
    shared->ownerProcessId = owner.value.dwProcessId;
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
    registration.lpszClassName = L"Shell_SecondaryTrayWnd";
    if (!RegisterClassW(&registration)) return fail("register private secondary taskbar class");
    const HWND secondary = CreateWindowExW(0, registration.lpszClassName, L"Secondary hook lifetime fixture",
        WS_POPUP, -32000, -32000, 120, 48, nullptr, nullptr, registration.hInstance, nullptr);
    if (!secondary) return fail("create private hidden secondary taskbar");
    const std::array<HWND, 2> windows{window, secondary};
    shared->targetCount = 2;
    shared->targets[0].taskbar = reinterpret_cast<std::uintptr_t>(window);
    shared->targets[1].taskbar = reinterpret_cast<std::uintptr_t>(secondary);
    for (auto& target : shared->targets) target.protectAutoHideActivation = TRUE;
    const auto observeActivation = [&](bool expected) {
        std::array<AutoHideTraceRecord, kAutoHideTraceCapacity> records;
        LONG dropped = 0;
        DrainAutoHideTrace(shared->autoHideTrace, records, dropped);
        for (const HWND target : windows) SendMessageW(target, WM_ACTIVATE, WA_ACTIVE, 0);
        const LONG count = DrainAutoHideTrace(shared->autoHideTrace, records, dropped);
        if (dropped || count != (expected ? 4 : 0)) return false;
        for (std::size_t i = 0; expected && i < windows.size(); ++i)
        {
            const auto& before = records[i * 2];
            const auto& after = records[i * 2 + 1];
            if (before.kind != AutoHideTraceKind::ActivateBefore || after.kind != AutoHideTraceKind::ActivateAfter ||
                before.taskbar != reinterpret_cast<std::uintptr_t>(windows[i]) || after.taskbar != before.taskbar ||
                before.activation != WA_ACTIVE || after.activation != WA_ACTIVE) return false;
        }
        return true;
    };
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
        message.message = WM_NULL;
        for (const HWND target : windows)
        {
            message.hwnd = target;
            hookProc(HC_ACTION, 0, reinterpret_cast<LPARAM>(&message));
        }
        const bool failedSafely = WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0 &&
            shared->status == kStatusFailed && shared->lastError == ERROR_ALREADY_EXISTS &&
            GetPropW(window, kHookOwnerProperty) == library;
        FreeLibrary(library);
        resident = GetModuleHandleW(path.c_str());
        report << "ATTACH " << path.filename().string() << ": failure_status_and_owner=" << failedSafely
            << ", code_resident=" << (resident == library) << ", status=" << shared->status
            << ", error=" << shared->lastError << ", stage=" << shared->diagnosticStage << '\n';
        return failedSafely && resident == library;
    };
    HMODULE first = nullptr;
    if (!attach(root / L"SnowDesktopTaskbarHook.dll", first))
        return fail("failed TAP must retain owner callback code after the caller reference is released");
    if (!observeActivation(true))
        return fail("failed XAML must not prevent native activation dispatch on either taskbar");
    if (!AreTaskbarThreadsConnected(windows, GetCurrentProcessId(), owner.value.dwProcessId))
        return fail("failed XAML must still acknowledge native connection for this host on both taskbars");
    // A late symbol helper result must reach the observer through the owner
    // callback even though no VisualTreeWatcher was ever constructed. The
    // fixture deliberately fails ABI compatibility rather than hook real Shell.
    shared->autoHideResolutionError = ERROR_NOT_SUPPORTED;
    for (const HWND target : windows)
        SendMessageW(target, RegisterWindowMessageW(kApplyMessageName), 0, 0);
    std::array<AutoHideTraceRecord, kAutoHideTraceCapacity> symbols;
    LONG dropped = 0;
    const LONG symbolCount = DrainAutoHideTrace(shared->autoHideTrace, symbols, dropped);
    if (std::none_of(symbols.begin(), symbols.begin() + symbolCount, [](const auto& record) {
            return record.kind == AutoHideTraceKind::Adapter && record.request == ERROR_NOT_SUPPORTED;
        }) || shared->status != kStatusFailed || shared->lastError != ERROR_ALREADY_EXISTS)
        return fail("late adapter Apply must be consumed without clearing the separate XAML failure");
    RECT rect{};
    const auto query = [&] {
        rect = {};
        return SendMessageW(window, LifetimeRectMessage(),
            reinterpret_cast<WPARAM>(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)),
            reinterpret_cast<LPARAM>(&rect)) == 0x53444f4b &&
            rect.left == 11 && rect.top == 22 && rect.right == 111 && rect.bottom == 122;
    };
    if (!query()) return fail("foreign taskbar rectangle query must pass through the failed TAP owner");
    // Finish the first worker before exercising the same pinned DLL. Replaying
    // its result must restore host-reset IPC without spawning a cooldown retry.
    if (!owner.Stop() || !WaitForLifetimeWorkers(first))
        return fail("finish first owner and failed TAP workers before reuse");
    shared->status = kStatusInjecting;
    shared->lastError = ERROR_SUCCESS;
    shared->diagnosticStage = 4242;
    HMODULE reused = nullptr;
    if (!attach(root / L"SnowDesktopTaskbarHook.dll", reused) || reused != first ||
        shared->diagnosticStage != 4242)
        return fail("same-owner cooldown must replay the failure without another TAP worker");
    OwnerProcess restartedOwner;
    if (!CreateProcessW(ownerExecutable.c_str(), nullptr, nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &restartedOwner.value))
        return fail("create restarted private host");
    shared->ownerProcessId = restartedOwner.value.dwProcessId;
    shared->status = kStatusInjecting;
    shared->lastError = ERROR_SUCCESS;
    shared->diagnosticStage = 4243;
    ResetEvent(ready);
    if (ResumeTaskbarThreads(windows, GetCurrentProcessId(), restartedOwner.value.dwProcessId, nullptr) != ERROR_SUCCESS ||
        WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0 || shared->diagnosticStage != 90 ||
        shared->status != kStatusFailed || shared->lastError != ERROR_ALREADY_EXISTS || !WaitForLifetimeWorkers(first))
        return fail("resident reconnect for a new owner must retry failed TAP without a new Hook registration");
    report << "PASS: resident resume restarts failed appearance initialization in the existing DLL\n";
    shared->status = kStatusInjecting;
    shared->lastError = ERROR_SUCCESS;
    if (!attach(root / L"SnowDesktopTaskbarHook.dll", reused) || reused != first ||
        shared->diagnosticStage != 90 || !observeActivation(true) ||
        !AreTaskbarThreadsConnected(windows, GetCurrentProcessId(), restartedOwner.value.dwProcessId))
        return fail("restarted host must run a new TAP attempt in the same pinned DLL and rebind both bars");
    // A fresh DLL instance must retire the failed one on its window thread.
    HMODULE replacement = nullptr;
    if (!attach(root / L"ReplacementTaskbarHook.dll", replacement) || replacement == first ||
        GetPropW(window, kHookOwnerProperty) != replacement || !query() || !observeActivation(true))
        return fail("replacement must safely retire a failed owner and handle the same query");
    // Both old and replacement properties describe this host, so checking the
    // PID alone would incorrectly accept a retired candidate. Exercise the
    // production connector/fallback against actual DLLs in these private bars.
    std::wstring selectedPath = (root / L"SnowDesktopTaskbarHook.dll").wstring();
    unsigned fallbackCount = 0;
    const auto isolatedPath = (root / L"Isolated" / L"SnowDesktopTaskbarHook.dll").wstring();
    const auto resolve = [&] { ++fallbackCount; return isolatedPath; };
    ResetEvent(ready);
    if (ConnectTaskbarRuntime(window, windows, GetCurrentProcessId(), restartedOwner.value.dwProcessId,
            selectedPath, nullptr, nullptr, resolve) != ERROR_SUCCESS ||
        selectedPath != isolatedPath || fallbackCount != 1 ||
        WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0)
        return fail("retired shared candidate must select and attach exactly one isolated runtime");
    const HMODULE recovery = GetModuleHandleW(isolatedPath.c_str());
    if (!recovery || recovery == first || recovery == replacement ||
        GetPropW(window, kHookOwnerProperty) != recovery ||
        GetPropW(secondary, kHookOwnerProperty) != recovery || !query() || !observeActivation(true))
        return fail("isolated recovery must own both bars without reviving either retired callback");
    if (ConnectTaskbarRuntime(window, windows, GetCurrentProcessId(), restartedOwner.value.dwProcessId,
            selectedPath, nullptr, nullptr, resolve) != ERROR_SUCCESS || fallbackCount != 1)
        return fail("subsequent connections must reuse the selected isolated instance");
    // Exercise the real resident subclass, including fresh nonce/host checks.
    // No WH_CALLWNDPROC registration, new runtime path or TAP installation is
    // involved in these 32 subsequent connections.
    for (unsigned connection = 0; connection < 32; ++connection)
    {
        for (HWND target : windows) RemovePropW(target, kHookHostProcessProperty);
        const DWORD resumed = ResumeTaskbarThreads(windows, GetCurrentProcessId(),
            restartedOwner.value.dwProcessId, nullptr);
        // The diagnostic observer is limited to 64 records/thread/second.
        // Verify activation around the burst, not four trace records per
        // reconnect; otherwise this test measures rate limiting, not liveness.
        const bool activation = connection != 0 || observeActivation(true);
        if (resumed != ERROR_SUCCESS || fallbackCount != 1 ||
            GetPropW(window, kHookOwnerProperty) != recovery ||
            GetPropW(secondary, kHookOwnerProperty) != recovery || !activation)
        {
            report << "RESUME cycle=" << connection << " error=" << resumed << " activation=" << activation
                << " primaryHost=" << reinterpret_cast<std::uintptr_t>(GetPropW(window, kHookHostProcessProperty))
                << " secondaryHost=" << reinterpret_cast<std::uintptr_t>(GetPropW(secondary, kHookHostProcessProperty))
                << " expectedHost=" << restartedOwner.value.dwProcessId
                << " primaryProtocol=" << reinterpret_cast<std::uintptr_t>(GetPropW(window, kHookResumeProtocolProperty))
                << " secondaryProtocol=" << reinterpret_cast<std::uintptr_t>(GetPropW(secondary, kHookResumeProtocolProperty)) << '\n';
            return fail("resident reconnect must reuse the same active DLL and freshly acknowledge every taskbar");
        }
    }
    if (!observeActivation(true)) return fail("activation observation must remain live after the reconnect burst");
    SetPropW(window, kHookResumeProtocolProperty,
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(kSharedStateVersion + 1)));
    if (ResumeTaskbarThreads(windows, GetCurrentProcessId(), restartedOwner.value.dwProcessId,
            nullptr) != ERROR_NOT_SUPPORTED || GetPropW(window, kHookOwnerProperty) != recovery)
        return fail("incompatible resident protocol must not be adopted or change the active owner");
    SetPropW(window, kHookResumeProtocolProperty,
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(kSharedStateVersion)));
    if (ResumeTaskbarThreads(windows, GetCurrentProcessId(), owner.value.dwProcessId,
            nullptr) != ERROR_DLL_INIT_FAILED)
        return fail("resident reconnect must reject a caller that does not own the live shared state");
    const HANDLE cancelled = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!cancelled) return fail("create cancelled recovery request");
    std::wstring cancelledPath = (root / L"SnowDesktopTaskbarHook.dll").wstring();
    const DWORD cancelledResult = ConnectTaskbarRuntime(window, windows, GetCurrentProcessId(),
        restartedOwner.value.dwProcessId, cancelledPath, nullptr, cancelled, resolve);
    const DWORD cancelledResume = ResumeTaskbarThreads(windows, GetCurrentProcessId(),
        restartedOwner.value.dwProcessId, cancelled);
    CloseHandle(cancelled);
    if (cancelledResult != ERROR_CANCELLED || cancelledResume != ERROR_CANCELLED || fallbackCount != 1 ||
        GetPropW(window, kHookOwnerProperty) != recovery)
        return fail("cancellation must not mint another runtime or change the active owner");
    shared->enabled = FALSE;
    for (const HWND target : windows)
        SendMessageW(target, RegisterWindowMessageW(kApplyMessageName), 0, 0);
    if (!query() || !observeActivation(false)) return fail("host disable must stop observation and preserve ordinary messages");
    shared->enabled = TRUE;
    for (const HWND target : windows)
        SendMessageW(target, RegisterWindowMessageW(kApplyMessageName), 0, 0);
    if (!observeActivation(true)) return fail("re-enable must restore observation without XAML reinjection");
    if (!restartedOwner.Stop() || !WaitForLifetimeWorkers(first) ||
        !WaitForLifetimeWorkers(replacement) || !WaitForLifetimeWorkers(recovery))
        return fail("private host exit must finish owner and TAP workers for all DLL instances");
    if (!GetModuleHandleW((root / L"SnowDesktopTaskbarHook.dll").c_str()) ||
        !GetModuleHandleW((root / L"ReplacementTaskbarHook.dll").c_str()) ||
        !query() || !observeActivation(false))
        return fail("after caller/worker release, host exit must stop observation and retain callback code");
    for (const HWND target : windows)
        if (SendMessageW(target, RegisterWindowMessageW(kRetireHookMessageName),
                reinterpret_cast<WPARAM>(recovery), 0) != 1 ||
            GetPropW(target, kHookOwnerProperty) || GetPropW(target, kHookHostProcessProperty) ||
            GetPropW(target, kHookConnectionProperty))
            return fail("retirement must remove native owner acknowledgement from every taskbar");
    if (ResumeTaskbarThreads(windows, GetCurrentProcessId(), restartedOwner.value.dwProcessId,
            nullptr) != ERROR_NOT_SUPPORTED)
        return fail("retired resident callbacks must never be resumed");
    if (!query() || !observeActivation(false))
        return fail("retirement must stop observation and preserve the original window procedure");
    DestroyWindow(secondary);
    DestroyWindow(window);
    UnregisterClassW(registration.lpszClassName, registration.hInstance);
    UnregisterClassW(L"Shell_TrayWnd", registration.hInstance);
    if (!GetModuleHandleW((root / L"SnowDesktopTaskbarHook.dll").c_str()) ||
        !GetModuleHandleW((root / L"ReplacementTaskbarHook.dll").c_str()))
        return fail("retired callback code must remain safe for calls already in flight");
    CloseHandle(ready);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    FreeLibrary(sentinel);
    report << "PASS: failed TAP release, cooldown status replay, same-DLL host restart retry, retired-DLL isolated recovery and reuse, cancellation, all-taskbar activation, late adapter Apply, foreign query, disable, re-enable, host exit and retirement\n";
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
    std::filesystem::create_directory(root / L"Isolated", error);
    if (error) return 1;
    std::filesystem::copy_file(hookCopy, root / L"Isolated" / L"SnowDesktopTaskbarHook.dll", error);
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
