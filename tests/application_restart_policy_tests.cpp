#include "platform/application_crash_watchdog.h"
#include "platform/application_restart_policy.h"
#include "platform/crash_restart_history.h"
#include "platform/owned_window_shutdown.h"
#include "../src/app/lifecycle/startup_cancellation.h"
#include "../src/app/lifecycle/startup_diagnostics.h"
#include "shell/shell_call_diagnostics.h"

#include <iostream>
#include <array>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace { std::vector<std::wstring> startupMessages; }

// Replace only the disk sink. The production scopes and call wrapper still
// execute, including before-return logging and restoration of Win32 errors.
void WriteDiagnosticLogEntry(const wchar_t* message, DiagnosticLogLevel)
{
    startupMessages.emplace_back(message);
    SetLastError(ERROR_ACCESS_DENIED);
}

namespace
{
int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

void TestCrashHistoryRead()
{
    const std::array<DWORD, 2> expected{ 100, 120 };
    for (const DWORD size : { 0u, 3u, 4u, 5u, 7u, 8u, 260u })
    {
        int reads = 0;
        const auto ticks = snowdesktop::ReadCrashRestartHistory(
            [&](DWORD* type, BYTE* data, DWORD* bytes) -> LSTATUS {
                *type = REG_BINARY;
                if (!data) { *bytes = size; return ERROR_SUCCESS; }
                ++reads;
                if (*bytes < size || size > sizeof(expected))
                    return ERROR_MORE_DATA;
                std::memcpy(data, expected.data(), size);
                *bytes = size;
                return ERROR_SUCCESS;
            });
        const bool valid = size == 4 || size == 8;
        Expect(valid ? (ticks.size() == size / sizeof(DWORD) && ticks[0] == 100) :
                (ticks.empty() && reads == 0),
            "registry history rejects malformed sizes before allocating/reading and preserves valid ticks");
    }
    for (const int change : { 0, 1, 2, 3 })
    {
        const auto ticks = snowdesktop::ReadCrashRestartHistory(
            [&](DWORD* type, BYTE* data, DWORD* bytes) -> LSTATUS {
                *type = REG_BINARY;
                if (!data) { *bytes = 8; return ERROR_SUCCESS; }
                if (change == 0) return ERROR_ACCESS_DENIED;
                if (change == 1) *type = REG_SZ;
                *bytes = change == 2 ? 5u : change == 3 ? 12u : 8u;
                return change == 3 ? ERROR_MORE_DATA : ERROR_SUCCESS;
            });
        Expect(ticks.empty(), "a changed type/size or failed second registry read cannot supply crash ticks");
    }
    const std::wstring keyName = L"Software\\SnowDesktopTests\\CrashHistory-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HKEY key = nullptr;
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0, nullptr, 0,
        KEY_READ | KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS,
        "isolated crash-history registry fixture opens");
    if (!key) return;
    RegSetValueExW(key, L"CrashTicks", 0, REG_BINARY,
        reinterpret_cast<const BYTE*>(expected.data()), sizeof(expected));
    Expect(snowdesktop::ReadCrashRestartHistory(key) ==
        std::vector<DWORD>(expected.begin(), expected.end()), "real binary registry history is read intact");
    RegSetValueExW(key, L"CrashTicks", 0, REG_BINARY,
        reinterpret_cast<const BYTE*>(expected.data()), 5);
    Expect(snowdesktop::ReadCrashRestartHistory(key).empty(),
        "real malformed registry history is rejected");
    RegCloseKey(key);
    RegDeleteKeyW(HKEY_CURRENT_USER, keyName.c_str());
}

LRESULT CALLBACK LifetimeFixtureWindow(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* callbacks = reinterpret_cast<int*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (callbacks && (message == WM_APP || message == WM_DESTROY || message == WM_NCDESTROY))
        ++*callbacks;
    return DefWindowProcW(window, message, wp, lp);
}

