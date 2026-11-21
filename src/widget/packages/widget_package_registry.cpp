#include "widget_package_manager.h"
#include "widget_package_manifest_fields.h"
#include "widget_package_read.h"

#include "common/json_value.h"
#include "data/atomic_file.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace snowdesktop::widget
{
using detail::DecimalIdentifier;
using detail::HasReparsePoint;
using detail::JsonEscape;
using detail::ReadPackageFile;
using detail::ReadString;
using detail::ReadStringArray;
using detail::Timestamp;
using detail::Utf8ToWide;

bool WidgetPackageManager::Registry::IsEnabled(const std::string& packageId) const
{
    const auto installed = entries.find(packageId);
    return installed != entries.end() ? installed->second.enabled
        : !disabledDevelopmentIds.contains(packageId);
}

bool WidgetPackageManager::Registry::Load(const PackagePaths& paths,
    const WidgetPackageValidator& validator, std::string& error)
{
    entries.clear();
    permissionDecisions.clear();
    developmentOverrides.clear();
    knownDevelopmentIds.clear();
    disabledDevelopmentIds.clear();
    steamSubscriptionsByAccount.clear();
    steamLibrarySubscriptionsByAccount.clear();
    std::error_code ec;
    if (!std::filesystem::exists(paths.registry, ec)) return true;
    std::string text;
    if (!ReadPackageFile(paths.registry, text, 4 * 1024 * 1024))
    {
        error = "cannot read package registry";
        return false;
    }
    JsonValue root;
    std::string parseError;
    if (!ParseJson(text, root, &parseError) || !root.IsObject())
    {
        const auto quarantine = paths.quarantine /
            (L"packages-" + Utf8ToWide(Timestamp()) + L".invalid.json");
        std::filesystem::rename(paths.registry, quarantine, ec);
        // The registry is derived state. Quarantine it and rebuild from package
        // directories so a damaged JSON file cannot hide built-in packages.
        error.clear();
        return true;
    }
    const JsonValue* packages = root.Find("packages");
    if (packages && packages->IsArray())
    {
        for (const auto& value : packages->array)
        {
            if (!value.IsObject()) continue;
            RegistryEntry entry;
            ReadString(value, "packageId", entry.packageId);
            ReadString(value, "activeVersion", entry.activeVersion);
            ReadString(value, "providerId", entry.source.providerId);
            ReadString(value, "externalItemId", entry.source.externalItemId);
            if (const JsonValue* permissionState =
                    value.Find("permissionState");
                permissionState)
            {
                const auto parsed = permissionState->IsString()
                    ? ParsePermissionDecisionState(permissionState->string)
                    : std::nullopt;
                entry.permissionState = parsed.value_or(
                    PermissionDecisionState::Pending);
            }
            bool arraysValid = true;
            entry.grantedPermissions =
                ReadStringArray(value, "grantedPermissions", arraysValid);
            entry.grantedNetworkDomains =
                ReadStringArray(value, "grantedNetworkDomains", arraysValid);
            if (const JsonValue* enabled = value.Find("enabled");
                enabled && enabled->IsBoolean())
                entry.enabled = enabled->boolean;
            if (WidgetPackageValidator::IsUuid(entry.packageId) &&
                WidgetPackageValidator::IsSemVer(entry.activeVersion))
                entries[entry.packageId] = std::move(entry);
        }
    }
    if (const JsonValue* decisions = root.Find("permissionDecisions");
        decisions && decisions->IsArray())
    {
        for (const auto& value : decisions->array)
        {
            if (!value.IsObject()) continue;
            PermissionDecisionRecord record;
            ReadString(value, "packageId", record.packageId);
            ReadString(value, "providerId", record.source.providerId);
            ReadString(value, "externalItemId",
                record.source.externalItemId);
            if (const JsonValue* state = value.Find("state"); state)
            {
                const auto parsed = state->IsString()
                    ? ParsePermissionDecisionState(state->string)
                    : std::nullopt;
                record.state = parsed.value_or(
                    PermissionDecisionState::Pending);
            }
            bool arraysValid = true;
            record.requestedPermissions =
                ReadStringArray(value, "requestedPermissions", arraysValid);
            record.requestedOptionalPermissions =
                ReadStringArray(value, "requestedOptionalPermissions",
                    arraysValid);
            record.requestedNetworkDomains =
                ReadStringArray(value, "requestedNetworkDomains", arraysValid);
            ReadString(value, "requestedScopeFingerprint",
                record.requestedScopeFingerprint);
            record.grantedPermissions =
                ReadStringArray(value, "grantedPermissions", arraysValid);
            record.grantedNetworkDomains =
                ReadStringArray(value, "grantedNetworkDomains", arraysValid);
            if (!WidgetPackageValidator::IsUuid(record.packageId) ||
                record.source.providerId.empty() ||
                record.source.externalItemId.empty())
                continue;
            const std::string key = record.packageId + "\n" +
                record.source.providerId + "\n" +
                record.source.externalItemId;
            permissionDecisions[key] = std::move(record);
        }
    }
    if (const JsonValue* overrides = root.Find("developmentOverrides");
        overrides && overrides->IsArray())
    {
        for (const auto& value : overrides->array)
            if (value.IsString() &&
                WidgetPackageValidator::IsUuid(value.string))
                developmentOverrides.insert(value.string);
    }
    if (const JsonValue* known = root.Find("knownDevelopmentIds"); known && known->IsArray())
    {
        for (const auto& value : known->array)
            if (value.IsString() && WidgetPackageValidator::IsUuid(value.string))
                knownDevelopmentIds.insert(value.string);
    }
    else
    {
        // Older registries did not distinguish untouched candidates from a
        // deliberate opt-out. Preserve every existing candidate's selection.
        std::error_code scanError;
        for (std::filesystem::directory_iterator it(paths.development, scanError), end;
            !scanError && it != end; it.increment(scanError))
        {
            if (!it->is_directory(scanError) || HasReparsePoint(it->path())) continue;
            PackageManifest manifest;
            if (validator.ValidateDirectory(it->path(), &manifest).Ok())
                knownDevelopmentIds.insert(manifest.id);
        }
    }
    if (const JsonValue* disabled = root.Find("disabledDevelopmentIds");
        disabled && disabled->IsArray())
    {
        for (const auto& value : disabled->array)
            if (value.IsString() && WidgetPackageValidator::IsUuid(value.string))
                disabledDevelopmentIds.insert(value.string);
    }
    if (const JsonValue* history = root.Find(
            "steamSubscriptionsByAccount");
        history && history->IsObject())
    {
        for (const auto& [accountId, value] : history->object)
        {
            if (!DecimalIdentifier(accountId) || !value.IsArray()) continue;
            auto& subscriptions = steamSubscriptionsByAccount[accountId];
            for (const auto& item : value.array)
                if (item.IsString() && DecimalIdentifier(item.string))
                    subscriptions.insert(item.string);
        }
    }
    if (const JsonValue* history = root.Find("steamLibrarySubscriptionsByAccount");
        history && history->IsObject())
    {
        for (const auto& [accountId, libraries] : history->object)
        {
            if (!DecimalIdentifier(accountId) || !libraries.IsObject()) continue;
            auto& account = steamLibrarySubscriptionsByAccount[accountId];
            for (const auto& [library, values] : libraries.object)
            {
                if (library.empty() || !values.IsArray()) continue;
                auto& subscriptions = account[library];
                for (const auto& value : values.array)
                    if (value.IsString() && DecimalIdentifier(value.string))
                        subscriptions.push_back(value.string);
                std::sort(subscriptions.begin(), subscriptions.end());
                subscriptions.erase(std::unique(subscriptions.begin(),
                    subscriptions.end()), subscriptions.end());
            }
        }
    }
    return true;
}

bool WidgetPackageManager::Registry::Save(const std::filesystem::path& path,
    std::string& error) const
{
    std::vector<const RegistryEntry*> orderedEntries;
    orderedEntries.reserve(entries.size());
    for (const auto& [id, entry] : entries) orderedEntries.push_back(&entry);
    std::sort(orderedEntries.begin(), orderedEntries.end(),
        [](const auto* a, const auto* b) { return a->packageId < b->packageId; });
    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 1,\n  \"packages\": [";
    for (std::size_t i = 0; i < orderedEntries.size(); ++i)
    {
        if (i) out << ',';
        const auto& entry = *orderedEntries[i];
        out << "\n    {\"packageId\":\"" << JsonEscape(entry.packageId)
            << "\",\"activeVersion\":\"" << JsonEscape(entry.activeVersion)
            << "\",\"providerId\":\"" << JsonEscape(entry.source.providerId)
            << "\",\"externalItemId\":\""
            << JsonEscape(entry.source.externalItemId)
            << "\",\"permissionState\":\""
            << PermissionDecisionStateName(entry.permissionState)
            << "\",\"enabled\":" << (entry.enabled ? "true" : "false")
            << ",\"grantedPermissions\":[";
        for (std::size_t permission = 0;
            permission < entry.grantedPermissions.size(); ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(entry.grantedPermissions[permission]) << '"';
        }
        out << "],\"grantedNetworkDomains\":[";
        for (std::size_t domain = 0;
            domain < entry.grantedNetworkDomains.size(); ++domain)
        {
            if (domain) out << ',';
            out << '"' << JsonEscape(entry.grantedNetworkDomains[domain]) << '"';
        }
        out << "]}";
    }
    if (!orderedEntries.empty()) out << '\n';
    out << "  ],\n  \"developmentOverrides\": [";
    std::vector<std::string> orderedOverrides(
        developmentOverrides.begin(), developmentOverrides.end());
    std::sort(orderedOverrides.begin(), orderedOverrides.end());
    for (std::size_t i = 0; i < orderedOverrides.size(); ++i)
    {
        if (i) out << ',';
        out << "\n    \"" << JsonEscape(orderedOverrides[i]) << '"';
    }
    if (!orderedOverrides.empty()) out << '\n';
    out << "  ],\n  \"knownDevelopmentIds\": [";
    std::vector<std::string> knownIds(knownDevelopmentIds.begin(), knownDevelopmentIds.end());
    std::sort(knownIds.begin(), knownIds.end());
    for (std::size_t i = 0; i < knownIds.size(); ++i)
    {
        if (i) out << ',';
        out << '"' << JsonEscape(knownIds[i]) << '"';
    }
    if (!disabledDevelopmentIds.empty())
    {
        out << "  ],\n  \"disabledDevelopmentIds\": [";
        std::vector<std::string> disabledIds(
            disabledDevelopmentIds.begin(), disabledDevelopmentIds.end());
        std::sort(disabledIds.begin(), disabledIds.end());
        for (std::size_t i = 0; i < disabledIds.size(); ++i)
        {
            if (i) out << ',';
            out << '"' << JsonEscape(disabledIds[i]) << '"';
        }
    }
    out << "  ],\n  \"permissionDecisions\": [";
    std::vector<const PermissionDecisionRecord*> decisions;
    decisions.reserve(permissionDecisions.size());
    for (const auto& [key, decision] : permissionDecisions)
    {
        (void)key;
        decisions.push_back(&decision);
    }
    std::sort(decisions.begin(), decisions.end(),
        [](const auto* a, const auto* b) {
            return std::tie(a->packageId, a->source.providerId,
                       a->source.externalItemId) <
                std::tie(b->packageId, b->source.providerId,
                       b->source.externalItemId);
        });
    for (std::size_t i = 0; i < decisions.size(); ++i)
    {
        if (i) out << ',';
        const auto& decision = *decisions[i];
        out << "\n    {\"packageId\":\""
            << JsonEscape(decision.packageId)
            << "\",\"providerId\":\""
            << JsonEscape(decision.source.providerId)
            << "\",\"externalItemId\":\""
            << JsonEscape(decision.source.externalItemId)
            << "\",\"state\":\""
            << PermissionDecisionStateName(decision.state)
            << "\",\"requestedScopeFingerprint\":\""
            << JsonEscape(decision.requestedScopeFingerprint)
            << "\",\"requestedPermissions\":[";
        for (std::size_t permission = 0;
            permission < decision.requestedPermissions.size(); ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(
                decision.requestedPermissions[permission]) << '"';
        }
        out << "],\"requestedOptionalPermissions\":[";
        for (std::size_t permission = 0;
            permission < decision.requestedOptionalPermissions.size();
            ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(
                decision.requestedOptionalPermissions[permission]) << '"';
        }
        out << "],\"requestedNetworkDomains\":[";
        for (std::size_t domain = 0;
            domain < decision.requestedNetworkDomains.size(); ++domain)
        {
            if (domain) out << ',';
            out << '"' << JsonEscape(
                decision.requestedNetworkDomains[domain]) << '"';
        }
        out << "],\"grantedPermissions\":[";
        for (std::size_t permission = 0;
            permission < decision.grantedPermissions.size(); ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(
                decision.grantedPermissions[permission]) << '"';
        }
        out << "],\"grantedNetworkDomains\":[";
        for (std::size_t domain = 0;
            domain < decision.grantedNetworkDomains.size(); ++domain)
        {
            if (domain) out << ',';
            out << '"' << JsonEscape(
                decision.grantedNetworkDomains[domain]) << '"';
        }
        out << "]}";
    }
    if (!decisions.empty()) out << '\n';
    out << "  ],\n  \"steamSubscriptionsByAccount\": {";
    std::vector<std::string> accounts;
    accounts.reserve(steamSubscriptionsByAccount.size());
    for (const auto& [accountId, subscriptions] :
        steamSubscriptionsByAccount)
    {
        (void)subscriptions;
        accounts.push_back(accountId);
    }
    std::sort(accounts.begin(), accounts.end());
    for (std::size_t account = 0; account < accounts.size(); ++account)
    {
        if (account) out << ',';
        const auto& accountId = accounts[account];
        std::vector<std::string> subscriptions(
            steamSubscriptionsByAccount.at(accountId).begin(),
            steamSubscriptionsByAccount.at(accountId).end());
        std::sort(subscriptions.begin(), subscriptions.end());
        out << "\n    \"" << JsonEscape(accountId) << "\":[";
        for (std::size_t item = 0; item < subscriptions.size(); ++item)
        {
            if (item) out << ',';
            out << '\"' << JsonEscape(subscriptions[item]) << '\"';
        }
        out << ']';
    }
    if (!accounts.empty()) out << '\n';
    out << "  },\n  \"steamLibrarySubscriptionsByAccount\": {";
    accounts.clear();
    for (const auto& [accountId, libraries] : steamLibrarySubscriptionsByAccount)
    {
        (void)libraries;
        accounts.push_back(accountId);
    }
    std::sort(accounts.begin(), accounts.end());
    for (std::size_t account = 0; account < accounts.size(); ++account)
    {
        if (account) out << ',';
        const auto& accountId = accounts[account];
        const auto& libraries = steamLibrarySubscriptionsByAccount.at(accountId);
        out << "\n    \"" << JsonEscape(accountId) << "\":{";
        std::vector<std::string> keys;
        for (const auto& [library, subscriptions] : libraries)
        {
            (void)subscriptions;
            keys.push_back(library);
        }
        std::sort(keys.begin(), keys.end());
        for (std::size_t library = 0; library < keys.size(); ++library)
        {
            if (library) out << ',';
            out << '\"' << JsonEscape(keys[library]) << "\":[";
            auto subscriptions = libraries.at(keys[library]);
            std::sort(subscriptions.begin(), subscriptions.end());
            subscriptions.erase(std::unique(subscriptions.begin(),
                subscriptions.end()), subscriptions.end());
            for (std::size_t item = 0; item < subscriptions.size(); ++item)
            {
                if (item) out << ',';
                out << '\"' << JsonEscape(subscriptions[item]) << '\"';
            }
            out << ']';
        }
        out << '}';
    }
    if (!accounts.empty()) out << '\n';
    out << "  }\n}\n";
    return atomic_file::WriteAll(path, out.str(), {}, &error);
}

std::unordered_map<std::string, std::vector<std::string>>
WidgetPackageManager::SteamSubscriptionHistory() const
{
    std::unordered_map<std::string, std::vector<std::string>> result;
    for (const auto& [accountId, subscriptions] :
        registry_.steamSubscriptionsByAccount)
    {
        auto& values = result[accountId];
        values.assign(subscriptions.begin(), subscriptions.end());
        std::sort(values.begin(), values.end());
    }
    return result;
}

bool WidgetPackageManager::UpdateSteamSubscriptionHistory(
    const std::string& accountId,
    const std::vector<std::string>& publishedFileIds, std::string& error,
    const SteamLibrarySubscriptions* libraries)
{
    if (!DecimalIdentifier(accountId) ||
        !std::all_of(publishedFileIds.begin(), publishedFileIds.end(),
            DecimalIdentifier) ||
        (libraries && !std::all_of(libraries->begin(), libraries->end(),
            [](const auto& entry) {
                return !entry.first.empty() && std::all_of(entry.second.begin(),
                    entry.second.end(), DecimalIdentifier);
            })))
    {
        error = "invalid Steam subscription history identity";
        return false;
    }

    std::unordered_set<std::string> subscriptions(publishedFileIds.begin(), publishedFileIds.end());
    SteamLibrarySubscriptions nextLibraries;
    if (libraries)
    {
        nextLibraries = *libraries;
        for (auto& [library, ids] : nextLibraries)
        {
            (void)library;
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }
    }

    const auto previous = registry_.steamSubscriptionsByAccount.find(accountId);
    const bool existed = previous != registry_.steamSubscriptionsByAccount.end();
    const auto previousLibraries = registry_.steamLibrarySubscriptionsByAccount.find(accountId);
    const bool librariesExisted = previousLibraries != registry_.steamLibrarySubscriptionsByAccount.end();
    if (existed && previous->second == subscriptions &&
        (!libraries || (librariesExisted && previousLibraries->second == nextLibraries)))
        return true;

    std::unordered_set<std::string> previousSubscriptions;
    if (existed) previousSubscriptions = std::move(previous->second);
    SteamLibrarySubscriptions savedLibraries;
    if (libraries && librariesExisted) savedLibraries = std::move(previousLibraries->second);
    registry_.steamSubscriptionsByAccount[accountId] = std::move(subscriptions);
    if (libraries) registry_.steamLibrarySubscriptionsByAccount[accountId] = std::move(nextLibraries);
    if (registry_.Save(paths_.registry, error)) return true;
    if (libraries)
    {
        if (librariesExisted)
            registry_.steamLibrarySubscriptionsByAccount[accountId] = std::move(savedLibraries);
        else
            registry_.steamLibrarySubscriptionsByAccount.erase(accountId);
    }
    if (existed)
        registry_.steamSubscriptionsByAccount[accountId] =
            std::move(previousSubscriptions);
    else
        registry_.steamSubscriptionsByAccount.erase(accountId);
    return false;
}

SteamLibrarySubscriptionHistory WidgetPackageManager::SteamLibraryHistory() const
{
    return registry_.steamLibrarySubscriptionsByAccount;
}
}
