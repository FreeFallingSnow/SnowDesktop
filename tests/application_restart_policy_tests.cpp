#include "platform/application_crash_watchdog.h"
#include "platform/application_restart_policy.h"
#include "platform/crash_restart_history.h"
#include "platform/owned_window_shutdown.h"
#include "platform/user_object_recovery.h"
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
    const auto previousModules = snowdesktop::diagnostics::enabledLogModules.load();
    struct RestoreModules
    {
        std::uint32_t value;
        ~RestoreModules() { snowdesktop::diagnostics::SetLogModules(value); }
    } restore{previousModules};
    snowdesktop::diagnostics::SetLogModules(
        snowdesktop::diagnostics::ModuleBit(DiagnosticLogModule::Runtime));
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
    snowdesktop::diagnostics::SetLogModules(0);
    diagnosticTick = 1000;
    {
        Context disabled(L"desktop.full", L"C:\\Desktop", &DiagnosticClock);
        const HRESULT result = Call(L"Enum.Next", [] {
            diagnosticTick += 625;
            SetLastError(ERROR_NOT_READY);
            return E_PENDING;
        });
        Expect(result == E_PENDING && GetLastError() == ERROR_NOT_READY,
            "disabled Shell timing preserves provider results and errors");
    }
    Expect(startupMessages.empty(), "disabled Shell timing emits no per-call or total records");
    snowdesktop::diagnostics::SetLogModules(
        snowdesktop::diagnostics::ModuleBit(DiagnosticLogModule::Shell));
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
    snowdesktop::diagnostics::SetLogModules(0);
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
    const DWORD expectedError = expectedAttempt && !launchSucceeds ? ERROR_GEN_FAILURE : ERROR_SUCCESS;
    Expect(result.error == expectedError,
        "watchdog preserves successful observation and reports failed restart launch");
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

void TestRecoveryFailureDecisions()
{
    using namespace snowdesktop::application_crash_watchdog;
    for (const int scenario : {0, 1, 2, 3})
    {
        HANDLE child = StartChild(L"--child-exit=3221225477");
        Expect(child != nullptr, "recovery decision fixture starts a real isolated child");
        if (!child) continue;
        int launches = 0, choices = 0;
        const bool prevented = scenario < 2;
        const auto result = WatchProcess(child, [=] { return prevented; },
            [&](DWORD predecessor) {
                ++launches;
                Expect(predecessor == GetCurrentProcessId(), "manual recovery waits for its watcher process");
                if (scenario >= 2 && launches == 1) { SetLastError(ERROR_ACCESS_DENIED); return false; }
                return true;
            },
            [&](const WatchResult& failure) {
                ++choices;
                Expect(failure.crashDetected && (failure.restartPrevented || failure.error == ERROR_ACCESS_DENIED),
                    "continuation is offered only after actual crash throttling or a failed restart boundary");
                return scenario == 1 || scenario == 3;
            });
        const int expectedLaunches = scenario == 0 ? 0 : scenario == 3 ? 2 : 1;
        Expect(choices == 1 && launches == expectedLaunches && result.restartLaunched == (scenario == 1 || scenario == 3),
            "crash recovery offers a real continue/cancel choice and never retries without consent");
        const DWORD expectedError = scenario == 2 ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
        Expect(result.error == expectedError,
            "recovery preserves the launch error or records a successful explicit continuation");
    }
}

