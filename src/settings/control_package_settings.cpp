#include "control_package_settings.h"
#include "control_settings_values.h"
#include "winui/settings_ipc_values.h"
#include <algorithm>

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;
using namespace winui;
const InstalledWidgetPackageSnapshot& Package(const Target& target, const WidgetsPageSnapshot& snapshot)
{
    if (target.id.empty()) throw Error("invalid_params", "package target requires an id", -32602);
    const auto id = values_detail::Wide(target.id);
    const auto found = std::find_if(snapshot.installed.begin(), snapshot.installed.end(), [&](const auto& value) { return value.packageId == id; });
    if (found == snapshot.installed.end()) throw Error("settings_target_not_found", "Component package no longer exists");
    return *found;
}
Value Identity(const WidgetsPageSnapshot& snapshot)
{
    return Object({{"generation", String(std::to_string(snapshot.generation))},
        {"installed", ToValue(snapshot.installed)}, {"agentSkills", ToValue(snapshot.agentSkills)}, {"agentSkillTargetMask", Number(snapshot.agentSkillTargetMask)}});
}
Value Values(const Target& target, const WidgetsPageSnapshot& snapshot, const Value& params)
{
    if (target.kind == "package") return ToValue(Package(target, snapshot));
    if (target.kind == "agent-skills") return Object({{"targets", ToValue(snapshot.agentSkills)},
        {"selectedMask", Number(snapshot.agentSkillTargetMask)}, {"error", String(values_detail::Utf8(snapshot.agentSkillStatusError))}});
    const auto offset = params.Find("offset") ? Integer(params, "offset", 0, 1000000) : 0;
    const auto limit = params.Find("limit") ? Integer(params, "limit", 1, 64) : 32;
    auto entries = Array();
    for (std::size_t index = static_cast<std::size_t>(offset); index < snapshot.installed.size() && entries.array.size() < static_cast<std::size_t>(limit); ++index)
        entries.array.push_back(ToValue(snapshot.installed[index]));
    return Object({{"installed", std::move(entries)}, {"offset", Number(offset)}, {"total", Number(static_cast<double>(snapshot.installed.size()))},
        {"agentSkills", ToValue(snapshot.agentSkills)}, {"developerOverridesVisible", Boolean(snapshot.developerOverridesVisible)}});
}
Value Response(const Target& target, const WidgetsPageSnapshot& snapshot, const Value& params)
{
    return Object({{"target", TargetJson(target)}, {"revision", String(Revision(Identity(snapshot)))}, {"values", Values(target, snapshot, params)},
        {"task", ToValue(snapshot.task)}, {"feedback", ToValue(snapshot.feedback)}});
}
bool Flag(const Value& arguments, std::string_view key)
{
    const auto* value = arguments.Find(key);
    if (!value || !value->IsBoolean()) throw Error("invalid_params", "Expected boolean: " + std::string(key), -32602);
    return value->boolean;
}
Value Arguments(const Value& params)
{
    const auto* value = params.Find("arguments");
    if (!value || !value->IsObject()) throw Error("invalid_params", "arguments must be a named object", -32602);
    return *value;
}
std::vector<std::wstring> Strings(const Value& arguments, const char* key)
{
    const auto* value = arguments.Find(key);
    if (!value || !value->IsArray() || value->array.size() > 256)
        throw Error("invalid_params", "Expected bounded string array", -32602);
    std::vector<std::wstring> result;
    for (const auto& item : value->array)
    {
        if (!item.IsString() || item.string.size() > 4096 || item.string.find('\0') != item.string.npos)
            throw Error("invalid_params", "Invalid permission/domain identity", -32602);
        const auto text = values_detail::Wide(item.string);
        if (std::find(result.begin(), result.end(), text) != result.end()) throw Error("invalid_params", "Duplicate permission/domain identity", -32602);
        result.push_back(text);
    }
    return result;
}
void PermissionRequest(WidgetsPageRequest& request, const InstalledWidgetPackageSnapshot& package, const Value& arguments)
{
    Fields(arguments, {"decision", "permissions", "networkDomains", "nativeAcknowledged", "confirmed"});
    const auto decision = Text(arguments, "decision");
    if (decision != "grant" && decision != "deny") throw Error("invalid_params", "decision must be grant or deny", -32602);
    request.command = WidgetsPageCommand::SetPermissionDecision;
    request.version = package.version; request.sourceId = package.sourceId;
    request.externalItemId = package.sourceExternalItemId; request.scopeFingerprint = package.permissionScopeFingerprint;
    request.permissionState = decision == "grant" ? WidgetPackagePermissionState::Granted : WidgetPackagePermissionState::Denied;
    if (decision == "grant")
    {
        if (!Flag(arguments, "confirmed")) throw Error("confirmation_required", "Review the exact package permissions before granting");
        request.grantedPermissions = Strings(arguments, "permissions");
        request.grantedNetworkDomains = Strings(arguments, "networkDomains");
        request.nativeAcknowledged = arguments.Find("nativeAcknowledged") && Flag(arguments, "nativeAcknowledged");
        for (const auto& id : request.grantedPermissions)
            if (std::none_of(package.permissions.begin(), package.permissions.end(), [&](const auto& permission) { return permission.id == id; }))
                throw Error("invalid_params", "Permission is not declared by this exact package", -32602);
        for (const auto& domain : request.grantedNetworkDomains)
            if (std::find(package.declaredNetworkDomains.begin(), package.declaredNetworkDomains.end(), domain) == package.declaredNetworkDomains.end())
                throw Error("invalid_params", "Network domain is not declared by this exact package", -32602);
    }
    else if (arguments.Find("permissions") || arguments.Find("networkDomains") || arguments.Find("nativeAcknowledged"))
        throw Error("invalid_params", "Deny does not accept grants", -32602);
}
}

