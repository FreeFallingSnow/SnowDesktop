#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace snowdesktop::widget
{
std::optional<std::wstring> ResolvePackageAssetWithinRoot(
    const std::filesystem::path& packageRoot,
    const std::wstring& relativePath);
}
