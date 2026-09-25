#pragma once

#include "auto_start_manager.h"
#include <optional>

namespace snowdesktop::auto_start
{
// Private, short-lived IPC command. It never starts the desktop host or changes
// the task's runtime elevation level. Only explicit settings actions call it.
[[nodiscard]] bool ConfigureElevated(const Target& target, bool enabled,
    std::wstring* error) noexcept;
[[nodiscard]] std::optional<int> TryRunElevationCommand() noexcept;
}
