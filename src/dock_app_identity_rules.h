#pragma once

#include <algorithm>
#include <span>
#include <string>

enum class DockAppIdentityKind
{
    None,
    Executable,
    Applications,
    Steam,
};

namespace snowdesktop::dock_app_identity_rules
{

inline bool IsPathInsideDirectory(
    const std::wstring& path, const std::wstring& directory)
{
    if (path.empty() || directory.empty() ||
        path.size() <= directory.size() ||
        path.compare(0, directory.size(), directory) != 0)
        return false;
    return directory.back() == L'\\' ||
        path[directory.size()] == L'\\';
}

inline bool MatchesExecutableProcessFamily(
    const std::wstring& launcherExecutablePath,
    const std::wstring& runningExecutablePath,
    std::span<const std::wstring> ancestorExecutablePaths)
{
    if (launcherExecutablePath.empty() ||
        runningExecutablePath.empty())
        return false;
    if (launcherExecutablePath == runningExecutablePath)
        return true;

    const size_t separator =
        launcherExecutablePath.find_last_of(L'\\');
    if (separator == std::wstring::npos || separator <= 2)
        return false;
    const std::wstring installDirectory =
        launcherExecutablePath.substr(0, separator);
    if (!IsPathInsideDirectory(
            runningExecutablePath, installDirectory))
        return false;
    return std::find(
        ancestorExecutablePaths.begin(),
        ancestorExecutablePaths.end(),
        launcherExecutablePath) !=
        ancestorExecutablePaths.end();
}

inline bool MatchesRunningApp(
    DockAppIdentityKind kind,
    const std::wstring& identityExecutablePath,
    const std::wstring& identityAppUserModelId,
    const std::wstring& steamInstallDirectory,
    const std::wstring& runningExecutablePath,
    const std::wstring& runningAppUserModelId,
    std::span<const std::wstring>
        ancestorExecutablePaths = {})
{
    switch (kind)
    {
    case DockAppIdentityKind::Executable:
        return MatchesExecutableProcessFamily(
            identityExecutablePath,
            runningExecutablePath,
            ancestorExecutablePaths);
    case DockAppIdentityKind::Applications:
        return !identityAppUserModelId.empty() &&
            identityAppUserModelId == runningAppUserModelId;
    case DockAppIdentityKind::Steam:
        return (!identityAppUserModelId.empty() &&
                identityAppUserModelId == runningAppUserModelId) ||
            IsPathInsideDirectory(
                runningExecutablePath,
                steamInstallDirectory);
    default:
        return false;
    }
}

} // namespace snowdesktop::dock_app_identity_rules
