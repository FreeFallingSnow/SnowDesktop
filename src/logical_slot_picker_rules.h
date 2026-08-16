#pragma once

#include <algorithm>
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
