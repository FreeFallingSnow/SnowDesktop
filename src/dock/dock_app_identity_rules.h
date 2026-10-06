#pragma once

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

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

inline bool MatchesSquirrelVersionedExecutable(
    const std::wstring& launcherExecutablePath,
    const std::wstring& launcherAppUserModelId,
    const std::wstring& runningExecutablePath)
{
    // Squirrel's stable root stub exits after starting app-<version>\<same exe>.
    // Its explicit shortcut ID distinguishes this layout from arbitrary apps
    // with similarly named executables. All inputs are already normalized.
    constexpr std::wstring_view squirrelIdPrefix = L"COM.SQUIRREL.";
    if (!launcherAppUserModelId.starts_with(squirrelIdPrefix) ||
        launcherAppUserModelId.size() == squirrelIdPrefix.size())
        return false;

    const size_t separator = launcherExecutablePath.find_last_of(L'\\');
    if (separator == std::wstring::npos || separator <= 2 ||
        separator + 1 == launcherExecutablePath.size())
        return false;
    const std::wstring_view launcher(launcherExecutablePath);
    const std::wstring_view running(runningExecutablePath);
    if (!running.starts_with(launcher.substr(0, separator + 1)))
        return false;

    const auto relative = running.substr(separator + 1);
    const size_t versionEnd = relative.find(L'\\');
    if (versionEnd == std::wstring_view::npos ||
        relative.substr(versionEnd + 1) != launcher.substr(separator + 1))
        return false;
    const auto directory = relative.substr(0, versionEnd);
    if (!directory.starts_with(L"APP-") || directory.size() <= 4 ||
        directory[4] < L'0' || directory[4] > L'9')
        return false;
    // Include prerelease/build suffixes, but never nested paths or traversal.
    return std::all_of(directory.begin() + 4, directory.end(), [](wchar_t ch) {
        return (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') ||
            ch == L'.' || ch == L'-' || ch == L'+';
    });
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
            ancestorExecutablePaths) ||
            MatchesSquirrelVersionedExecutable(
                identityExecutablePath,
                identityAppUserModelId,
                runningExecutablePath);
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
