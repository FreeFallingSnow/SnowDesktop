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

PermissionRiskClass ClassifyPermissionRisk(
    std::string_view permission) noexcept
{
    if (permission == "ui.input" || permission == "ui.contextMenu")
        return PermissionRiskClass::Basic;

    if (permission == "system.read" ||
        permission == "system.performance.read" ||
        permission == "system.power.read" ||
        permission == "system.storage.read" ||
        permission == "system.network.read" ||
        permission == "system.display.read" ||
        permission == "audio.output.read")
        return PermissionRiskClass::SystemStatus;

    if (permission == "media.read" || permission == "desktop.read" ||
        permission == "app.discovery" || permission == "calendar.read")
        return PermissionRiskClass::PersonalData;

    if (permission == "network.http" ||
        permission == "network.internet" ||
        permission == "ui.notify" || permission == "notification.post")
        return PermissionRiskClass::ExternalCommunication;

    if (permission == "network.local" ||
        permission == "everything.search" ||
        permission == "clipboard.read" ||
        permission == "process.summary.read" ||
        permission == "audio.output.analyze")
        return PermissionRiskClass::ElevatedRead;

    if (permission == "desktop.action" || permission == "app.launch" ||
        permission == "shell.launch" || permission == "calendar.write" ||
        permission == "media.action" ||
        permission == "audio.output.control" ||
        permission == "clipboard.write")
        return PermissionRiskClass::Modification;

    if (permission == "filesystem.userSelected.read" ||
        permission == "filesystem.userSelected.write" ||
        permission == "filesystem.userSelected.watch")
        return PermissionRiskClass::UserScoped;

    if (permission == "audio.microphone.capture" ||
        permission == "camera.capture" || permission == "location.read")
        return PermissionRiskClass::Sensor;

    return PermissionRiskClass::Unknown;
}

bool PermissionRequiresConsent(std::string_view permission) noexcept
{
    return ClassifyPermissionRisk(permission) != PermissionRiskClass::Basic;
}

std::vector<std::string> PermissionsRequiringConsent(
    std::span<const std::string> permissions)
{
    std::vector<std::string> result;
    result.reserve(permissions.size());
    for (const auto& permission : permissions)
        if (PermissionRequiresConsent(permission))
            result.push_back(permission);
    return result;
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
