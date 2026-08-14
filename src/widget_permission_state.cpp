#include "widget_permission_state.h"

namespace snowdesktop::widget
{
const char* PermissionDecisionStateName(
    PermissionDecisionState state) noexcept
{
    switch (state)
    {
    case PermissionDecisionState::LegacyImplicit:
        return "legacyImplicit";
    case PermissionDecisionState::Pending:
        return "pending";
    case PermissionDecisionState::Granted:
        return "granted";
    case PermissionDecisionState::Denied:
        return "denied";
    }
    return "pending";
}

std::optional<PermissionDecisionState> ParsePermissionDecisionState(
    std::string_view value) noexcept
{
    if (value == "legacyImplicit")
        return PermissionDecisionState::LegacyImplicit;
    if (value == "pending")
        return PermissionDecisionState::Pending;
    if (value == "granted")
        return PermissionDecisionState::Granted;
    if (value == "denied")
        return PermissionDecisionState::Denied;
    return std::nullopt;
}

bool PermissionStateAllowsRuntime(
    PermissionDecisionState state) noexcept
{
    return PermissionRuntimeBlockFor(state) ==
        PermissionRuntimeBlock::None;
}

PermissionRuntimeBlock PermissionRuntimeBlockFor(
    PermissionDecisionState state) noexcept
{
    switch (state)
    {
    case PermissionDecisionState::LegacyImplicit:
    case PermissionDecisionState::Granted:
        return PermissionRuntimeBlock::None;
    case PermissionDecisionState::Pending:
        return PermissionRuntimeBlock::PendingConsent;
    case PermissionDecisionState::Denied:
        return PermissionRuntimeBlock::Denied;
    }
    return PermissionRuntimeBlock::PendingConsent;
}

std::vector<std::string> ResolveGrantedScopes(
    PermissionDecisionState state,
    std::span<const std::string> declared,
    std::span<const std::string> stored)
{
    if (state == PermissionDecisionState::Pending ||
        state == PermissionDecisionState::Denied)
    {
        return {};
    }
    if (state == PermissionDecisionState::LegacyImplicit && stored.empty())
        return { declared.begin(), declared.end() };
    return { stored.begin(), stored.end() };
}
}