void TestFailedStartupWindows()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = LifetimeFixtureWindow;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SnowDesktop.StartupLifetime.Test";
    Expect(RegisterClassW(&wc) != 0, "startup lifetime fixture registers");
    for (const bool controlCreated : { false, true })
    {
        int callbacks = 0;
        const auto create = [&] {
            return CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                HWND_MESSAGE, nullptr, wc.hInstance, &callbacks);
        };
        HWND main = create(), input = create(), control = controlCreated ? create() : nullptr;
        const HWND oldMain = main, oldInput = input, oldControl = control;
        Expect(main && input && (!controlCreated || control), "all reached startup windows exist");
        snowdesktop::RetireOwnedWindows({ &input, &main, &control });
        PostMessageW(oldMain, WM_APP, 0, 0);
        PostMessageW(oldInput, WM_APP, 0, 0);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&message);
        Expect(!main && !input && !control && !IsWindow(oldMain) &&
            !IsWindow(oldInput) && !IsWindow(oldControl) && callbacks == 0,
            "failed startup destroys every window and detaches callbacks before destruction/message pumping");
        // Also release windows when exercising an isolated old-behavior copy.
        for (HWND window : { oldMain, oldInput, oldControl })
            if (IsWindow(window)) { SetWindowLongPtrW(window, GWLP_USERDATA, 0); DestroyWindow(window); }
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

void TestStartupDiagnostics()
{
    using snowdesktop::startup_diagnostics::Call;
    using snowdesktop::startup_diagnostics::Scope;
    Call(L"ordinary paint", [] {});
    Expect(startupMessages.empty(), "runtime calls stay silent outside startup");
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        Scope startup(L"ReloadItems.partial", true, 22);
        Expect(GetLastError() == ERROR_FILE_NOT_FOUND, "begin logging preserves Win32 errors");
        Expect(startupMessages.size() == 1 &&
            startupMessages[0].find(L"begin:") != std::wstring::npos &&
            startupMessages[0].find(L"items=22") != std::wstring::npos,
            "startup records its batch before entering any blocking operation");
        std::thread unrelated([] { Call(L"unrelated thread", [] {}); });
        unrelated.join();
        Expect(startupMessages.size() == 1, "startup logging is scoped to its calling thread");
        const HRESULT result = Call(L"Clipboard.GetData", [] {
            Expect(startupMessages.size() == 2 &&
                startupMessages.back().find(L"begin:") != std::wstring::npos &&
                startupMessages.back().find(L"step=Clipboard.GetData") != std::wstring::npos,
                "an unfinished call already has a diagnostic identifying the blocked operation");
            SetLastError(ERROR_NOT_READY);
            return E_PENDING;
        });
        Expect(result == E_PENDING && GetLastError() == ERROR_NOT_READY,
            "call instrumentation preserves results and errors");
        Expect(startupMessages.size() == 3 &&
            startupMessages.back().find(L"end:") != std::wstring::npos &&
            startupMessages.back().find(L"elapsed_ms=") != std::wstring::npos,
            "completed operations append their duration");
        const auto field = [](const std::wstring& line, const wchar_t* key) {
            const auto pos = line.find(key);
            return pos == std::wstring::npos ? std::wstring{} :
                line.substr(pos, line.find(L' ', pos) - pos);
        };
        Expect(field(startupMessages[1], L"call=") == field(startupMessages[2], L"call=") &&
            field(startupMessages[0], L"call=") != field(startupMessages[1], L"call=") &&
            field(startupMessages[0], L"pass=") == field(startupMessages[1], L"pass=") &&
            field(startupMessages[0], L"run=") == field(startupMessages[1], L"run="),
            "nested calls retain the run and pass while begin/end pairs have distinct call IDs");
        try { Call(L"failed step", [] { throw 1; }); }
        catch (int) {}
        Expect(startupMessages.size() == 5 && startupMessages.back().find(L"end:") != std::wstring::npos,
            "exceptions close their diagnostic scope");
    }
    const size_t finishedCount = startupMessages.size();
    Call(L"later paint", [] {});
    Expect(finishedCount == 6 && startupMessages.size() == finishedCount,
        "finishing startup also disables subsequent runtime probes");
}

ULONGLONG diagnosticTick = 0;
ULONGLONG WINAPI DiagnosticClock() { return diagnosticTick; }

