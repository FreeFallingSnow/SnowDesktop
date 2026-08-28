#pragma once

#include "auto_start_rules.h"

#include <string>
#include <string_view>

namespace snowdesktop::auto_start
{

struct Target
{
    UnifiedAutoStartOwner owner = UnifiedAutoStartOwner::Unknown;
    std::wstring executable;
    std::wstring arguments;
    std::wstring workingDirectory;
};

struct State
{
    UnifiedAutoStartTaskState status =
        UnifiedAutoStartTaskState::Unavailable;
    Target target;
    bool migrationPending = false;
    bool enableAfterMigration = false;
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
[[nodiscard]] bool Configure(const Target& target, bool enabled) noexcept;

/** Stage a disabled task that records the intended post-migration state. */
[[nodiscard]] bool ConfigureMigration(
    const Target& target, bool enableAfterMigration) noexcept;

/** Change only the Enabled bit of an existing SnowDesktop-owned task. */
[[nodiscard]] bool SetEnabled(bool enabled) noexcept;

/** Delete the SnowDesktop-owned task, used only to roll back migration. */
[[nodiscard]] bool Delete() noexcept;

/** Test whether a queried task target belongs to the running deployment. */
[[nodiscard]] bool IsCurrentDeploymentTarget(
    const Target& target) noexcept;

} // namespace snowdesktop::auto_start
