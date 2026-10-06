#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string_view>

namespace snowdesktop::application_crash_watchdog
{
using PreventRestartCallback = std::function<bool()>;
using LaunchRestartCallback = std::function<bool(DWORD)>;

struct WatchResult
{
    DWORD error = ERROR_SUCCESS;
    DWORD exitCode = ERROR_SUCCESS;
    bool crashDetected = false;
    bool restartPrevented = false;
    bool restartAttempted = false;
    bool restartLaunched = false;
};

/** Waits for and closes an owned process handle. */
WatchResult WatchProcess(HANDLE ownedProcessHandle,
    const PreventRestartCallback& preventRestart,
    const LaunchRestartCallback& launchRestart);

/** Starts a restricted child watcher for the current process. */
bool StartForCurrentProcess(std::wstring_view executablePath);
}