void TestSlowShellDiagnostics()
{
    using namespace snowdesktop::shell_call_diagnostics;
    startupMessages.clear();
    diagnosticTick = 1000;
    {
        Context trace(L"icon.phase1", L"C:\\app.lnk", &DiagnosticClock);
        Call(L"fast", [] { diagnosticTick += 249; });
        Expect(startupMessages.empty(), "fast Shell calls do not write to the disk sink");
        const HRESULT result = Call(L"Link.SourceIcon", [] {
            return Call(L"Resource.SHDefExtractIcon", [] {
                diagnosticTick += 250;
                SetLastError(ERROR_NOT_READY);
                return E_PENDING;
            }, L"C:\\app.exe");
        });
        Expect(result == E_PENDING && GetLastError() == ERROR_NOT_READY,
            "slow-call diagnostics preserve provider failure results and Win32 errors");
        Expect(startupMessages.size() == 2 &&
            startupMessages[0].find(L"step=Resource.SHDefExtractIcon") != std::wstring::npos &&
            startupMessages[0].find(L"elapsed_ms=250") != std::wstring::npos &&
            startupMessages[0].find(L"path=C:\\app.lnk detail=C:\\app.exe") != std::wstring::npos &&
            startupMessages[0].find(L"parentCall=2") != std::wstring::npos,
            "a nested slow resource call retains its API, duration, source, resource and parent");
        std::thread unrelated([] { Call(L"unrelated", [] {}); });
        unrelated.join();
        Expect(startupMessages.size() == 2, "other threads do not inherit a Shell task context");
        {
            Context child(L"desktop.local", L"C:\\Desktop", &DiagnosticClock);
            child.SetItem(7);
            try { Call(L"Enum.Next", [] { diagnosticTick += 300; throw 1; }); }
            catch (int) {}
        }
        const auto field = [](const std::wstring& line, const wchar_t* key) {
            const auto pos = line.find(key);
            if (pos == std::wstring::npos) return std::wstring{};
            const auto value = pos + wcslen(key);
            return line.substr(value, line.find(L' ', value) - value);
        };
        Expect(startupMessages.size() == 4 &&
            startupMessages[2].find(L"step=Enum.Next item=7") != std::wstring::npos &&
            field(startupMessages[2], L"parentTrace=") == field(startupMessages[0], L"trace="),
            "a failed nested read identifies the next item ordinal and original task");
        Call(L"after-child", [] { diagnosticTick += 250; });
        Expect(startupMessages.size() == 5 &&
            field(startupMessages.back(), L"trace=") == field(startupMessages[0], L"trace="),
            "nested reads restore the parent task for later calls");
    }
    Expect(startupMessages.size() == 6 &&
        startupMessages.back().find(L"step=total") != std::wstring::npos,
        "slow tasks retain a total for uninstrumented gaps");
    Call(L"outside", [] {});
    Expect(startupMessages.size() == 6, "completed tasks leave later callers uninstrumented");
}

HANDLE StartChild(std::wstring_view argument)
{
    std::wstring executable(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(),
        static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return nullptr;
    executable.resize(length);
    std::wstring commandLine = L"\"" + executable + L"\" " + std::wstring(argument);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(),
            nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process))
    {
        return nullptr;
    }
    CloseHandle(process.hThread);
    return process.hProcess;
}

void TestRealProcessExit(DWORD exitCode, bool preventRestart,
    bool launchSucceeds, bool expectedCrash, bool expectedAttempt,
    bool cancelStartup = false)
{
    HANDLE child = StartChild(cancelStartup ? L"--child-cancel-startup" :
        L"--child-exit=" + std::to_wstring(static_cast<unsigned long long>(exitCode)));
    Expect(child != nullptr, "watchdog test child starts");
    if (!child)
        return;

    bool preventCalled = false;
    bool launchCalled = false;
    const auto result =
        snowdesktop::application_crash_watchdog::WatchProcess(
            child,
            [&]() {
                preventCalled = true;
                return preventRestart;
            },
            [&](DWORD predecessor) {
                launchCalled = predecessor == GetCurrentProcessId();
                return launchSucceeds;
            });
    Expect(result.error == ERROR_SUCCESS,
        "watchdog reads the real child process exit code");
    Expect(result.exitCode == exitCode,
        "watchdog preserves the child process exit code");
    Expect(result.crashDetected == expectedCrash,
        "watchdog classifies the real process exit correctly");
    Expect(result.restartAttempted == expectedAttempt,
        "watchdog attempts restart only when policy allows it");
    Expect(launchCalled == expectedAttempt,
        "watchdog invokes the restart launcher exactly when expected");
    Expect(preventCalled == expectedCrash,
        "watchdog consults throttling only after a crash");
    if (expectedAttempt)
    {
        Expect(result.restartLaunched == launchSucceeds,
            "watchdog reports the restart launcher result");
    }
}
}

