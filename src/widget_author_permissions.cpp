#include "widget_author_permissions.h"

#include "widget_api_registry.h"
#include "widget_permission_broker.h"
#include "widget_permission_state.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_authoring
{
namespace
{
std::string JsonString(std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20)
            {
                result += "\\u00";
                result.push_back(hex[character >> 4]);
                result.push_back(hex[character & 0x0f]);
            }
            else result.push_back(static_cast<char>(character));
            break;
        }
    }
    result.push_back('"');
    return result;
}

const char* RiskName(snowdesktop::widget::PermissionRiskClass risk)
{
    using snowdesktop::widget::PermissionRiskClass;
    switch (risk)
    {
    case PermissionRiskClass::Basic: return "basic";
    case PermissionRiskClass::SystemStatus: return "systemStatus";
    case PermissionRiskClass::PersonalData: return "personalData";
    case PermissionRiskClass::ExternalCommunication:
        return "externalCommunication";
    case PermissionRiskClass::ElevatedRead: return "elevatedRead";
    case PermissionRiskClass::Modification: return "modification";
    case PermissionRiskClass::UserScoped: return "userScoped";
    case PermissionRiskClass::Sensor: return "sensor";
    case PermissionRiskClass::Unknown: return "unknown";
    }
    return "unknown";
}

struct Capability
{
    std::string kind;
    std::string id;
};

std::map<std::string, std::vector<Capability>> PermissionCapabilities()
{
    std::map<std::string, std::vector<Capability>> result;
    for (const auto& contract :
        snowdesktop::widget_api::PublicApiFunctionContracts())
    {
        if (!contract.requiredPermission ||
            !*contract.requiredPermission)
            continue;
        result[contract.requiredPermission].push_back({
            "function", std::string(contract.library) + "." +
                contract.name });
    }
    for (const auto& contract :
        snowdesktop::widget_api::SystemDataTopicContracts())
    {
        if (!contract.requiredPermission ||
            !*contract.requiredPermission)
            continue;
        result[contract.requiredPermission].push_back(
            { "data", contract.name });
    }
    for (const auto& contract :
        snowdesktop::widget_api::SystemTaskContracts())
    {
        if (!contract.requiredPermission ||
            !*contract.requiredPermission)
            continue;
        result[contract.requiredPermission].push_back(
            { "task", contract.name });
    }
    for (auto& [permission, capabilities] : result)
    {
        (void)permission;
        std::sort(capabilities.begin(), capabilities.end(),
            [](const Capability& left, const Capability& right) {
                return std::tie(left.kind, left.id) <
                    std::tie(right.kind, right.id);
            });
        capabilities.erase(std::unique(capabilities.begin(),
            capabilities.end(), [](const Capability& left,
                const Capability& right) {
                return left.kind == right.kind && left.id == right.id;
            }), capabilities.end());
    }
    return result;
}

void WriteCapabilities(std::ostringstream& output,
    const std::vector<Capability>& capabilities)
{
    output << '[';
    for (std::size_t index = 0; index < capabilities.size(); ++index)
    {
        if (index) output << ',';
        output << "{\"kind\":" << JsonString(capabilities[index].kind)
            << ",\"id\":" << JsonString(capabilities[index].id) << '}';
    }
    output << ']';
}
} // namespace

PermissionReport BuildPermissionReport(
    const snowdesktop::widget::PackageManifest& manifest)
{
    PermissionReport report;
    const auto capabilities = PermissionCapabilities();
    std::vector<std::pair<std::string, bool>> declarations;
    declarations.reserve(manifest.permissions.size() +
        manifest.optionalPermissions.size());
    for (const auto& permission : manifest.permissions)
        declarations.emplace_back(permission, true);
    for (const auto& permission : manifest.optionalPermissions)
        declarations.emplace_back(permission, false);
    std::sort(declarations.begin(), declarations.end());

    bool requiresConsent = false;
    std::ostringstream output;
    output << "{\"ok\":true,\"schemaVersion\":1,\"packageId\":"
        << JsonString(manifest.id)
        << ",\"scopeFingerprint\":"
        << JsonString(snowdesktop::widget::WidgetPermissionBroker::
            ScopeFingerprint(manifest.permissions,
                manifest.optionalPermissions,
                manifest.networkDomains))
        << ",\"permissions\":[";
    for (std::size_t index = 0; index < declarations.size(); ++index)
    {
        if (index) output << ',';
        const auto& [id, required] = declarations[index];
        const auto risk = snowdesktop::widget::ClassifyPermissionRisk(id);
        const bool consent =
            snowdesktop::widget::PermissionRequiresConsent(id);
        requiresConsent = requiresConsent || consent;
        output << "{\"id\":" << JsonString(id)
            << ",\"required\":" << (required ? "true" : "false")
            << ",\"risk\":" << JsonString(RiskName(risk))
            << ",\"requiresConsent\":"
            << (consent ? "true" : "false")
            << ",\"capabilities\":";
        if (const auto found = capabilities.find(id);
            found != capabilities.end())
            WriteCapabilities(output, found->second);
        else
            output << "[]";
        output << '}';
    }
    output << "],\"network\":{\"domains\":[";
    std::vector<std::string> domains = manifest.networkDomains;
    std::sort(domains.begin(), domains.end());
    for (std::size_t index = 0; index < domains.size(); ++index)
    {
        if (index) output << ',';
        output << JsonString(domains[index]);
    }
    const auto hasPermission = [&](std::string_view permission) {
        return std::find(manifest.permissions.begin(),
                manifest.permissions.end(), permission) !=
                manifest.permissions.end() ||
            std::find(manifest.optionalPermissions.begin(),
                manifest.optionalPermissions.end(), permission) !=
                manifest.optionalPermissions.end();
    };
    output << "],\"internet\":"
        << (hasPermission("network.internet") ||
            hasPermission("network.http") ? "true" : "false")
        << ",\"local\":"
        << (hasPermission("network.local") ? "true" : "false")
        << "},\"requiresConsent\":"
        << (requiresConsent ? "true" : "false") << '}';
    report.ok = true;
    report.json = output.str();
    return report;
}

} // namespace snowdesktop::widget_authoring
