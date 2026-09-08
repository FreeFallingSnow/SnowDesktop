#pragma once

#include <filesystem>
#include <cstddef>
#include <string>

namespace snowdesktop::steam_runtime
{
inline constexpr wchar_t kDistributionManifestFilename[] =
    L"SnowDesktop.steam.json";

struct ApplyResult
{
    bool ok = false;
    bool usedFallback = false;
    std::filesystem::path executable;
    std::string buildId;
    std::string error;
};

struct PruneResult
{
    bool ok = false;
    std::size_t removed = 0;
    std::size_t retained = 0;
    std::string error;
};

/**
 * Validate the Steam-controlled distribution and materialize an immutable
 * runtime copy. If an update is incomplete, the last completed runtime is
 * returned without modifying user data.
 */
[[nodiscard]] ApplyResult ApplyDistribution(
    const std::filesystem::path& installRoot);

/**
 * Remove every launcher-owned inactive runtime except the selected
 * executable's runtime, including runtimes polluted by unexpected files.
 * Occupied directories are retained for a later retry.
 */
[[nodiscard]] PruneResult PruneInactiveRuntimes(
    const std::filesystem::path& installRoot,
    const std::filesystem::path& currentExecutable);
}
