#include "widget_permission_state.h"

#include <array>

namespace snowdesktop::widget
{
namespace
{
constexpr std::array kPermissionDescriptors = {
    WidgetPermissionDescriptor{ "ui.input", PermissionRiskClass::Basic,
        "app.settings.widgets_permission_ui_input" },
    WidgetPermissionDescriptor{ "ui.contextMenu", PermissionRiskClass::Basic,
        "app.settings.widgets_permission_ui_context_menu" },
    WidgetPermissionDescriptor{ "system.performance.read",
        PermissionRiskClass::SystemStatus,
        "app.settings.widgets_permission_system_performance_read" },
    WidgetPermissionDescriptor{ "system.power.read",
        PermissionRiskClass::SystemStatus,
        "app.settings.widgets_permission_system_power_read" },
    WidgetPermissionDescriptor{ "system.storage.read",
        PermissionRiskClass::SystemStatus,
        "app.settings.widgets_permission_system_storage_read" },
    WidgetPermissionDescriptor{ "system.network.read",
        PermissionRiskClass::SystemStatus,
        "app.settings.widgets_permission_system_network_read" },
    WidgetPermissionDescriptor{ "system.display.read",
        PermissionRiskClass::SystemStatus,
        "app.settings.widgets_permission_system_display_read" },
    WidgetPermissionDescriptor{ "audio.output.read",
        PermissionRiskClass::SystemStatus,
        "app.settings.widgets_permission_audio_output_read" },
    WidgetPermissionDescriptor{ "media.read", PermissionRiskClass::PersonalData,
        "app.settings.widgets_permission_media_read" },
    WidgetPermissionDescriptor{ "desktop.read",
        PermissionRiskClass::PersonalData,
        "app.settings.widgets_permission_desktop_read" },
    WidgetPermissionDescriptor{ "app.discovery",
        PermissionRiskClass::PersonalData,
        "app.settings.widgets_permission_app_discovery" },
    WidgetPermissionDescriptor{ "calendar.read",
        PermissionRiskClass::PersonalData,
        "app.settings.widgets_permission_calendar_read" },
    WidgetPermissionDescriptor{ "network.http",
        PermissionRiskClass::ExternalCommunication,
        "app.settings.widgets_permission_network_http" },
    WidgetPermissionDescriptor{ "network.internet",
        PermissionRiskClass::ExternalCommunication,
        "app.settings.widgets_permission_network_internet" },
    WidgetPermissionDescriptor{ "ui.notify",
        PermissionRiskClass::ExternalCommunication,
        "app.settings.widgets_permission_ui_notify" },
    WidgetPermissionDescriptor{ "notification.post",
        PermissionRiskClass::ExternalCommunication,
        "app.settings.widgets_permission_notification_post" },
    WidgetPermissionDescriptor{ "network.local",
        PermissionRiskClass::ElevatedRead,
        "app.settings.widgets_permission_network_local" },
    WidgetPermissionDescriptor{ "everything.search",
        PermissionRiskClass::ElevatedRead,
        "app.settings.widgets_permission_everything_search" },
    WidgetPermissionDescriptor{ "clipboard.read",
        PermissionRiskClass::ElevatedRead,
        "app.settings.widgets_permission_clipboard_read" },
    WidgetPermissionDescriptor{ "process.summary.read",
        PermissionRiskClass::ElevatedRead,
        "app.settings.widgets_permission_process_summary_read" },
    WidgetPermissionDescriptor{ "audio.output.analyze",
        PermissionRiskClass::ElevatedRead,
        "app.settings.widgets_permission_audio_output_analyze" },
    WidgetPermissionDescriptor{ "desktop.action",
        PermissionRiskClass::Modification,
        "app.settings.widgets_permission_desktop_action" },
    WidgetPermissionDescriptor{ "app.launch", PermissionRiskClass::Modification,
        "app.settings.widgets_permission_app_launch" },
    WidgetPermissionDescriptor{ "shell.launch",
        PermissionRiskClass::Modification,
        "app.settings.widgets_permission_shell_launch" },
    WidgetPermissionDescriptor{ "calendar.write",
        PermissionRiskClass::Modification,
        "app.settings.widgets_permission_calendar_write" },
    WidgetPermissionDescriptor{ "media.action",
        PermissionRiskClass::Modification,
        "app.settings.widgets_permission_media_action" },
    WidgetPermissionDescriptor{ "audio.output.control",
        PermissionRiskClass::Modification,
        "app.settings.widgets_permission_audio_output_control" },
    WidgetPermissionDescriptor{ "clipboard.write",
        PermissionRiskClass::Modification,
        "app.settings.widgets_permission_clipboard_write" },
    WidgetPermissionDescriptor{ "filesystem.userSelected.read",
        PermissionRiskClass::UserScoped,
        "app.settings.widgets_permission_filesystem_user_selected_read" },
    WidgetPermissionDescriptor{ "filesystem.userSelected.write",
        PermissionRiskClass::UserScoped,
        "app.settings.widgets_permission_filesystem_user_selected_write" },
    WidgetPermissionDescriptor{ "filesystem.userSelected.watch",
        PermissionRiskClass::UserScoped,
        "app.settings.widgets_permission_filesystem_user_selected_watch" },
};
}

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
    if (const auto* descriptor =
            FindWidgetPermissionDescriptor(permission))
        return descriptor->risk;
    if (permission == "system.read")
        return PermissionRiskClass::SystemStatus;
    if (permission == "audio.microphone.capture" ||
        permission == "camera.capture" || permission == "location.read")
        return PermissionRiskClass::Sensor;

    return PermissionRiskClass::Unknown;
}

std::span<const WidgetPermissionDescriptor>
WidgetPermissionDescriptors() noexcept
{
    return kPermissionDescriptors;
}

const WidgetPermissionDescriptor* FindWidgetPermissionDescriptor(
    std::string_view permission) noexcept
{
    for (const auto& descriptor : kPermissionDescriptors)
        if (descriptor.id == permission) return &descriptor;
    return nullptr;
}

bool IsKnownWidgetPermission(std::string_view permission) noexcept
{
    return FindWidgetPermissionDescriptor(permission) != nullptr;
}

const char* WidgetPermissionLabelLocalizationKey(
    std::string_view permission) noexcept
{
    if (const auto* descriptor =
            FindWidgetPermissionDescriptor(permission))
        return descriptor->labelLocalizationKey;
    if (permission == "system.read")
        return "app.settings.widgets_permission_system_read";
    return nullptr;
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
