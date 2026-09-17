#pragma once

#include <windows.h>
#include <lm.h>
#include <iostream>
#include <string>

// Test-only guard: Windows.Storage may read mapped-drive desktop.ini files
// even when every file-operation fixture is local. Querying connection state
// can itself block, so no network-provider API runs in the test process.
namespace shell_network_preflight
{
constexpr int Ready = 0;
constexpr int Failed = 1;
constexpr int Unavailable = 77;
constexpr int Unknown = 78;
constexpr int TimedOut = 79;
constexpr DWORD ProbeTimeoutMs = 3000;

inline int ClassifyConnection(DWORD status)
{
    switch (status)
    {
    case USE_OK: return Ready;
    case USE_PAUSED:
    case USE_SESSLOST:
    case USE_NETERR:
    case USE_CONN:
    case USE_RECONN: return Unavailable;
    default: return Unknown;
    }
}

inline int QueryConnections()
{
    DWORD resume = 0;
    NET_API_STATUS status;
    do
    {
        LPBYTE buffer = nullptr;
        DWORD count = 0, total = 0;
        status = NetUseEnum(nullptr, 1, &buffer, MAX_PREFERRED_LENGTH,
            &count, &total, &resume);
        int result = Ready;
        if (status != NERR_Success && status != ERROR_MORE_DATA)
            result = Unknown;
        else
        {
            const auto* entries = reinterpret_cast<const USE_INFO_1*>(buffer);
            for (DWORD i = 0; i < count; ++i)
            {
                // Printers and unassigned server sessions cannot be drive roots.
                if (entries[i].ui1_asg_type != USE_DISKDEV ||
                    !entries[i].ui1_local || !entries[i].ui1_local[0]) continue;
                const int connection = ClassifyConnection(entries[i].ui1_status);
                if (connection != Ready) result = connection;
            }
        }
        if (buffer) NetApiBufferFree(buffer);
        if (result != Ready) return result;
    } while (status == ERROR_MORE_DATA);
    return Ready;
}

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit Handle(HANDLE handle) : value(handle) {}
};

inline int RunProbe(const wchar_t* argument, DWORD timeoutMs = ProbeTimeoutMs)
{
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return Failed;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " + argument;

    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value,
        JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return Failed;

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child))
        return Failed;
    Handle process(child.hProcess), thread(child.hThread);
    const bool started = AssignProcessToJobObject(job.value, process.value) &&
        ResumeThread(thread.value) != static_cast<DWORD>(-1);
    const DWORD wait = started ? WaitForSingleObject(process.value, timeoutMs) : WAIT_FAILED;
    if (wait != WAIT_OBJECT_0)
    {
        // Only the read-only preflight helper is terminated, never the real
        // integration test after it has started. Confirm cleanup before skip.
        const bool stopped = TerminateProcess(process.value, TimedOut) &&
            WaitForSingleObject(process.value, 1000) == WAIT_OBJECT_0;
        return stopped && wait == WAIT_TIMEOUT ? TimedOut : Failed;
    }
    DWORD exitCode = Failed;
    if (!GetExitCodeProcess(process.value, &exitCode)) return Failed;
    if (exitCode == Ready || exitCode == Unavailable || exitCode == Unknown)
        return static_cast<int>(exitCode);
    return Failed; // Crashes and broken probes must not become environment skips.
}

inline int CheckEnvironment()
{
    const int result = RunProbe(L"--query-network-connections");
    if (result == Unavailable)
        std::cout << "SKIP: mapped SMB drive is disconnected, reconnecting or unavailable.\n";
    else if (result == Unknown)
        std::cout << "SKIP: mapped SMB drive state could not be determined.\n";
    else if (result == TimedOut)
        std::cout << "SKIP: mapped SMB drive state query exceeded 3000 ms; helper stopped.\n";
    else if (result != Ready)
        std::cerr << "FAILED: network preflight helper failed.\n";
    if (result == Unavailable || result == Unknown || result == TimedOut)
    {
        std::cout << "Shell integration was not started; no file-operation coverage claimed.\n";
        return Unavailable;
    }
    return result;
}
}
