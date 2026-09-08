#pragma once

#include "steam_runtime_context.h"

#include <filesystem>
#include <optional>

namespace snowdesktop::data_paths
{
struct RuntimeDataPathPolicy
{
    std::filesystem::path dataRoot;
    std::optional<std::filesystem::path> legacyRoot;
    std::optional<std::filesystem::path> pendingMigrationStateRoot;
};

/**
 * Resolve the paths that the current deployment is allowed to read or mutate.
 *
 * A local Steam development profile deliberately has no legacy or pending
 * migration root. This keeps a development launch from inspecting or moving
 * data owned by the managed Steam installation.
 */
inline RuntimeDataPathPolicy ResolveRuntimeDataPathPolicy(
    const deployment::RuntimeDeploymentContext& context,
    const std::filesystem::path& packageLocalState,
    const std::filesystem::path& executableDirectory)
{
    RuntimeDataPathPolicy policy;
    if (context.kind == deployment::RuntimeDeploymentKind::Invalid)
        return policy;

    if (!packageLocalState.empty())
    {
        policy.dataRoot = packageLocalState / L"data";
        policy.legacyRoot = packageLocalState;
        policy.pendingMigrationStateRoot = packageLocalState;
        return policy;
    }

    if (context.kind == deployment::RuntimeDeploymentKind::SteamManaged)
    {
        policy.dataRoot = context.dataRoot;
        policy.legacyRoot = context.installRoot;
        policy.pendingMigrationStateRoot = context.installRoot;
        return policy;
    }

    if (context.kind ==
        deployment::RuntimeDeploymentKind::SteamLocalDevelopment)
    {
        policy.dataRoot = context.dataRoot;
        return policy;
    }

    policy.dataRoot = executableDirectory / L"data";
    policy.legacyRoot = executableDirectory;
    policy.pendingMigrationStateRoot = executableDirectory;
    return policy;
}

inline bool EnsureDirectoryTree(
    const std::filesystem::path& directory) noexcept
{
    if (directory.empty())
        return false;

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    return std::filesystem::is_directory(directory, error) && !error;
}
}
