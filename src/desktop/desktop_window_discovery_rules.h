#pragma once

#include <cstdint>

namespace snowdesktop::desktop_window_discovery_rules
{

constexpr bool IsExplorerDesktopViewProcess(
    std::uint32_t candidateProcessId,
    std::uint32_t shellProcessId) noexcept
{
    return candidateProcessId != 0 &&
        shellProcessId != 0 &&
        candidateProcessId == shellProcessId;
}

} // namespace snowdesktop::desktop_window_discovery_rules