// Replace only the resource query, persistence and restart boundaries. The
// production monitor owns timing, busy deferral, reentrancy and attempt limits.
void TestUserObjectRecovery()
{
    namespace recovery = snowdesktop::user_object_recovery;
    struct Fixture
    {
        recovery::Monitor monitor;
        DWORD count = 10000, quota = 10000;
        bool safe = true, saved = true, launched = true, reenter = false;
        bool throwCheckpoint = false, throwRestart = false, throwReport = false;
        LSTATUS reservation = ERROR_SUCCESS;
        LSTATUS lastError = ERROR_SUCCESS;
        int queries = 0, checkpoints = 0, reservations = 0, restarts = 0;
        std::vector<recovery::Event> events;
        bool Poll(ULONGLONG now)
        {
            return monitor.Poll(now, quota, safe,
                [&] { ++queries; return count; },
                [&] {
                    ++checkpoints;
                    if (throwCheckpoint) throw std::bad_alloc{};
                    if (reenter)
                    {
                        reenter = false;
                        const auto before = queries;
                        Expect(!Poll(now + 10000) && queries == before,
                            "nested timer dispatch cannot query or restart during a recovery checkpoint");
                    }
                    return saved;
                },
                [&](ULONGLONG) { ++reservations; return reservation; },
                [&] { ++restarts; if (throwRestart) throw 1; return launched; },
                [&](recovery::Event event, DWORD objects, DWORD limit, LSTATUS error) {
                    if (throwReport) throw std::bad_alloc{};
                    lastError = error;
                    Expect(objects == count && limit == quota,
                        "recovery diagnostics retain the triggering resource sample");
                    events.push_back(event);
                });
        }
    };
    Fixture exhausted;
    Expect(!exhausted.Poll(0) && !exhausted.Poll(500) && exhausted.queries == 1,
        "resource sampling does not allocate another timer or run per input message");
    Expect(!exhausted.Poll(1000) && !exhausted.Poll(2000) && exhausted.Poll(3000) &&
            exhausted.checkpoints == 1 && exhausted.reservations == 1 && exhausted.restarts == 1,
        "the reported 10000-object exhaustion invokes one saved, budgeted restart after sustained pressure");
    Expect(!exhausted.Poll(4000) && exhausted.restarts == 1,
        "a queued maintenance message cannot immediately repeat an accepted restart");

    Fixture transient;
    transient.count = 9500;
    transient.Poll(0);
    transient.count = 9400;
    transient.Poll(2000);
    transient.count = 9500;
    Expect(!transient.Poll(4000) && !transient.Poll(6000) && transient.Poll(7000),
        "a temporary near-quota spike resets the continuous-pressure deadline");

    Fixture busy;
    busy.safe = false;
    busy.Poll(0);
    Expect(!busy.Poll(5000) && busy.checkpoints == 0 && busy.reservations == 0 && busy.restarts == 0,
        "busy drag, menu, file-operation and startup gates cannot save an intermediate model or restart");
    busy.safe = true;
    busy.reenter = true;
    Expect(busy.Poll(6000) && busy.restarts == 1,
        "sustained exhaustion can recover once the host reaches a safe maintenance boundary");

    Fixture unsaved;
    unsaved.saved = false;
    unsaved.Poll(0);
    Expect(!unsaved.Poll(3000) && unsaved.reservations == 0 && unsaved.restarts == 0 &&
            unsaved.events.back() == recovery::Event::CheckpointFailed,
        "layout persistence failure retains the old host without consuming a restart reservation");
    unsaved.saved = true;
    Expect(!unsaved.Poll(32000) && unsaved.Poll(33000),
        "a failed checkpoint retries at a bounded interval instead of on every maintenance tick");

    for (const LSTATUS error : {ERROR_RETRY, ERROR_ACCESS_DENIED})
    {
        Fixture limited;
        limited.reservation = error;
        limited.Poll(0);
        Expect(!limited.Poll(3000) && limited.restarts == 0 &&
                limited.events.back() == recovery::Event::RestartLimited &&
                !limited.Poll(33000) && limited.reservations == 1,
            "a previous restart or unavailable persistent budget denies automatic relaunch");
    }
    Fixture failed;
    failed.launched = false;
    failed.Poll(0);
    Expect(!failed.Poll(3000) && failed.events.back() == recovery::Event::RestartFailed &&
            !failed.Poll(33000) && failed.restarts == 1,
        "a failed restart handoff keeps the host and cannot form a tight launch loop");
    Fixture noReading;
    noReading.count = 0;
    noReading.Poll(0);
    Expect(!noReading.Poll(10000) && noReading.checkpoints == 0 && noReading.events.empty(),
        "a failed or empty USER query is not classified as exhaustion");
    Fixture customQuota;
    customQuota.quota = 18000;
    customQuota.Poll(0);
    Expect(!customQuota.Poll(4000) && customQuota.restarts == 0,
        "10000 objects do not exhaust an explicitly raised 18000-object quota");
    customQuota.count = 17100;
    Expect(!customQuota.Poll(5000) && customQuota.Poll(8000),
        "a configured process quota determines the sustained recovery threshold");

    Fixture allocationFailure;
    allocationFailure.throwCheckpoint = true;
    allocationFailure.Poll(0);
    bool escaped = false;
    try { allocationFailure.Poll(3000); } catch (...) { escaped = true; }
    Expect(!escaped && allocationFailure.reservations == 0 && allocationFailure.restarts == 0 &&
            allocationFailure.lastError == ERROR_NOT_ENOUGH_MEMORY,
        "low-memory checkpoint exceptions keep the old host without escaping its timer handler");
    allocationFailure.throwCheckpoint = false;
    Expect(allocationFailure.Poll(33000),
        "exception recovery releases its reentrancy guard for a later bounded attempt");
    Fixture launchException;
    launchException.throwRestart = true;
    launchException.Poll(0);
    try { launchException.Poll(3000); } catch (...) { escaped = true; }
    Expect(!escaped && launchException.lastError == ERROR_UNHANDLED_EXCEPTION &&
            !launchException.Poll(33000) && launchException.restarts == 1,
        "a throwing restart endpoint retains the persistent cooldown and cannot crash the timer");
    Fixture diagnosticFailure;
    diagnosticFailure.throwReport = true;
    diagnosticFailure.Poll(0);
    Expect(diagnosticFailure.Poll(3000) && diagnosticFailure.restarts == 1,
        "a diagnostic allocation failure cannot interrupt a saved and budgeted recovery");
}

