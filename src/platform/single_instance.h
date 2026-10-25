/**
 * @file single_instance.h
 * @brief Cross-deployment SnowDesktop single-instance coordination.
 */

#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace snowdesktop::single_instance
{
inline constexpr wchar_t kMutexName[] =
    L"Local\\FreeFallingSnow.SnowDesktop.SingleInstance.v1";

enum class AcquireResult
{
    Primary,
    Existing,
    Error
};

class Guard
{
public:
    Guard() = default;
    ~Guard();

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    AcquireResult Acquire(const wchar_t* mutexName = kMutexName);
    DWORD LastError() const { return lastError_; }

private:
    HANDLE mutex_ = nullptr;
    DWORD lastError_ = ERROR_SUCCESS;
};

// Internal tray/settings restart handoff. Preparation can fail while the old
// host is still usable; the child cannot execute until host teardown finishes.
class PreparedRestart
{
public:
    PreparedRestart() = default;
    ~PreparedRestart();
    PreparedRestart(const PreparedRestart&) = delete;
    PreparedRestart& operator=(const PreparedRestart&) = delete;

    DWORD Prepare(std::wstring_view executablePath);
    DWORD Resume();
    DWORD ProcessId() const { return processId_; }

private:
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
    DWORD processId_ = 0;
};

struct InstanceInfo
{
    HWND controlWindow = nullptr;
    DWORD processId = 0;
    std::wstring version;
    std::wstring executablePath;
    std::wstring dataDirectory;
    bool packaged = false;
};

enum class LaunchIntent
{
    Interactive,
    AutoStart,
    ApplicationRecovery
};

enum class ExistingLaunchAction
{
    ExitNewInstance,
    ReplaceManagedRuntime,
    PromptVersionConflict,
    ActivationFailed
};

enum class StartupResolution { ExitNewInstance, RetryLaunch };
struct StartupAcquisition
{
    bool primary = false;
    DWORD error = ERROR_SUCCESS;
};

// Keep the actual bounded startup loop shared with regression tests. Find and
// resolve are the window/UI boundaries; Guard still owns the native mutex.
template<typename Find, typename Resolve, typename Report>
StartupAcquisition AcquireForStartup(Guard& guard, Find&& find, Resolve&& resolve,
    Report&& report, const wchar_t* mutexName = kMutexName)
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (const auto running = find(0))
        {
            if (resolve(*running) == StartupResolution::ExitNewInstance) return {};
            continue;
        }
        const auto acquired = guard.Acquire(mutexName);
        if (acquired == AcquireResult::Primary) return {true};
        if (acquired == AcquireResult::Error)
        {
            const DWORD error = guard.LastError() ? guard.LastError() : ERROR_GEN_FAILURE;
            report(error);
            return {false, error};
        }
        (void)find(5000);
    }
    // A live mutex without a usable control window is not a successful handoff.
    report(ERROR_BUSY);
    return {false, ERROR_BUSY};
}

// Accept a full command line (including the executable) so quoted paths and
// values cannot be mistaken for a background launch marker.
LaunchIntent ParseLaunchIntent(std::wstring_view commandLine);

// Deliver settings activation only for an interactive compatible launch.
// Background duplicates exit silently; the current Steam launcher may still
// replace a restored immutable runtime from the same installation.
ExistingLaunchAction HandleExistingInstanceLaunch(
    const InstanceInfo& running, const InstanceInfo& requested,
    LaunchIntent intent);

/**
 * @brief Parse the internal restart predecessor argument.
 */
DWORD ParseRestartPredecessorProcessId(std::wstring_view commandLine);

/**
 * @brief Wait until the process that launched this restart has exited.
 */
bool WaitForRestartPredecessor(
    DWORD processId, DWORD timeoutMilliseconds);

/**
 * @brief Describe the current executable and its deployment data directory.
 */
InstanceInfo DescribeCurrentInstance(std::wstring_view version);

/**
 * @brief Resolve the data directory for a queried executable deployment.
 *
 * A valid Steam runtime sidecar supplies the managed or local-development
 * data root. Packaged and sidecar-free portable executables retain their
 * legacy LocalState/data and executable-relative data layouts.
 */
std::wstring ResolveInstanceDataDirectory(
    std::wstring_view executablePath,
    std::wstring_view packageFamilyName = {});

/**
 * @brief Find the running instance and read its version/deployment details.
 */
std::optional<InstanceInfo> FindExistingInstance(
    DWORD timeoutMilliseconds);

/**
 * @brief Compare display versions while accepting omitted trailing zero parts.
 */
bool VersionsMatch(
    std::wstring_view left, std::wstring_view right);

/**
 * @brief Check whether two instances use the same data directory.
 */
bool DataDirectoriesMatch(
    std::wstring_view left, std::wstring_view right);

/**
 * @brief Check whether a launch requests a different immutable runtime from
 * the same managed Steam installation.
 */
bool IsManagedSteamRuntimeReplacement(
    const InstanceInfo& running, const InstanceInfo& requested);

/**
 * @brief Ask a specific running instance to show its settings.
 */
bool NotifyExistingInstance(const InstanceInfo& instance);

/**
 * @brief Ask an already running SnowDesktop instance to show its settings.
 */
bool NotifyExistingInstance(DWORD timeoutMilliseconds);

/**
 * @brief Gracefully close a running instance and wait for it to exit.
 */
bool RequestExistingInstanceExit(
    const InstanceInfo& instance, DWORD timeoutMilliseconds);
}
