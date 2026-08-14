#pragma once

#include "widget_permission_state.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace snowdesktop::widget
{
struct PermissionGrantSnapshot
{
    PermissionRuntimeBlock runtimeBlock =
        PermissionRuntimeBlock::PendingConsent;
    std::vector<std::string> permissions;
    std::vector<std::string> networkDomains;
    std::string requestedScopeFingerprint;
};

class WidgetPermissionBroker
{
public:
    static PermissionGrantSnapshot Evaluate(
        PermissionDecisionState state,
        std::span<const std::string> declaredPermissions,
        std::span<const std::string> declaredNetworkDomains,
        std::span<const std::string> storedGrantedPermissions,
        std::span<const std::string> storedGrantedNetworkDomains);
    static PermissionGrantSnapshot Evaluate(
        PermissionDecisionState state,
        std::span<const std::string> requiredPermissions,
        std::span<const std::string> optionalPermissions,
        std::span<const std::string> declaredNetworkDomains,
        std::span<const std::string> storedGrantedPermissions,
        std::span<const std::string> storedGrantedNetworkDomains);

    static PermissionRuntimeBlock ActivationBlock(
        PermissionDecisionState state) noexcept;
    static bool AllowsPermission(
        std::span<const std::string> grantedPermissions,
        std::string_view permission) noexcept;
    static bool AllowsPermission(
        const std::unordered_set<std::string>& grantedPermissions,
        std::string_view permission);
    static bool AllowsNetworkDomain(
        std::span<const std::string> grantedNetworkDomains,
        std::string_view domain) noexcept;
    static std::string ScopeFingerprint(
        std::span<const std::string> declaredPermissions,
        std::span<const std::string> declaredNetworkDomains);
    static std::string ScopeFingerprint(
        std::span<const std::string> requiredPermissions,
        std::span<const std::string> optionalPermissions,
        std::span<const std::string> declaredNetworkDomains);
};
}
