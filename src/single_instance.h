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
