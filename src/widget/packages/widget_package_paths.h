#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace snowdesktop::widget
{
struct PackagePaths
{
    std::filesystem::path builtin;
    std::filesystem::path installed;
    std::filesystem::path development;
    std::filesystem::path staging;
    std::filesystem::path quarantine;
    std::filesystem::path migrations;
    std::filesystem::path registry;

    static PackagePaths ForCurrentDeployment();
};

std::optional<std::wstring> ResolvePackageAssetWithinRoot(
    const std::filesystem::path& packageRoot,
    const std::wstring& relativePath);
}
