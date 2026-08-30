#pragma once

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

inline bool MatchesRunningApp(
    DockAppIdentityKind kind,
    const std::wstring& identityExecutablePath,
    const std::wstring& identityAppUserModelId,
    const std::wstring& steamInstallDirectory,
    const std::wstring& runningExecutablePath,
    const std::wstring& runningAppUserModelId)
{
    switch (kind)
    {
    case DockAppIdentityKind::Executable:
        return !identityExecutablePath.empty() &&
            identityExecutablePath == runningExecutablePath;
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
