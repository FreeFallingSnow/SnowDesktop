#pragma once

#include <filesystem>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::steam_runtime
{
inline constexpr wchar_t kDistributionManifestFilename[] =
    L"SnowDesktop.steam.json";

enum class RuntimeFileRole
{
    Required, Desktop, RuntimeManifest, Shell, Dock, Wallpaper32, Wallpaper64,
    WindowSupport, Steam, Workshop, WidgetTool, WinUI, WebView, WindowsAI,
    WindowsServices, Count,
};

[[nodiscard]] RuntimeFileRole GetRuntimeFileRole(const std::filesystem::path& relative);

struct ApplyResult
{
    bool ok = false;
    bool usedFallback = false;
    std::filesystem::path executable;
    std::string buildId;
    std::string manifestDigest;
    std::string error;
    unsigned launcherProtocol = 0;
    bool missingFile = false;
    std::filesystem::path missingFilePath;
    std::vector<std::filesystem::path> missingOptionalFiles;
};

struct LaunchPreparation
{
    ApplyResult runtime;
    std::optional<ApplyResult> previousRuntime;
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
 * runtime copy. For an interactive launch, a runtime missing only optional
 * modules can be offered before the last complete runtime. The caller asks
 * for consent before selecting an incomplete runtime with BeginRuntimeLaunch.
 */
[[nodiscard]] LaunchPreparation ApplyDistribution(
    const std::filesystem::path& installRoot, bool retryFailedLaunch = false,
    bool offerIncompleteRuntime = false);

// Only the launcher receiving this runtime's readiness acknowledgement calls
// this. A stale acknowledgement cannot confirm a newer selection.
[[nodiscard]] bool ConfirmRuntimeStarted(
    const std::filesystem::path& installRoot,
    const std::filesystem::path& executable, std::string& error);

struct LaunchAttempt
{
    std::string token;
    std::string previousRuntime;
};

// Persist the conservative data-access boundary before creating the process.
// The token prevents an older concurrent attempt from undoing a later launch.
[[nodiscard]] bool BeginRuntimeLaunch(
    const std::filesystem::path& installRoot,
    const ApplyResult& applied, LaunchAttempt& attempt,
    std::string& error, bool revalidate = false);

// Used only when the failed child has not begun accessing user data.
[[nodiscard]] ApplyResult RecoverAfterLaunchFailure(
    const std::filesystem::path& installRoot,
    const ApplyResult& failed, const LaunchAttempt& attempt);

/**
 * Retire every inactive runtime only after the current executable has reported
 * readiness. Occupied directories are left for the next launch.
 */
[[nodiscard]] PruneResult PruneInactiveRuntimes(
    const std::filesystem::path& installRoot,
    const std::filesystem::path& currentExecutable);
}
