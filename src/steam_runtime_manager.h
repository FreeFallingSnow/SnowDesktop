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
    unsigned launcherProtocol = 0;
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
    const std::filesystem::path& installRoot, bool retryFailedLaunch = false);

// Only the launcher receiving this runtime's readiness acknowledgement calls
// this. A stale acknowledgement cannot confirm a newer selection.
[[nodiscard]] bool ConfirmRuntimeStarted(
    const std::filesystem::path& installRoot,
    const std::filesystem::path& executable, std::string& error);

// Used only when the failed child has not begun accessing user data.
[[nodiscard]] ApplyResult RecoverAfterLaunchFailure(
    const std::filesystem::path& installRoot,
    const std::filesystem::path& failedExecutable);

/**
 * Retire inactive runtimes only for the confirmed current executable, keeping
 * its predecessor. Occupied directories are left for the next launch.
 */
[[nodiscard]] PruneResult PruneInactiveRuntimes(
    const std::filesystem::path& installRoot,
    const std::filesystem::path& currentExecutable);
}
