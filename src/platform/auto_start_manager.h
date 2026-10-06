#pragma once

#include "auto_start_rules.h"

#include <string>
#include <string_view>
#include <utility>
#include <functional>

namespace snowdesktop::auto_start
{

struct Target
{
    UnifiedAutoStartOwner owner = UnifiedAutoStartOwner::Unknown;
    std::wstring executable;
    std::wstring arguments;
    std::wstring workingDirectory;
    std::wstring error;
};

struct State
{
    UnifiedAutoStartTaskState status =
        UnifiedAutoStartTaskState::Unavailable;
    Target target;
    bool migrationPending = false;
    bool enableAfterMigration = false;
    // Preserve the Windows enabled bit even when old task metadata is invalid.
    bool enabledKnown = false;
    bool enabled = false;
    std::wstring error;
};

// Internal task-store boundary. Tests use a unique folder and the real Windows
// scheduler, without touching the user's production login task.
class TaskStore
{
public:
    explicit TaskStore(std::wstring folder = L"\\SnowDesktop", std::wstring userSid = {})
        : folder_(std::move(folder)), userSid_(std::move(userSid)) {}
    [[nodiscard]] State Query() const noexcept;
    [[nodiscard]] bool Configure(const Target& target, bool enabled,
        std::wstring_view description, std::wstring* error = nullptr,
        std::int32_t* failureCode = nullptr) const noexcept;
    [[nodiscard]] bool SetEnabled(bool enabled,
        std::wstring* error = nullptr) const noexcept;
    [[nodiscard]] bool Delete(std::wstring* error = nullptr) const noexcept;

private:
    std::wstring folder_;
    std::wstring userSid_;
};

// Separate from the legacy SnowDesktop value consumed by migration. Paths are
// injectable so integration tests never modify the real login registrations.
class RunStore
{
public:
    explicit RunStore(
        std::wstring runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        std::wstring approvalKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
        std::wstring valueName = L"SnowDesktopFallback")
        : runKey_(std::move(runKey)), approvalKey_(std::move(approvalKey)),
          valueName_(std::move(valueName)) {}
    [[nodiscard]] State Query() const noexcept;
    [[nodiscard]] bool Configure(const Target& target, bool enabled,
        std::wstring* error = nullptr) const noexcept;
    [[nodiscard]] bool Delete(std::wstring* error = nullptr) const noexcept;
private:
    std::wstring runKey_, approvalKey_, valueName_;
};

class LoginStore
{
public:
    using ElevationRetry = std::function<bool(const Target&, bool, std::wstring*)>;
    LoginStore(TaskStore task = TaskStore{}, RunStore run = RunStore{}, ElevationRetry elevate = {})
        : task_(std::move(task)), run_(std::move(run)), elevate_(std::move(elevate)) {}
    [[nodiscard]] State Query() const noexcept;
    [[nodiscard]] bool Configure(const Target& target, bool enabled,
        std::wstring* error = nullptr) const noexcept;
private:
    TaskStore task_;
    RunStore run_;
    ElevationRetry elevate_;
};

// Shared native diagnostics; these are private host implementation interfaces.
[[nodiscard]] std::wstring DescribeError(std::wstring_view operation, std::int32_t code);

/** Return the scheduled-task target for the running deployment. */
[[nodiscard]] Target CurrentDeploymentTarget() noexcept;

/** Return the stable packaged target used by portable-to-installed migration. */
[[nodiscard]] Target PackagedDeploymentTarget() noexcept;

/** Convert a legacy Run command into a portable scheduled-task target. */
[[nodiscard]] Target PortableTargetFromLegacyCommand(
    std::wstring_view command) noexcept;

/** Query the scheduled task and current-user fallback registration. */
[[nodiscard]] State Query() noexcept;

/** Apply an explicit user choice, trying the current-user Run fallback. */
[[nodiscard]] bool Apply(const Target& target, bool enabled,
    std::wstring* error = nullptr) noexcept;

/** Create or replace the SnowDesktop-owned task with the requested target. */
[[nodiscard]] bool Configure(const Target& target, bool enabled,
    std::wstring* error = nullptr) noexcept;

/** Stage a disabled task that records the intended post-migration state. */
[[nodiscard]] bool ConfigureMigration(
    const Target& target, bool enableAfterMigration,
    std::wstring* error = nullptr) noexcept;

/** Change only the Enabled bit of an existing SnowDesktop-owned task. */
[[nodiscard]] bool SetEnabled(bool enabled, std::wstring* error = nullptr) noexcept;

/** Delete the SnowDesktop-owned task, used only to roll back migration. */
[[nodiscard]] bool Delete(std::wstring* error = nullptr) noexcept;

/** Test whether a queried task target belongs to the running deployment. */
[[nodiscard]] bool IsCurrentDeploymentTarget(
    const Target& target) noexcept;

} // namespace snowdesktop::auto_start