void TestUserObjectRestartBudget()
{
    namespace recovery = snowdesktop::user_object_recovery;
    const std::wstring keyName = L"Software\\SnowDesktopTests\\UserObjectRecovery-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto now = GetTickCount64();
    Expect(recovery::ReserveRestart(now, HKEY_CURRENT_USER, keyName.c_str()) == ERROR_SUCCESS,
        "first automatic recovery reserves its isolated registry budget");
    HANDLE child = StartChild(L"\"--child-resource-budget=" + keyName + L"\"");
    Expect(child != nullptr, "a fresh process checks the persisted recovery budget");
    if (child)
    {
        const auto wait = WaitForSingleObject(child, 10000);
        DWORD code = STILL_ACTIVE;
        GetExitCodeProcess(child, &code);
        Expect(wait == WAIT_OBJECT_0 && code == 0,
            "a replacement process cannot bypass the automatic restart interval");
        if (wait != WAIT_OBJECT_0) { TerminateProcess(child, ERROR_CANCELLED); WaitForSingleObject(child, 5000); }
        CloseHandle(child);
    }
    Expect(recovery::ReserveRestart(now + 15 * 60 * 1000, HKEY_CURRENT_USER, keyName.c_str()) == ERROR_SUCCESS,
        "a later recovery can reserve again after the documented fifteen-minute interval");
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        const DWORD malformed = 42;
        RegSetValueExW(key, L"LastRestartUptime", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&malformed), sizeof(malformed));
        RegCloseKey(key);
    }
    Expect(recovery::ReserveRestart(now, HKEY_CURRENT_USER, keyName.c_str()) != ERROR_SUCCESS,
        "an unreadable or malformed persistent guard cannot authorize a restart loop");
    RegDeleteTreeW(HKEY_CURRENT_USER, keyName.c_str());
}
}

int wmain(int argc, wchar_t* argv[])
{
    constexpr std::wstring_view childExitPrefix = L"--child-exit=";
    if (argc == 2)
    {
        const std::wstring_view argument(argv[1]);
        constexpr std::wstring_view budgetPrefix = L"--child-resource-budget=";
        if (argument.starts_with(budgetPrefix))
        {
            const std::wstring key(argument.substr(budgetPrefix.size()));
            return snowdesktop::user_object_recovery::ReserveRestart(
                GetTickCount64(), HKEY_CURRENT_USER, key.c_str()) == ERROR_RETRY ? 0 : 1;
        }
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
    TestRecoveryFailureDecisions();
    TestUserObjectRecovery();
    TestUserObjectRestartBudget();
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
