#include "application_crash_watchdog.h"
#include "application_restart_policy.h"

#include <iostream>
#include <string>

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

HANDLE StartExitCodeChild(DWORD exitCode)
{
    std::wstring executable(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, executable.data(),
        static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return nullptr;
    executable.resize(length);
    std::wstring commandLine = L"\"" + executable +
        L"\" --child-exit=" +
        std::to_wstring(static_cast<unsigned long long>(exitCode));
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
    bool launchSucceeds, bool expectedCrash, bool expectedAttempt)
{
    HANDLE child = StartExitCodeChild(exitCode);
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
        if (argument.starts_with(childExitPrefix))
        {
            const std::wstring value(
                argument.substr(childExitPrefix.size()));
            ExitProcess(static_cast<DWORD>(
                std::wcstoull(value.c_str(), nullptr, 10)));
        }
    }

    using namespace snowdesktop::application_restart_policy;
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
