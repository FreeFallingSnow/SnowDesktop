#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget
{
enum class PermissionDecisionState
{
    LegacyImplicit,
    Pending,
    Granted,
    Denied,
};

const char* PermissionDecisionStateName(
    PermissionDecisionState state) noexcept;
std::optional<PermissionDecisionState> ParsePermissionDecisionState(
    std::string_view value) noexcept;
bool PermissionStateAllowsRuntime(
    PermissionDecisionState state) noexcept;

std::vector<std::string> ResolveGrantedScopes(
    PermissionDecisionState state,
    std::span<const std::string> declared,
    std::span<const std::string> stored);
}
