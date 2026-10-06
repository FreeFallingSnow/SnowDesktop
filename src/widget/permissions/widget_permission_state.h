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

enum class PermissionRuntimeBlock
{
    None,
    PendingConsent,
    Denied,
    MissingRequired,
};

enum class PermissionRiskClass
{
    Basic,
    SystemStatus,
    PersonalData,
    ExternalCommunication,
    ElevatedRead,
    Modification,
    UserScoped,
    Sensor,
    Unknown,
};

struct WidgetPermissionDescriptor
{
    std::string_view id;
    PermissionRiskClass risk = PermissionRiskClass::Unknown;
    const char* labelLocalizationKey = nullptr;
};

const char* PermissionDecisionStateName(
    PermissionDecisionState state) noexcept;
std::optional<PermissionDecisionState> ParsePermissionDecisionState(
    std::string_view value) noexcept;
bool PermissionStateAllowsRuntime(
    PermissionDecisionState state) noexcept;
PermissionRuntimeBlock PermissionRuntimeBlockFor(
    PermissionDecisionState state) noexcept;

PermissionRiskClass ClassifyPermissionRisk(
    std::string_view permission) noexcept;
std::span<const WidgetPermissionDescriptor>
WidgetPermissionDescriptors() noexcept;
const WidgetPermissionDescriptor* FindWidgetPermissionDescriptor(
    std::string_view permission) noexcept;
bool IsKnownWidgetPermission(std::string_view permission) noexcept;
const char* WidgetPermissionLabelLocalizationKey(
    std::string_view permission) noexcept;
bool PermissionRequiresConsent(std::string_view permission) noexcept;
std::vector<std::string> PermissionsRequiringConsent(
    std::span<const std::string> permissions);

std::vector<std::string> ResolveGrantedScopes(
    PermissionDecisionState state,
    std::span<const std::string> declared,
    std::span<const std::string> stored);
}
