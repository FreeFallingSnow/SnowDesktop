#include "application_crash_watchdog.h"

#include "application_restart_policy.h"

#include <cstddef>
#include <string>
#include <vector>

namespace snowdesktop::application_crash_watchdog
{
WatchResult WatchProcess(HANDLE ownedProcessHandle,
    const PreventRestartCallback& preventRestart,
    const LaunchRestartCallback& launchRestart)
{
    WatchResult result;
    if (!ownedProcessHandle ||
        ownedProcessHandle == INVALID_HANDLE_VALUE)
    {
        result.error = ERROR_INVALID_HANDLE;
        return result;
    }

    const DWORD waitResult =
        WaitForSingleObject(ownedProcessHandle, INFINITE);
    const bool readExitCode = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeProcess(ownedProcessHandle, &result.exitCode) != FALSE;
    result.error = readExitCode ? ERROR_SUCCESS : GetLastError();
    CloseHandle(ownedProcessHandle);
    if (!readExitCode)
        return result;

    result.crashDetected = application_restart_policy::
        IsCrashExitCode(result.exitCode);
    if (!result.crashDetected)
        return result;

    result.restartPrevented = preventRestart && preventRestart();
    if (result.restartPrevented)
        return result;

    result.restartAttempted = true;
    result.restartLaunched =
        launchRestart && launchRestart(GetCurrentProcessId());
    return result;
}

bool StartForCurrentProcess(std::wstring_view executablePath)
{
    if (executablePath.empty())
        return false;

    HANDLE processHandle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
            GetCurrentProcess(), &processHandle,
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            TRUE, 0))
    {
        return false;
    }

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(
        nullptr, 1, 0, &attributeBytes);
    if (attributeBytes == 0)
    {
        CloseHandle(processHandle);
        return false;
    }
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<
        PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(
            attributes, 1, 0, &attributeBytes))
    {
        CloseHandle(processHandle);
        return false;
    }
    if (!UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            &processHandle, sizeof(processHandle), nullptr, nullptr))
    {
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(processHandle);
        return false;
    }

    const std::wstring executable(executablePath);
    std::wstring handleArgument(
        application_restart_policy::kWatchProcessHandlePrefix);
    handleArgument += std::to_wstring(
        reinterpret_cast<std::uintptr_t>(processHandle));
    std::wstring commandLine =
        L"\"" + executable + L"\" " + handleArgument;

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION processInfo{};
    const bool started = CreateProcessW(
        executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
        nullptr, nullptr, &startup.StartupInfo, &processInfo) != FALSE;
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(processHandle);
    if (!started)
        return false;
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}
}