int wmain(int argc, wchar_t* argv[])
{
    constexpr std::wstring_view childExitPrefix = L"--child-exit=";
    if (argc == 2)
    {
        const std::wstring_view argument(argv[1]);
        if (argument == L"--child-cancel-startup")
        {
            snowdesktop::StartupCancellation cancellation;
            (void)cancellation.TerminateStartup();
            return ERROR_INVALID_FUNCTION; // Successful self-termination never returns.
        }
        if (argument.starts_with(childExitPrefix))
        {
            const std::wstring value(
                argument.substr(childExitPrefix.size()));
            ExitProcess(static_cast<DWORD>(
                std::wcstoull(value.c_str(), nullptr, 10)));
        }
    }

    if (argc > 1 && std::wstring_view(argv[1]) == L"--history")
    {
        TestCrashHistoryRead();
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::wstring_view(argv[1]) == L"--windows")
    {
        TestFailedStartupWindows();
        return failures == 0 ? 0 : 1;
    }
    using namespace snowdesktop::application_restart_policy;
    TestStartupDiagnostics();
    TestCrashHistoryRead();
    TestFailedStartupWindows();
    TestSlowShellDiagnostics();
    if (!AllowsCrashRestart(kFlags))
    {
        std::cerr << "FAILED: application restart must remain enabled for crashes\n";
        return 1;
    }
    if (AllowsHangRestart(kFlags))
    {
        std::cerr << "FAILED: application restart must remain disabled for hangs\n";
        return 1;
    }
    if (!IsCrashExitCode(0xC0000409u) ||
        !IsCrashExitCode(0xC0000005u) ||
        IsCrashExitCode(ERROR_SUCCESS) ||
        IsCrashExitCode(ERROR_INVALID_DATA))
    {
        std::cerr << "FAILED: watchdog crash exit-code classification is invalid\n";
        return 1;
    }
    if (ParseWatchProcessHandle(L"--watch-process-handle=12345") !=
            12345u ||
        ParseWatchProcessHandle(
            L"--other value --watch-process-handle=987") != 987u ||
        ParseWatchProcessHandle(L"--watch-process-handle=0") != 0u ||
        ParseWatchProcessHandle(L"--watch-process-handle=12x") != 0u ||
        ParseWatchProcessHandle(L"prefix--watch-process-handle=12") != 0u)
    {
        std::cerr << "FAILED: watchdog process-handle parsing is invalid\n";
        return 1;
    }
    TestRealProcessExit(
        ERROR_SUCCESS, false, true, false, false);
    // Exercise the actual button action in an isolated child, then the real
    // watchdog: deliberate cancellation must terminate without auto-relaunch.
    TestRealProcessExit(
        ERROR_CANCELLED, false, true, false, false, true);
    snowdesktop::StartupCancellation completedStartup;
    Expect(completedStartup.IsStarting(), "startup initially accepts cancellation");
    Expect(completedStartup.BeginDesktopHandoff(), "ready startup allows desktop takeover");
    Expect(!completedStartup.IsStarting(), "desktop takeover closes cancellation");
    Expect(!completedStartup.TerminateStartup(),
        "a stale button action cannot terminate a ready desktop");
    Expect(completedStartup.BeginDesktopHandoff(), "repeated completion remains safe");
    TestRealProcessExit(
        0xC0000409u, false, true, true, true);
    TestRealProcessExit(
        0xC0000005u, true, true, true, false);
    TestRealProcessExit(
        0xC0000409u, false, false, true, true);
    if (failures != 0)
        return 1;
    std::cout << "Application restart policy tests passed\n";
    return 0;
}
