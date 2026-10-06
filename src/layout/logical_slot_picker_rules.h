#pragma once

#include <algorithm>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::logical_slot_picker_rules
{

inline bool Accepts(const std::vector<std::string>& accepts,
    std::string_view kind) noexcept
{
    return std::find(accepts.begin(), accepts.end(), kind) != accepts.end();
}

inline bool MatchesType(std::string_view requiredType,
    std::string_view candidateType) noexcept
{
    return requiredType.empty() || requiredType == candidateType;
}

inline bool IsFilesystemApplicationLaunchTarget(
    std::wstring_view target) noexcept
{
    const bool hasDrivePrefix = target.size() >= 2 && target[1] == L':';
    const bool hasUncPrefix = target.starts_with(L"\\\\");
    return hasDrivePrefix || hasUncPrefix;
}

inline std::wstring NormalizeApplicationLaunchTarget(
    std::wstring_view parsingName)
{
    if (parsingName.empty()) return {};
    std::wstring launchTarget(parsingName);
    const bool hasShellPrefix = launchTarget.size() >= 6 &&
        _wcsnicmp(launchTarget.c_str(), L"shell:", 6) == 0;
    const bool hasNamespacePrefix = launchTarget.starts_with(L"::");
    const bool hasFilesystemTarget =
        IsFilesystemApplicationLaunchTarget(launchTarget);
    if (!hasShellPrefix && !hasNamespacePrefix &&
        !hasFilesystemTarget)
    {
        launchTarget = L"shell:AppsFolder\\" + launchTarget;
    }
    return launchTarget;
}

inline std::string_view DesktopCandidateKind(
    const std::vector<std::string>& accepts, bool applicationShortcut,
    bool hasFilesystemTarget) noexcept
{
    if (applicationShortcut && Accepts(accepts, "app.reference"))
        return "app.reference";
    if (Accepts(accepts, "desktop.item"))
        return "desktop.item";
    if (hasFilesystemTarget &&
        Accepts(accepts, "filesystem.reference"))
        return "filesystem.reference";
    return {};
}

} // namespace snowdesktop::logical_slot_picker_rules