bool IsPackageSettingsTarget(const Target& target) noexcept
{ return target.kind == "packages" || target.kind == "package" || target.kind == "agent-skills"; }

Value HandlePackageSettings(std::string_view method, const Value& params,
    const Target& target, const PackageSettingsContext& context)
{
    using namespace control;
    using namespace winui;
    if (!IsPackageSettingsTarget(target) || (target.kind != "package" && !target.id.empty()))
        throw Error("invalid_params", "Invalid component package settings target", -32602);
    const auto current = context.backend.Snapshot();
    if (!current || !context.backend.IsGenerationCurrent(context.generation) || current->generation != context.generation)
        throw Error("not_ready", "Component package settings backend is not active");
    const auto* package = target.kind == "package" ? &Package(target, *current) : nullptr;
    if (method == "settings.get" || method == "settings.catalog")
    {
        Fields(params, {"target", "offset", "limit"});
        auto response = Response(target, *current, params);
        if (method == "settings.catalog")
        {
            auto fields = Object(), actions = Array();
            if (package)
            {
                fields.object.emplace("enabled", Object({{"type", String("boolean")}, {"readOnly", Boolean(!package->canEnable)}}));
                fields.object.emplace("developmentOverrideActive", Object({{"type", String("boolean")}, {"readOnly", Boolean(!package->canUseDevelopmentOverride || !current->developerOverridesVisible)}}));
                actions.array.push_back(Object({{"name", String("permissions")}, {"requiresRevision", Boolean(true)},
                    {"arguments", Object({{"decision", String("grant|deny")}, {"permissions", Array()}, {"networkDomains", Array()},
                        {"nativeAcknowledged", Boolean(false)}, {"confirmed", Boolean(false)}})}}));
            }
            else if (target.kind == "agent-skills")
            {
                fields.object.emplace("selectedMask", Object({{"type", String("integer")}, {"minimum", Number(0)}, {"maximum", Number(63)}}));
                for (const auto* name : {"refresh", "apply-selection"}) actions.array.push_back(Object({{"name", String(name)}, {"effect", String(name == std::string_view("apply-selection") ? "installs selected user skill files" : "refreshes installed skill state")}}));
            }
            else actions.array.push_back(Object({{"name", String("refresh")}}));
            response.object.emplace("fields", std::move(fields)); response.object.emplace("actions", std::move(actions));
            response.object.emplace("updateLimit", Number(1));
            response.object.emplace("managementActionsIncluded", Boolean(false));
        }
        return response;
    }
    RequireRevision(params, Revision(Identity(*current)));
    if (current->task.kind != WidgetsPageTaskKind::None) throw Error("settings_busy", "Component packages have pending work");
    WidgetsPageRequest request;
    if (package) request.packageId = package->packageId;
    if (method == "settings.update")
    {
        Fields(params, {"target", "ifRevision", "changes"});
        const auto* changes = params.Find("changes");
        if (!changes || !changes->IsObject() || changes->object.size() != 1)
            throw Error("invalid_params", "Change one package preference at a time", -32602);
        if (package && changes->Find("enabled"))
        {
            if (!package->canEnable) throw Error("read_only_setting", "Package enablement is unavailable");
            request.command = WidgetsPageCommand::SetPackageEnabled; request.enabled = Flag(*changes, "enabled");
        }
        else if (package && changes->Find("developmentOverrideActive"))
        {
            if (!package->canUseDevelopmentOverride || !current->developerOverridesVisible)
                throw Error("settings_unavailable", "Development source override is unavailable");
            request.command = WidgetsPageCommand::SetDevelopmentOverride; request.enabled = Flag(*changes, "developmentOverrideActive");
        }
        else if (target.kind == "agent-skills" && changes->Find("selectedMask"))
        { request.command = WidgetsPageCommand::SetAgentSkillTargetSelection; request.agentSkillTargetMask = Integer(*changes, "selectedMask", 0, 63); }
        else throw Error("invalid_params", "Unknown or read-only package preference", -32602);
    }
    else if (method == "settings.action")
    {
        Fields(params, {"target", "ifRevision", "action", "arguments"});
        const auto action = Text(params, "action");
        if (package && action == "permissions") PermissionRequest(request, *package, Arguments(params));
        else if (target.kind == "packages" && action == "refresh")
        { const auto* arguments = params.Find("arguments"); if (arguments) Fields(*arguments, {}); request.command = WidgetsPageCommand::Refresh; }
        else if (target.kind == "agent-skills" && (action == "refresh" || action == "apply-selection"))
        {
            if (action == "apply-selection")
            { const auto arguments = Arguments(params); Fields(arguments, {"confirmed"}); if (!Flag(arguments, "confirmed")) throw Error("confirmation_required", "Confirm installing the selected Agent Skill files"); }
            else if (const auto* arguments = params.Find("arguments")) Fields(*arguments, {});
            request.command = action == "refresh" ? WidgetsPageCommand::RefreshAgentSkills : WidgetsPageCommand::ApplyAgentSkillSelection;
        }
        else throw Error("unknown_settings_action", "Unknown package settings action; package management is a separate capability", -32602);
    }
    else if (method == "settings.reset") throw Error("unsupported_setting", "Package permission and source decisions have no universal default; set them explicitly");
    else throw Error("method_not_found", "Unknown settings method", -32601);
    const bool accepted = context.backend.Invoke(context.generation, request);
    const auto next = context.backend.Snapshot();
    if (!accepted || !next) throw Error("package_settings_failed", next ? values_detail::Utf8(next->feedback.message) : "Package settings operation failed");
    auto response = Response(target, *next, Object());
    response.object.emplace("status", String(next->task.kind == WidgetsPageTaskKind::None ? "applied" : "started"));
    response.object.emplace("persistence", String(next->task.kind == WidgetsPageTaskKind::None ? "completed" : "pending"));
    return response;
}
}
