#pragma once

#include <filesystem>
#include <string>

namespace snowdesktop::deployment
{
inline constexpr wchar_t kSteamRuntimeContextFilename[] =
    L"SnowDesktop.runtime-context.json";
inline constexpr wchar_t kSteamLauncherFilename[] =
    L"SnowDesktopLauncher.exe";

enum class RuntimeDeploymentKind
{
    Portable,
    Packaged,
    SteamManaged,
    SteamLocalDevelopment,
    Invalid,
};

struct RuntimeDeploymentContext
{
    RuntimeDeploymentKind kind = RuntimeDeploymentKind::Portable;
    bool explicitContext = false;
    std::filesystem::path installRoot;
    std::filesystem::path dataRoot;
    std::filesystem::path launcher;
    std::string profileId;
    std::string error;
};

/**
 * Resolve the deployment identity for one executable.
 *
 * MSIX package identity always wins. An unpackaged executable is Steam only
 * when a valid runtime sidecar exists next to it. Only a conclusive absence
 * retains the legacy portable behaviour; probe, I/O, path-type, and content
 * errors fail closed.
 */
[[nodiscard]] RuntimeDeploymentContext ResolveRuntimeDeploymentContext(
    const std::filesystem::path& executablePath,
    bool packaged);

[[nodiscard]] bool IsSteamDeployment(
    RuntimeDeploymentKind kind) noexcept;
[[nodiscard]] bool CanOwnProductionAutoStart(
    RuntimeDeploymentKind kind) noexcept;
}
