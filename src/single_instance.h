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

struct InstanceInfo
{
    HWND controlWindow = nullptr;
    DWORD processId = 0;
    std::wstring version;
    std::wstring executablePath;
    std::wstring dataDirectory;
    bool packaged = false;
};

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
