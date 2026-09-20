#pragma once

#include "auto_start_rules.h"

#include <string>
#include <string_view>
#include <utility>

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
    std::wstring error;
};

// Internal task-store boundary. Tests use a unique folder and the real Windows
// scheduler, without touching the user's production login task.
class TaskStore
{
public:
    explicit TaskStore(std::wstring folder = L"\\SnowDesktop")
        : folder_(std::move(folder)) {}
    [[nodiscard]] State Query() const noexcept;
    [[nodiscard]] bool Configure(const Target& target, bool enabled,
        std::wstring_view description, std::wstring* error = nullptr) const noexcept;
    [[nodiscard]] bool SetEnabled(bool enabled,
        std::wstring* error = nullptr) const noexcept;
    [[nodiscard]] bool Delete(std::wstring* error = nullptr) const noexcept;

private:
    std::wstring folder_;
};

/** Return the scheduled-task target for the running deployment. */
[[nodiscard]] Target CurrentDeploymentTarget() noexcept;

/** Return the stable packaged target used by portable-to-installed migration. */
[[nodiscard]] Target PackagedDeploymentTarget() noexcept;

/** Convert a legacy Run command into a portable scheduled-task target. */
[[nodiscard]] Target PortableTargetFromLegacyCommand(
    std::wstring_view command) noexcept;

/** Query the one SnowDesktop-owned per-user logon task. */
[[nodiscard]] State Query() noexcept;

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
