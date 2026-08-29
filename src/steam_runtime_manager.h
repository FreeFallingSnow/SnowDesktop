#pragma once

#include <filesystem>
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

/**
 * Validate the Steam-controlled distribution and materialize an immutable
 * runtime copy. If an update is incomplete, the last completed runtime is
 * returned without modifying user data.
 */
[[nodiscard]] ApplyResult ApplyDistribution(
    const std::filesystem::path& installRoot);
}
