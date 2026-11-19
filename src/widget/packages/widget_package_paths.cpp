#include "widget_package_paths.h"

#include "widget_package.h"

#include "data/data_paths.h"

#include <windows.h>

namespace snowdesktop::widget
{
std::optional<std::wstring> ResolvePackageAssetWithinRoot(
    const std::filesystem::path& packageRoot,
    const std::wstring& relativePath)
{
    if (!snowdesktop::widget::WidgetPackageValidator::IsSafeRelativePath(
        std::filesystem::path(relativePath)))
        return std::nullopt;
    if (packageRoot.empty())
        return std::nullopt;
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        packageRoot, error);
    if (error) return std::nullopt;
    const DWORD rootAttributes = GetFileAttributesW(root.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES ||
        (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return std::nullopt;
    const auto target = std::filesystem::weakly_canonical(
        root / relativePath, error);
    if (error) return std::nullopt;
    auto rootIt = root.begin();
    auto targetIt = target.begin();
    for (; rootIt != root.end(); ++rootIt, ++targetIt)
    {
        if (targetIt == target.end() ||
            _wcsicmp(rootIt->c_str(), targetIt->c_str()) != 0)
            return std::nullopt;
    }
    for (auto current = root; current != target; )
    {
        // Check each path component that already exists. Missing assets simply
        // fail at the image decoder without escaping the package.
        const auto relative = std::filesystem::relative(target, current, error);
        if (error || relative.empty()) break;
        current /= *relative.begin();
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return std::nullopt;
    }
    return target.wstring();
}

PackagePaths PackagePaths::ForCurrentDeployment()
{
    const std::filesystem::path executable(GetExecutableDirectoryPath());
    const std::filesystem::path data(GetDataDirectoryPath());
    PackagePaths paths;
    paths.builtin = executable / L"widgets";
    paths.installed = data / L"widgets" / L"installed";
    paths.development = data / L"widgets" / L"dev";
    paths.staging = data / L"widgets" / L"staging";
    paths.quarantine = data / L"widgets" / L"quarantine";
    paths.migrations = data / L"widgets" / L"migrations";
    paths.registry = data / L"widgets" / L"packages.json";
    return paths;
}
}
