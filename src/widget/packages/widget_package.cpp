#include "widget_package.h"
#include "widget_package_manifest_fields.h"
#include "widget_package_read.h"
#include "widget_catalog_refresh.h"

#include "common/language_fallback.h"
#include "widget/permissions/widget_permission_broker.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <system_error>

namespace snowdesktop::widget
{
namespace
{
using detail::DecimalIdentifier;
using detail::HasReparsePoint;
using detail::JsonEscape;
using detail::Lower;
using detail::PathUtf8;
using detail::StartsWithPath;
using detail::Timestamp;
using detail::Utf8ToWide;
using detail::WideToUtf8;

bool PathContainsReparsePoint(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (ec) return false;
    const auto normalized = absolute.lexically_normal();
    auto current = normalized.root_path();
    if (!current.empty() && HasReparsePoint(current)) return true;
    for (const auto& component : normalized.relative_path())
    {
        current /= component;
        if (HasReparsePoint(current)) return true;
    }
    return false;
}

struct PermissionExpansion
{
    std::vector<std::string> required;
    std::vector<std::string> optional;
    std::vector<std::string> networkDomains;

    bool Empty() const
    {
        return required.empty() && optional.empty() && networkDomains.empty();
    }
};

std::string DescribePermissionExpansion(const PermissionExpansion& expansion)
{
    std::vector<std::string> descriptions;
    descriptions.reserve(expansion.required.size() + expansion.optional.size() +
        expansion.networkDomains.size());
    for (const auto& permission : expansion.required)
        descriptions.push_back(
            "update requests a new required permission: " + permission);
    for (const auto& permission : expansion.optional)
        descriptions.push_back(
            "update requests a new optional permission: " + permission);
    for (const auto& domain : expansion.networkDomains)
        descriptions.push_back(
            "update requests a new network domain: " + domain);

    std::ostringstream output;
    for (std::size_t index = 0; index < descriptions.size(); ++index)
    {
        if (index != 0) output << "; ";
        output << descriptions[index];
    }
    return output.str();
}

PackageManifest LocalizedManifest(PackageManifest manifest,
    const std::string& requestedLocale)
{
    if (requestedLocale.empty() || manifest.locales.empty()) return manifest;
    const LocalizedMetadata* selected = nullptr;
    std::vector<std::string> available;
    available.reserve(manifest.locales.size());
    for (const auto& [locale, metadata] : manifest.locales)
    {
        (void)metadata;
        available.push_back(locale);
    }
    const std::string selectedLocale =
        snowdesktop::localization::ResolveBestLanguage(
            available, requestedLocale);
    if (!selectedLocale.empty())
        selected = &manifest.locales.at(selectedLocale);
    if (selected)
    {
        if (!selected->title.empty()) manifest.name = selected->title;
        if (!selected->description.empty())
            manifest.description = selected->description;
        auto translate = [&](const std::string& key,
            std::string& value) {
            if (key.empty()) return;
            if (const auto found = selected->strings.find(key);
                found != selected->strings.end())
                value = found->second;
        };
        translate(manifest.previewIntroductionKey,
            manifest.previewIntroduction);
        for (auto& preset : manifest.creationPresets)
            translate(preset.titleKey, preset.title);
        for (auto& helper : manifest.nativeHelpers)
        {
            translate(helper.purposeKey, helper.purpose);
            translate(helper.operationsKey, helper.operations);
            translate(helper.reasonKey, helper.reason);
            translate(helper.deniedKey, helper.denied);
        }
        for (const auto& [storageKey, localizationKey] :
            manifest.previewStorageKeys)
        {
            if (const auto value = manifest.previewStorage.find(storageKey);
                value != manifest.previewStorage.end())
                translate(localizationKey, value->second);
        }
        for (auto& variant : manifest.previewVariants)
        {
            translate(variant.titleKey, variant.title);
            translate(variant.descriptionKey, variant.description);
            for (const auto& [storageKey, localizationKey] :
                variant.storageKeys)
            {
                if (const auto value = variant.storage.find(storageKey);
                    value != variant.storage.end())
                    translate(localizationKey, value->second);
            }
        }
    }
    return manifest;
}

bool MatchesExpectedManifest(const PackageManifest& actual,
    const PackageManifest& expected, std::string& error)
{
    if (actual.id != expected.id)
    {
        error = "package identity does not match the source metadata";
        return false;
    }
    if (actual.version != expected.version)
    {
        error = "package version does not match the source metadata";
        return false;
    }
    if (expected.schemaVersion > 0 &&
        actual.schemaVersion != expected.schemaVersion)
    {
        error = "package schema version does not match the source metadata";
        return false;
    }
    if (expected.apiVersion > 0 && actual.apiVersion != expected.apiVersion)
    {
        error = "package API version does not match the source metadata";
        return false;
    }
    const std::set<std::string> actualPermissions(
        actual.permissions.begin(), actual.permissions.end());
    const std::set<std::string> expectedPermissions(
        expected.permissions.begin(), expected.permissions.end());
    if (actualPermissions != expectedPermissions)
    {
        error = "package permissions do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualOptionalPermissions(
        actual.optionalPermissions.begin(),
        actual.optionalPermissions.end());
    const std::set<std::string> expectedOptionalPermissions(
        expected.optionalPermissions.begin(),
        expected.optionalPermissions.end());
    if (actualOptionalPermissions != expectedOptionalPermissions)
    {
        error = "package optional permissions do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualDomains(
        actual.networkDomains.begin(), actual.networkDomains.end());
    const std::set<std::string> expectedDomains(
        expected.networkDomains.begin(), expected.networkDomains.end());
    if (actualDomains != expectedDomains)
    {
        error = "package network domains do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualRequiredFeatures(
        actual.requiredFeatures.begin(), actual.requiredFeatures.end());
    const std::set<std::string> expectedRequiredFeatures(
        expected.requiredFeatures.begin(), expected.requiredFeatures.end());
    if (actualRequiredFeatures != expectedRequiredFeatures)
    {
        error = "package required features do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualOptionalFeatures(
        actual.optionalFeatures.begin(), actual.optionalFeatures.end());
    const std::set<std::string> expectedOptionalFeatures(
        expected.optionalFeatures.begin(), expected.optionalFeatures.end());
    if (actualOptionalFeatures != expectedOptionalFeatures)
    {
        error = "package optional features do not match the source metadata";
        return false;
    }
    if (!expected.resources.empty() && actual.resources != expected.resources)
    {
        error = "package resources do not match the source metadata";
        return false;
    }
    if (!expected.logicalSlots.empty() &&
        actual.logicalSlots != expected.logicalSlots)
    {
        error = "package logical slots do not match the source metadata";
        return false;
    }
    return true;
}
}

PackageManifest LocalizePackageManifest(PackageManifest manifest,
    const std::string& requestedLocale)
{
    return LocalizedManifest(std::move(manifest), requestedLocale);
}

std::string PackagePermissionScopeFingerprint(const PackageManifest& manifest)
{
    auto permissions = manifest.permissions;
    if (!manifest.nativeHelpers.empty())
        permissions.push_back("native-content:" + manifest.nativeContentFingerprint);
    return WidgetPermissionBroker::ScopeFingerprint(permissions,
        manifest.optionalPermissions, manifest.networkDomains);
}

std::string PackagePermissionReviewFingerprint(const InstalledPackage& package)
{
    const auto scope = PackagePermissionScopeFingerprint(package.manifest);
    if (package.manifest.nativeHelpers.empty()) return scope;
    const std::vector<std::string> identity{
        "scope:" + scope, "id:" + package.manifest.id,
        "version:" + package.manifest.version, "provider:" + package.source.providerId,
        "item:" + package.source.externalItemId };
    return WidgetPermissionBroker::ScopeFingerprint(identity, {});
}

bool MeetsWidgetMinimumHostVersion(std::string_view host, std::string_view minimum)
{
    std::istringstream actual{std::string(host)};
    std::istringstream required{std::string(minimum)};
    std::string actualPart, requiredPart;
    for (int part = 0; part < 4; ++part)
    {
        const int a = std::getline(actual, actualPart, '.') ? std::atoi(actualPart.c_str()) : 0;
        const int b = std::getline(required, requiredPart, '.') ? std::atoi(requiredPart.c_str()) : 0;
        if (a != b) return a > b;
    }
    return true;
}

bool IsExecutablePackageContract(int schemaVersion, int apiVersion) noexcept
{
    return schemaVersion == kPackageSchemaVersion &&
        apiVersion == kHostApiVersion;
}

bool IsExecutablePackageContract(const PackageManifest& manifest) noexcept
{
    return IsExecutablePackageContract(
        manifest.schemaVersion, manifest.apiVersion);
}

std::vector<std::string> DeclaredPermissions(
    const PackageManifest& manifest)
{
    std::vector<std::string> result = manifest.permissions;
    for (const auto& permission : manifest.optionalPermissions)
        if (std::find(result.begin(), result.end(), permission) ==
            result.end())
            result.push_back(permission);
    return result;
}

bool ValidationReport::Ok() const
{
    return std::none_of(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.severity == ValidationSeverity::Error;
    });
}

void ValidationReport::Add(ValidationSeverity severity, std::string code,
    std::filesystem::path path, std::string message)
{
    issues.push_back({ severity, std::move(code), std::move(path),
        std::move(message) });
}

std::string ValidationReport::ToJson() const
{
    std::ostringstream out;
    out << "{\"ok\":" << (Ok() ? "true" : "false")
        << ",\"fileCount\":" << fileCount
        << ",\"totalBytes\":" << totalBytes << ",\"issues\":[";
    for (std::size_t i = 0; i < issues.size(); ++i)
    {
        if (i) out << ',';
        const char* severity = issues[i].severity == ValidationSeverity::Error
            ? "error" : issues[i].severity == ValidationSeverity::Warning
            ? "warning" : "info";
        out << "{\"severity\":\"" << severity << "\",\"code\":\""
            << JsonEscape(issues[i].code) << "\",\"path\":\""
            << JsonEscape(PathUtf8(issues[i].path)) << "\",\"message\":\""
            << JsonEscape(issues[i].message) << "\"}";
    }
    out << "]}";
    return out.str();
}

ValidationReport WidgetPackageValidator::ValidateArchive(
    const std::filesystem::path& archive) const
{
    ValidationReport report;
    if (PathContainsReparsePoint(archive))
    {
        report.Add(ValidationSeverity::Error, "archive.reparse", archive,
            "package archive and its path cannot contain reparse points");
        return report;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(archive, ec);
    if (ec || size > kMaxArchiveBytes)
    {
        report.Add(ValidationSeverity::Error, "archive.size", archive,
            "archive is missing or exceeds 20 MiB");
        return report;
    }
    if (Lower(archive.extension().string()) != ".snowwidget")
        report.Add(ValidationSeverity::Error, "archive.extension", archive,
            "package archive must use the .snowwidget extension");
    // Full entry validation happens during secure extraction. This function
    // deliberately never invokes the Windows shell ZIP handler.
    return report;
}

WidgetPackageManager::WidgetPackageManager(PackagePaths paths)
    : paths_(std::move(paths)),
      catalogueRefresh_(std::make_shared<detail::CatalogRefresh>())
{
}

bool WidgetPackageManager::Initialize(std::string& error)
{
    std::error_code ec;
    for (const auto& path : { paths_.installed, paths_.development,
        paths_.staging, paths_.quarantine, paths_.migrations })
    {
        std::filesystem::create_directories(path, ec);
        if (ec)
        {
            error = "cannot create package directory: " + ec.message();
            return false;
        }
    }
    if (!registry_.Load(paths_, validator_, error)) return false;
    if (!Refresh(error)) return false;
    return true;
}

bool WidgetPackageManager::RefreshCatalog(std::string& error)
{
    error.clear();
    if (catalogueRefresh_->CanReuse(catalogueRevision_)) return true;
    auto refreshed = *this;
    if (!refreshed.Refresh(error)) return false;
    *this = std::move(refreshed);
    return true;
}

bool WidgetPackageManager::Refresh(std::string& error, bool persistDiscovery)
{
    error.clear();
    catalogueRefresh_->Begin({paths_.builtin, paths_.installed, paths_.development});
    const auto previousKnown = registry_.knownDevelopmentIds;
    const auto previousOverrides = registry_.developmentOverrides;
    auto scanned = std::make_shared<std::vector<InstalledPackage>>();
    auto& packages = *scanned;
    invalidPackages_.clear();
    enum class ExplicitDecisionResult
    {
        NotFound,
        Applied,
        ScopeMismatch,
    };
    const auto applyExplicitDecision = [&](InstalledPackage& package) {
        const std::string key = package.manifest.id + "\n" +
            package.source.providerId + "\n" +
            package.source.externalItemId;
        const auto decision = registry_.permissionDecisions.find(key);
        if (decision == registry_.permissionDecisions.end())
            return ExplicitDecisionResult::NotFound;
        const std::set<std::string> requestedPermissions(
            decision->second.requestedPermissions.begin(),
            decision->second.requestedPermissions.end());
        const std::set<std::string> declaredPermissions(
            package.manifest.permissions.begin(),
            package.manifest.permissions.end());
        const std::set<std::string> requestedOptionalPermissions(
            decision->second.requestedOptionalPermissions.begin(),
            decision->second.requestedOptionalPermissions.end());
        const std::set<std::string> declaredOptionalPermissions(
            package.manifest.optionalPermissions.begin(),
            package.manifest.optionalPermissions.end());
        const std::set<std::string> requestedDomains(
            decision->second.requestedNetworkDomains.begin(),
            decision->second.requestedNetworkDomains.end());
        const std::set<std::string> declaredDomains(
            package.manifest.networkDomains.begin(),
            package.manifest.networkDomains.end());
        const std::string requestedScopeFingerprint =
            decision->second.requestedScopeFingerprint.empty()
            ? WidgetPermissionBroker::ScopeFingerprint(
                decision->second.requestedPermissions,
                decision->second.requestedOptionalPermissions,
                decision->second.requestedNetworkDomains)
            : decision->second.requestedScopeFingerprint;
        const std::string declaredScopeFingerprint =
            PackagePermissionScopeFingerprint(package.manifest);
        if (requestedPermissions != declaredPermissions ||
            requestedOptionalPermissions !=
                declaredOptionalPermissions ||
            requestedDomains != declaredDomains ||
            requestedScopeFingerprint != declaredScopeFingerprint)
            return ExplicitDecisionResult::ScopeMismatch;
        package.permissionState = decision->second.state;
        const auto grant = WidgetPermissionBroker::Evaluate(
            package.permissionState,
            package.manifest.permissions,
            package.manifest.optionalPermissions,
            package.manifest.networkDomains,
            decision->second.grantedPermissions,
            decision->second.grantedNetworkDomains);
        package.grantedPermissions = grant.permissions;
        package.grantedNetworkDomains = grant.networkDomains;
        return ExplicitDecisionResult::Applied;
    };
    const auto applyScopeMismatchBlock = [](
        InstalledPackage& package, ExplicitDecisionResult result) {
        if (result != ExplicitDecisionResult::ScopeMismatch) return;
        const auto declaredPermissions = DeclaredPermissions(
            package.manifest);
        const bool requiresConsent = !PermissionsRequiringConsent(
            declaredPermissions).empty();
        if (!requiresConsent)
        {
            package.permissionState = PermissionDecisionState::Granted;
            package.grantedPermissions = declaredPermissions;
            package.grantedNetworkDomains = package.manifest.networkDomains;
            return;
        }
        package.permissionState = PermissionDecisionState::Pending;
        package.grantedPermissions.clear();
        package.grantedNetworkDomains.clear();
    };
    auto scanRoot = [&](const std::filesystem::path& root, bool builtin,
        bool development) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) return;
        for (std::filesystem::directory_iterator it(root, ec), end;
            !ec && it != end; it.increment(ec))
        {
            if (!it->is_directory(ec) || HasReparsePoint(it->path())) continue;
            if (builtin || development)
            {
                PackageManifest manifest;
                const auto report = validator_.ValidateDirectory(
                    it->path(), &manifest);
                if (!report.Ok())
                {
                    // The bundled Agent Skill intentionally shares the
                    // built-in root without being a component package.
                    if (!builtin || std::filesystem::is_regular_file(
                            it->path() / L"widget.json", ec))
                    {
                        InvalidPackage invalid;
                        invalid.manifest = std::move(manifest);
                        invalid.packageId = invalid.manifest.id;
                        invalid.root = it->path();
                        invalid.report = report;
                        invalid.builtin = builtin;
                        invalid.development = development;
                        invalid.source = {
                            builtin ? "builtin" : "local-directory",
                            PathUtf8(it->path().filename()) };
                        invalid.selected = builtin ||
                            registry_.developmentOverrides.contains(
                                invalid.manifest.id);
                        invalidPackages_.push_back(std::move(invalid));
                    }
                    ec.clear();
                    continue;
                }
                if (development && registry_.knownDevelopmentIds.insert(manifest.id).second)
                    registry_.developmentOverrides.insert(manifest.id);
                InstalledPackage package;
                package.manifest = std::move(manifest);
                package.root = it->path();
                package.builtin = builtin;
                package.development = development;
                package.source = { builtin ? "builtin" : "local-directory",
                    PathUtf8(it->path().filename()) };
                package.permissionState =
                    PermissionDecisionState::LegacyImplicit;
                package.grantedPermissions =
                    DeclaredPermissions(package.manifest);
                package.grantedNetworkDomains =
                    package.manifest.networkDomains;
                package.enabled = true;
                applyScopeMismatchBlock(
                    package, applyExplicitDecision(package));
                package.active = builtin ||
                    registry_.developmentOverrides.contains(package.manifest.id);
                package.selected = package.active;
                if ((development || !package.manifest.nativeHelpers.empty()) && package.permissionState == PermissionDecisionState::LegacyImplicit &&
                    !PermissionsRequiringConsent(DeclaredPermissions(package.manifest)).empty())
                {
                    package.permissionState = PermissionDecisionState::Pending;
                    package.grantedPermissions.clear();
                    package.grantedNetworkDomains.clear();
                }
                packages.push_back(std::move(package));
                continue;
            }
            const std::string id = WideToUtf8(it->path().filename().wstring());
            if (!WidgetPackageValidator::IsUuid(id)) continue;
            for (std::filesystem::directory_iterator versionIt(it->path(), ec),
                versionEnd; !ec && versionIt != versionEnd;
                versionIt.increment(ec))
            {
                if (!versionIt->is_directory(ec)) continue;
                PackageManifest manifest;
                const auto report = validator_.ValidateDirectory(
                    versionIt->path(), &manifest);
                if (!report.Ok() || manifest.id != id)
                {
                    InvalidPackage invalid;
                    invalid.manifest = std::move(manifest);
                    invalid.packageId = id;
                    invalid.root = versionIt->path();
                    invalid.report = report;
                    const auto registryIt = registry_.entries.find(id);
                    invalid.source = registryIt != registry_.entries.end()
                        ? registryIt->second.source
                        : PackageSourceRef{ "local", id };
                    invalid.selected = registryIt != registry_.entries.end() &&
                        registryIt->second.activeVersion ==
                            invalid.manifest.version;
                    if (report.Ok() && invalid.manifest.id != id)
                    {
                        invalid.report.Add(ValidationSeverity::Error,
                            "package.idDirectory", versionIt->path(),
                            "manifest id does not match the installed package directory");
                    }
                    invalidPackages_.push_back(std::move(invalid));
                    continue;
                }
                InstalledPackage package;
                package.manifest = std::move(manifest);
                package.root = versionIt->path();
                const auto registryIt = registry_.entries.find(id);
                package.active = registryIt != registry_.entries.end() &&
                    registryIt->second.activeVersion == package.manifest.version;
                package.selected = package.active;
                package.enabled = registryIt == registry_.entries.end() ||
                    registryIt->second.enabled;
                if (registryIt != registry_.entries.end())
                {
                    package.source = registryIt->second.source;
                    package.permissionState =
                        registryIt->second.permissionState;
                    const auto grant = WidgetPermissionBroker::Evaluate(
                        package.permissionState,
                        package.manifest.permissions,
                        package.manifest.optionalPermissions,
                        package.manifest.networkDomains,
                        registryIt->second.grantedPermissions,
                        registryIt->second.grantedNetworkDomains);
                    package.grantedPermissions = grant.permissions;
                    package.grantedNetworkDomains = grant.networkDomains;
                    applyScopeMismatchBlock(
                        package, applyExplicitDecision(package));
                }
                else
                {
                    package.source = { "local", id };
                    package.unregistered = true;
                    package.permissionState =
                        PermissionDecisionState::LegacyImplicit;
                    package.grantedPermissions =
                        DeclaredPermissions(package.manifest);
                    package.grantedNetworkDomains =
                        package.manifest.networkDomains;
                }
                packages.push_back(std::move(package));
            }
        }
        if (ec && error.empty())
            error = "cannot enumerate package directory: " + ec.message();
    };
    scanRoot(paths_.builtin, true, false);
    scanRoot(paths_.installed, false, false);
    scanRoot(paths_.development, false, true);

    if (!error.empty())
    {
        packages_ = std::move(scanned);
        return false;
    }
    if (persistDiscovery && registry_.knownDevelopmentIds != previousKnown &&
        !registry_.Save(paths_.registry, error))
    {
        registry_.knownDevelopmentIds = previousKnown;
        registry_.developmentOverrides = previousOverrides;
        packages.clear();
        packages_ = std::move(scanned);
        return false;
    }
    // Newly discovered projects become the active source; persisted opt-outs remain off.
    std::set<std::string> developmentIds;
    for (const auto& package : packages)
        if (package.development && package.active)
            developmentIds.insert(package.manifest.id);
    for (auto& package : packages)
        if (!package.development && developmentIds.contains(package.manifest.id))
            package.active = false;

    // Built-ins are the fallback when no installed active version exists.
    std::set<std::string> activeIds;
    for (const auto& package : packages)
        if (!package.builtin && package.active) activeIds.insert(package.manifest.id);
    for (auto& package : packages)
        if (package.builtin && activeIds.contains(package.manifest.id))
            package.active = false;
    packages_ = std::move(scanned);
    catalogueRevision_ = std::make_shared<const char>('\0');
    catalogueRefresh_->Complete(catalogueRevision_);
    return error.empty();
}

std::vector<InstalledPackage> WidgetPackageManager::ListPackages() const
{
    return *packages_;
}

std::vector<InvalidPackage> WidgetPackageManager::ListInvalidPackages() const
{
    return invalidPackages_;
}

bool WidgetPackageManager::ContainsPackage(const std::string& packageId) const
{
    return std::any_of(packages_->begin(), packages_->end(),
        [&](const auto& package)
        {
            return package.manifest.id == packageId;
        });
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

std::optional<InstalledPackage> WidgetPackageManager::Resolve(
    const std::string& packageId) const
{
    const auto* package = FindResolved(packageId);
    return package ? std::optional<InstalledPackage>(*package) : std::nullopt;
}

const InstalledPackage* WidgetPackageManager::FindResolved(
    const std::string& packageId) const
{
    if (const auto registry = registry_.entries.find(packageId);
        registry != registry_.entries.end() && !registry->second.enabled &&
        !registry_.developmentOverrides.contains(packageId))
        return nullptr;
    const InstalledPackage* builtinFallback = nullptr;
    for (const auto& package : *packages_)
    {
        if (package.manifest.id != packageId) continue;
        if (package.active && package.enabled) return &package;
        if (package.builtin && package.enabled && !builtinFallback)
            builtinFallback = &package;
    }
    return builtinFallback;
}

std::optional<InstalledPackage> WidgetPackageManager::ResolveSelected(
    const std::string& packageId) const
{
    const auto found = std::find_if(packages_->begin(), packages_->end(),
        [&](const InstalledPackage& package) {
            return package.manifest.id == packageId && package.active;
        });
    return found == packages_->end() ? std::nullopt : std::optional<InstalledPackage>(*found);
}

std::optional<std::filesystem::path> WidgetPackageManager::ResolveEntry(
    const std::string& packageId) const
{
    const auto package = Resolve(packageId);
    if (!package) return std::nullopt;
    const auto entry = package->root / Utf8ToWide(package->manifest.entry);
    std::error_code ec;
    const auto canonicalRoot = std::filesystem::weakly_canonical(
        package->root, ec);
    const auto canonicalEntry = std::filesystem::weakly_canonical(entry, ec);
    if (ec || !StartsWithPath(canonicalEntry, canonicalRoot) ||
        HasReparsePoint(canonicalEntry))
        return std::nullopt;
    return canonicalEntry;
}

std::optional<std::filesystem::path> WidgetPackageManager::ResolveSelectedEntry(
    const std::string& packageId) const
{
    const auto package = ResolveSelected(packageId);
    if (!package) return std::nullopt;
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(package->root, ec);
    if (ec) return std::nullopt;
    const auto entry = std::filesystem::weakly_canonical(
        package->root / Utf8ToWide(package->manifest.entry), ec);
    if (ec || !StartsWithPath(entry, root) || HasReparsePoint(entry))
        return std::nullopt;
    return entry;
}

std::optional<InstalledPackage> WidgetPackageManager::ResolveSelectedEntryPath(
    const std::filesystem::path& entryPath) const
{
    std::error_code ec;
    const auto input = std::filesystem::weakly_canonical(entryPath, ec);
    if (ec) return std::nullopt;
    for (const auto& package : *packages_)
    {
        if (!package.active) continue;
        const auto entry = ResolveSelectedEntry(package.manifest.id);
        if (entry && std::filesystem::equivalent(*entry, input, ec)) return package;
        ec.clear();
    }
    return std::nullopt;
}

std::optional<InstalledPackage> WidgetPackageManager::ResolveEntryPath(
    const std::filesystem::path& entryPath) const
{
    std::error_code ec;
    const auto canonicalInput =
        std::filesystem::weakly_canonical(entryPath, ec);
    if (ec) return std::nullopt;
    for (const auto& package : *packages_)
    {
        if (!package.active || !package.enabled)
            continue;
        const auto resolved = ResolveEntry(package.manifest.id);
        if (!resolved)
            continue;
        if (std::filesystem::equivalent(*resolved, canonicalInput, ec))
            return package;
        ec.clear();
    }
    return std::nullopt;
}

bool WidgetPackageManager::RefreshChangedPermissionScope(
    const std::string& packageId, bool& changed, std::string& error)
{
    changed = false;
    const auto package = Resolve(packageId);
    if (!package)
    {
        error = "package is unavailable";
        return false;
    }

    PackageManifest diskManifest;
    const ValidationReport report = validator_.ValidateDirectory(
        package->root, &diskManifest);
    if (!report.Ok() || diskManifest.id != packageId)
    {
        error = "active package failed validation while refreshing permissions";
        return false;
    }

    const std::string knownScope = PackagePermissionScopeFingerprint(package->manifest);
    const std::string diskScope = PackagePermissionScopeFingerprint(diskManifest);
    if (knownScope == diskScope)
    {
        error.clear();
        return true;
    }

    if (!Refresh(error)) return false;
    changed = true;
    return true;
}

ValidationReport WidgetPackageManager::ValidateDirectory(
    const std::filesystem::path& root, PackageManifest* manifest) const
{
    return validator_.ValidateDirectory(root, manifest);
}

ValidationReport WidgetPackageManager::ValidateArchive(
    const std::filesystem::path& archive, PackageManifest* manifest,
    bool* malformedArchive) const
{
    if (malformedArchive) *malformedArchive = false;
    ValidationReport report = validator_.ValidateArchive(archive);
    if (!report.Ok()) return report;
    std::error_code ec;
    if (paths_.staging.empty())
    {
        report.Add(ValidationSeverity::Error, "archive.staging", archive,
            "the validation staging directory is not configured");
        return report;
    }
    std::filesystem::create_directories(paths_.staging, ec);
    if (ec || HasReparsePoint(paths_.staging))
    {
        report.Add(ValidationSeverity::Error, "archive.staging", archive,
            "cannot prepare the validation staging directory");
        return report;
    }
    const auto staging = CreateStagingPath("validation");
    std::string error;
    report = {};
    if (!ExtractArchive(archive, staging, report, error, malformedArchive))
    {
        report.Add(ValidationSeverity::Error, "archive.content", archive,
            std::move(error));
    }
    else
    {
        report = validator_.ValidateDirectory(staging, manifest);
    }
    std::filesystem::remove_all(staging, ec);
    return report;
}

std::filesystem::path WidgetPackageManager::CreateStagingPath(
    const char* purpose) const
{
    return paths_.staging /
        (Utf8ToWide(std::string(purpose) + "-" + Timestamp() + "-" +
            GenerateUuid()));
}

bool WidgetPackageManager::CopyPackageTree(
    const std::filesystem::path& source,
    const std::filesystem::path& destination, std::string& error) const
{
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec)
    {
        error = "cannot create package destination: " + ec.message();
        return false;
    }
    std::filesystem::copy(source, destination,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = "cannot copy package: " + ec.message();
        return false;
    }
    return true;
}

bool WidgetPackageManager::CommitStagedPackage(
    const std::filesystem::path& stagedRoot, const PackageManifest& manifest,
    const PackageSourceRef& sourceRef, bool allowSourceChange,
    bool allowPermissionExpansion, InstalledPackage& installed,
    std::string& error)
{
    const auto existing = registry_.entries.find(manifest.id);
    if (existing != registry_.entries.end() &&
        !(existing->second.source == sourceRef) &&
        !allowSourceChange)
    {
        error = "package is already owned by provider " +
            existing->second.source.providerId +
            "; source changes require explicit confirmation";
        return false;
    }
    if (existing != registry_.entries.end() &&
        !detail::IsNewerValidSemVer(manifest.version, existing->second.activeVersion) &&
        manifest.version != existing->second.activeVersion)
    {
        error = "package updates cannot downgrade the active version; use rollback";
        return false;
    }
    if (existing != registry_.entries.end() && !allowPermissionExpansion)
    {
        std::vector<std::string> currentRequired;
        std::vector<std::string> currentDeclared;
        std::vector<std::string> currentDomains;
        if (const auto current = ResolveSelected(manifest.id))
        {
            currentRequired = current->manifest.permissions;
            currentDeclared = DeclaredPermissions(current->manifest);
            currentDomains = current->manifest.networkDomains;
        }
        const std::set<std::string> previouslyRequired(
            currentRequired.begin(), currentRequired.end());
        const std::set<std::string> previouslyRequested(
            currentDeclared.begin(), currentDeclared.end());
        PermissionExpansion expansion;
        for (const auto& permission : manifest.permissions)
        {
            if (!previouslyRequired.contains(permission))
                expansion.required.push_back(permission);
        }
        for (const auto& permission : manifest.optionalPermissions)
        {
            if (!previouslyRequested.contains(permission))
                expansion.optional.push_back(permission);
        }
        const std::set<std::string> previouslyRequestedDomains(
            currentDomains.begin(), currentDomains.end());
        for (const auto& domain : manifest.networkDomains)
        {
            if (!previouslyRequestedDomains.contains(domain))
                expansion.networkDomains.push_back(domain);
        }
        if (!expansion.Empty())
        {
            error = DescribePermissionExpansion(expansion);
            return false;
        }
    }
    const auto target = paths_.installed / Utf8ToWide(manifest.id) /
        Utf8ToWide(manifest.version);
    std::error_code ec;
    std::filesystem::path unregisteredBackup;
    if (std::filesystem::exists(target, ec))
    {
        if (existing != registry_.entries.end())
        {
            error = "this package version is already installed";
            return false;
        }
        // An interrupted/older installation can leave valid files without an
        // owner. Install the freshly validated payload with fresh permissions;
        // preserve the old tree instead of trusting or deleting its contents.
        if (!std::filesystem::is_directory(target, ec) ||
            PathContainsReparsePoint(target))
        {
            error = "cannot recover an unsafe unregistered package directory";
            return false;
        }
        std::filesystem::create_directories(paths_.quarantine, ec);
        if (ec || PathContainsReparsePoint(paths_.quarantine))
        {
            error = "cannot prepare unregistered package recovery backup";
            return false;
        }
        unregisteredBackup = paths_.quarantine / Utf8ToWide(
            manifest.id + "-" + manifest.version + "-unregistered-" + GenerateUuid());
        std::filesystem::rename(target, unregisteredBackup, ec);
        if (ec)
        {
            error = "cannot preserve unregistered package: " + ec.message();
            return false;
        }
    }
    const auto restoreUnregistered = [&] {
        if (unregisteredBackup.empty()) return;
        std::error_code restoreError;
        std::filesystem::rename(unregisteredBackup, target, restoreError);
        if (restoreError)
            error += "; cannot restore unregistered package: " + restoreError.message();
    };
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec)
    {
        error = "cannot create package version directory: " + ec.message();
        restoreUnregistered();
        return false;
    }
    std::filesystem::rename(stagedRoot, target, ec);
    if (ec)
    {
        error = "cannot atomically activate staged package: " + ec.message();
        restoreUnregistered();
        return false;
    }
    RegistryEntry entry;
    entry.packageId = manifest.id;
    entry.activeVersion = manifest.version;
    entry.source = sourceRef;
    const auto declaredPermissions = DeclaredPermissions(manifest);
    const bool requiresConsent = !PermissionsRequiringConsent(
        declaredPermissions).empty();
    entry.permissionState = requiresConsent
        ? PermissionDecisionState::Pending
        : PermissionDecisionState::Granted;
    if (!requiresConsent)
    {
        entry.grantedPermissions = declaredPermissions;
        entry.grantedNetworkDomains = manifest.networkDomains;
    }
    entry.enabled = existing == registry_.entries.end() || existing->second.enabled;
    auto candidate = *this;
    candidate.registry_.entries[manifest.id] = entry;
    const bool refreshed = candidate.Refresh(error, false);
    const auto resolved = refreshed ? candidate.ResolveSelected(manifest.id) : std::nullopt;
    if (!refreshed || !resolved || !candidate.registry_.Save(candidate.paths_.registry, error))
    {
        if (error.empty()) error = "installed package could not be selected";
        const auto quarantine = paths_.quarantine /
            Utf8ToWide(manifest.id + "-" + manifest.version + "-failed-" + GenerateUuid());
        std::filesystem::rename(target, quarantine, ec);
        if (ec) error += "; cannot quarantine uncommitted package: " + ec.message();
        else restoreUnregistered();
        return false;
    }
    *this = std::move(candidate);
    installed = *resolved;
    return true;
}

bool WidgetPackageManager::InstallDirectory(
    const std::filesystem::path& source, const PackageSourceRef& sourceRef,
    bool allowSourceChange, InstalledPackage& installed,
    ValidationReport& report, std::string& error,
    bool allowPermissionExpansion, const PackageManifest* expectedManifest)
{
    PackageManifest manifest;
    report = validator_.ValidateDirectory(source, &manifest);
    if (!report.Ok())
    {
        error = "package validation failed";
        return false;
    }
    if (expectedManifest &&
        !MatchesExpectedManifest(manifest, *expectedManifest, error))
        return false;
    const auto staging = CreateStagingPath("install");
    if (!CopyPackageTree(source, staging, error)) return false;
    PackageManifest copiedManifest;
    report = validator_.ValidateDirectory(staging, &copiedManifest);
    if (!report.Ok() || copiedManifest.id != manifest.id ||
        copiedManifest.version != manifest.version)
    {
        error = "staged package failed validation";
        const auto quarantine = paths_.quarantine /
            (Utf8ToWide(manifest.id + "-" + Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    if (CommitStagedPackage(staging, copiedManifest, sourceRef,
        allowSourceChange, allowPermissionExpansion, installed, error))
        return true;
    std::error_code commitError;
    if (std::filesystem::exists(staging, commitError))
        std::filesystem::rename(staging, paths_.quarantine /
            (Utf8ToWide(copiedManifest.id + "-" + copiedManifest.version +
                "-" + Timestamp())), commitError);
    return false;
}

bool WidgetPackageManager::InstallArchive(
    const std::filesystem::path& archive, const PackageSourceRef& sourceRef,
    bool allowSourceChange, InstalledPackage& installed,
    ValidationReport& report, std::string& error,
    bool allowPermissionExpansion, const PackageManifest* expectedManifest)
{
    report = validator_.ValidateArchive(archive);
    if (!report.Ok())
    {
        error = "archive validation failed";
        return false;
    }
    const auto staging = CreateStagingPath("archive");
    report = {};
    if (!ExtractArchive(archive, staging, report, error))
    {
        const auto quarantine = paths_.quarantine /
            (archive.stem().wstring() + L"-" + Utf8ToWide(Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    PackageManifest manifest;
    report = validator_.ValidateDirectory(staging, &manifest);
    if (!report.Ok())
    {
        error = "extracted package validation failed";
        const auto quarantine = paths_.quarantine /
            (archive.stem().wstring() + L"-" + Utf8ToWide(Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    if (expectedManifest &&
        !MatchesExpectedManifest(manifest, *expectedManifest, error))
    {
        const auto quarantine = paths_.quarantine /
            (archive.stem().wstring() + L"-" + Utf8ToWide(Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    if (CommitStagedPackage(staging, manifest, sourceRef,
        allowSourceChange, allowPermissionExpansion, installed, error))
        return true;
    std::error_code commitError;
    if (std::filesystem::exists(staging, commitError))
        std::filesystem::rename(staging, paths_.quarantine /
            (Utf8ToWide(manifest.id + "-" + manifest.version + "-" +
                Timestamp())), commitError);
    return false;
}

bool WidgetPackageManager::InstallFromSource(IWidgetPackageSource& source,
    const std::string& externalItemId, const std::string& version,
    bool allowSourceChange, InstalledPackage& installed,
    ValidationReport& report, std::string& error,
    bool allowPermissionExpansion)
{
    const auto status = source.Status();
    if (!status.available)
    {
        error = status.message.empty() ? "package source is unavailable" :
            status.message;
        return false;
    }
    const auto details =
        source.GetVersionDetails(externalItemId, version, error);
    if (!details)
        return false;
    const auto staging = CreateStagingPath("provider");
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec)
    {
        error = "cannot create provider staging directory: " + ec.message();
        return false;
    }
    const auto destination = staging / L"artifact.snowwidget";
    const auto artifact =
        source.Materialize(externalItemId, version, destination, error);
    auto quarantineOrRemove = [&]()
    {
        bool empty = std::filesystem::is_empty(staging, ec);
        if (!ec && empty)
        {
            std::filesystem::remove_all(staging, ec);
            return;
        }
        ec.clear();
        std::filesystem::rename(staging, paths_.quarantine /
            Utf8ToWide("provider-" + Timestamp() + "-" + GenerateUuid()), ec);
        if (ec) std::filesystem::remove_all(staging, ec);
    };
    if (!artifact)
    {
        quarantineOrRemove();
        return false;
    }
    if (artifact->packageId != details->manifest.id ||
        !WidgetPackageValidator::IsUuid(artifact->packageId))
    {
        error = "provider returned an invalid package identity: artifact=" +
            (artifact->packageId.empty() ? "<empty>" : artifact->packageId) +
            ", expected=" +
            (details->manifest.id.empty() ? "<empty>" : details->manifest.id);
        quarantineOrRemove();
        return false;
    }
    if (artifact->version != version)
    {
        error = "provider returned a different package version";
        quarantineOrRemove();
        return false;
    }

    const PackageSourceRef sourceRef = details->source;
    if (sourceRef.providerId != source.ProviderId() ||
        sourceRef.externalItemId.empty())
    {
        error = "provider returned an invalid source identity";
        quarantineOrRemove();
        return false;
    }
    const bool ok = std::filesystem::is_directory(artifact->localPath, ec)
        ? InstallDirectory(artifact->localPath, sourceRef, allowSourceChange,
            installed, report, error, allowPermissionExpansion,
            &details->manifest)
        : InstallArchive(artifact->localPath, sourceRef, allowSourceChange,
            installed, report, error, allowPermissionExpansion,
            &details->manifest);
    if (!ok)
    {
        quarantineOrRemove();
        return false;
    }
    std::filesystem::remove_all(staging, ec);
    return true;
}

bool WidgetPackageManager::SetEnabled(const std::string& packageId,
    bool enabled, std::string& error)
{
    auto found = registry_.entries.find(packageId);
    if (found == registry_.entries.end())
    {
        const auto package = Resolve(packageId);
        if (!package || package->builtin)
        {
            error = "built-in packages cannot be disabled in the v1 registry";
            return false;
        }
        return false;
    }
    auto candidate = *this;
    candidate.registry_.entries.at(packageId).enabled = enabled;
    if (!candidate.Refresh(error, false) ||
        !candidate.registry_.Save(candidate.paths_.registry, error)) return false;
    *this = std::move(candidate);
    return true;
}

bool WidgetPackageManager::SetPermissionDecision(
    const std::string& packageId, PermissionDecisionState state,
    const std::vector<std::string>& grantedPermissions,
    const std::vector<std::string>& grantedNetworkDomains,
    std::string& error, bool nativeAcknowledged, std::string_view reviewedNativeIdentity)
{
    error.clear();
    std::optional<InstalledPackage> package = Resolve(packageId);
    if (!package)
    {
        if (const auto registered = registry_.entries.find(packageId);
            registered != registry_.entries.end())
        {
            const auto managed = std::find_if(packages_->begin(),
                packages_->end(), [&](const auto& candidate) {
                    return candidate.manifest.id == packageId &&
                        candidate.manifest.version ==
                            registered->second.activeVersion &&
                        candidate.source == registered->second.source;
                });
            if (managed != packages_->end()) package = *managed;
        }
    }
    if (!package)
    {
        const auto selected = std::find_if(packages_->begin(),
            packages_->end(), [&](const auto& candidate) {
                return candidate.manifest.id == packageId &&
                    candidate.active;
            });
        if (selected != packages_->end()) package = *selected;
    }
    if (!package)
    {
        error = "package is unavailable";
        return false;
    }
    if (state == PermissionDecisionState::LegacyImplicit)
    {
        error = "legacy implicit permission state cannot be user-authored";
        return false;
    }
    if ((state == PermissionDecisionState::Pending ||
            state == PermissionDecisionState::Denied) &&
        (!grantedPermissions.empty() || !grantedNetworkDomains.empty()))
    {
        error = "pending or denied decisions cannot retain granted scopes";
        return false;
    }
    const std::vector<std::string> allDeclaredPermissions =
        DeclaredPermissions(package->manifest);
    const std::set<std::string> declaredPermissions(
        allDeclaredPermissions.begin(),
        allDeclaredPermissions.end());
    const std::set<std::string> declaredDomains(
        package->manifest.networkDomains.begin(),
        package->manifest.networkDomains.end());
    if (!std::all_of(grantedPermissions.begin(), grantedPermissions.end(),
            [&](const auto& permission) {
                return declaredPermissions.contains(permission);
            }) ||
        !std::all_of(grantedNetworkDomains.begin(),
            grantedNetworkDomains.end(), [&](const auto& domain) {
                return declaredDomains.contains(domain);
            }))
    {
        error = "granted scopes must be declared by the active package";
        return false;
    }

    if (state == PermissionDecisionState::Granted &&
        std::find(grantedPermissions.begin(), grantedPermissions.end(), "native.execute") != grantedPermissions.end())
    {
        if (!nativeAcknowledged || reviewedNativeIdentity.empty())
        { error = "nativeConfirmationRequired"; return false; }
        PackageManifest currentManifest;
        if (!validator_.ValidateDirectory(package->root, &currentManifest).Ok() ||
            currentManifest.nativeHelpers.empty() || currentManifest.id != packageId ||
            currentManifest.version != package->manifest.version)
        { error = "nativeContentChanged"; return false; }
        auto current = *package;
        current.manifest = std::move(currentManifest);
        if (PackagePermissionReviewFingerprint(current) != reviewedNativeIdentity)
        { error = "nativeContentChanged"; return false; }
        package->manifest = std::move(current.manifest);
    }
    PermissionDecisionRecord record;
    record.packageId = packageId;
    record.source = package->source;
    record.state = state;
    record.requestedPermissions = package->manifest.permissions;
    record.requestedOptionalPermissions =
        package->manifest.optionalPermissions;
    record.requestedNetworkDomains = package->manifest.networkDomains;
    record.requestedScopeFingerprint =
        PackagePermissionScopeFingerprint(package->manifest);
    record.grantedPermissions = grantedPermissions;
    record.grantedNetworkDomains = grantedNetworkDomains;
    const std::string key = packageId + "\n" +
        record.source.providerId + "\n" + record.source.externalItemId;
    const auto previousDecision = registry_.permissionDecisions.find(key);
    const std::optional<PermissionDecisionRecord> oldDecision =
        previousDecision == registry_.permissionDecisions.end()
        ? std::nullopt
        : std::optional<PermissionDecisionRecord>(previousDecision->second);
    const auto registry = registry_.entries.find(packageId);
    const bool updateRegistry = registry != registry_.entries.end() &&
        registry->second.source == package->source;
    const std::optional<RegistryEntry> oldRegistry = updateRegistry
        ? std::optional<RegistryEntry>(registry->second)
        : std::nullopt;

    registry_.permissionDecisions[key] = record;
    if (updateRegistry)
    {
        registry->second.permissionState = state;
        registry->second.grantedPermissions = grantedPermissions;
        registry->second.grantedNetworkDomains = grantedNetworkDomains;
    }
    if (registry_.Save(paths_.registry, error) && Refresh(error)) return true;

    const std::string originalError = error;
    if (oldDecision)
        registry_.permissionDecisions[key] = *oldDecision;
    else
        registry_.permissionDecisions.erase(key);
    if (oldRegistry) registry_.entries[packageId] = *oldRegistry;
    std::string rollbackError;
    registry_.Save(paths_.registry, rollbackError);
    Refresh(rollbackError);
    error = originalError;
    return false;
}

bool WidgetPackageManager::SetDevelopmentOverride(
    const std::string& packageId, bool active, std::string& error)
{
    const bool developmentExists = std::any_of(
        packages_->begin(), packages_->end(), [&](const auto& package)
        {
            return package.development &&
                package.manifest.id == packageId;
        });
    if (active && !developmentExists)
    {
        error = "development package is unavailable";
        return false;
    }

    const bool previous = registry_.developmentOverrides.contains(packageId);
    if (active)
        registry_.developmentOverrides.insert(packageId);
    else
        registry_.developmentOverrides.erase(packageId);
    if (!registry_.Save(paths_.registry, error))
    {
        if (previous)
            registry_.developmentOverrides.insert(packageId);
        else
            registry_.developmentOverrides.erase(packageId);
        return false;
    }
    const auto rollbackOverride = [&](std::string originalError) {
        if (previous)
            registry_.developmentOverrides.insert(packageId);
        else
            registry_.developmentOverrides.erase(packageId);
        std::string rollbackError;
        const bool saved = registry_.Save(paths_.registry, rollbackError);
        if (saved) Refresh(rollbackError);
        error = std::move(originalError);
        if (!rollbackError.empty())
            error += "; development override rollback failed: " +
                rollbackError;
        return false;
    };

    if (!Refresh(error))
        return rollbackOverride(error);

    if (active)
    {
        const auto resolved = Resolve(packageId);
        if (!resolved || !resolved->development)
        {
            return rollbackOverride(
                "development package could not be activated");
        }
        const std::vector<std::string> declaredPermissions =
            DeclaredPermissions(resolved->manifest);
        const bool sensitive = !PermissionsRequiringConsent(
            declaredPermissions).empty();
        if (sensitive && resolved->permissionState ==
                PermissionDecisionState::LegacyImplicit)
        {
            std::string permissionError;
            if (!SetPermissionDecision(packageId,
                    PermissionDecisionState::Pending, {}, {},
                    permissionError))
            {
                return rollbackOverride(std::move(permissionError));
            }
        }
    }
    return true;
}

bool WidgetPackageManager::Rollback(const std::string& packageId,
    const std::string& version, std::string& error)
{
    auto registry = registry_.entries.find(packageId);
    if (registry == registry_.entries.end())
    {
        error = "package has no installed version registry";
        return false;
    }
    const auto target = paths_.installed / Utf8ToWide(packageId) /
        Utf8ToWide(version);
    PackageManifest manifest;
    const auto report = validator_.ValidateDirectory(target, &manifest);
    if (!report.Ok() || manifest.id != packageId || manifest.version != version)
    {
        error = "requested rollback version is invalid or missing";
        return false;
    }
    const auto previous = registry->second.activeVersion;
    registry->second.activeVersion = version;
    if (!registry_.Save(paths_.registry, error))
    {
        registry->second.activeVersion = previous;
        return false;
    }
    return Refresh(error);
}

bool WidgetPackageManager::QuarantineInactiveVersion(
    const InstalledPackage& package, std::string& error)
{
    const auto& id = package.manifest.id;
    const auto& version = package.manifest.version;
    const auto registered = registry_.entries.find(id);
    if (registered == registry_.entries.end() || registered->second.source != package.source ||
        registered->second.activeVersion == version ||
        !WidgetPackageValidator::IsUuid(id) || !WidgetPackageValidator::IsSemVer(version))
    {
        error = "cannot quarantine the active or unowned package version";
        return false;
    }
    const auto source = paths_.installed / Utf8ToWide(id) / Utf8ToWide(version);
    if (source != package.root || PathContainsReparsePoint(source) ||
        PathContainsReparsePoint(paths_.quarantine))
    {
        error = "cannot quarantine an unsafe package version path";
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(source, paths_.quarantine /
        Utf8ToWide(id + "-" + version + "-rejected-" + GenerateUuid()), ec);
    if (ec)
    {
        error = "cannot quarantine rejected package version: " + ec.message();
        return false;
    }
    return Refresh(error);
}

bool WidgetPackageManager::Uninstall(const std::string& packageId,
    std::string& error)
{
    if (!registry_.entries.contains(packageId))
    {
        error = "package is not installed";
        return false;
    }
    const auto source = paths_.installed / Utf8ToWide(packageId);
    const auto quarantine = paths_.quarantine /
        (Utf8ToWide(packageId) + L"-uninstalled-" + Utf8ToWide(Timestamp()));
    std::error_code ec;
    std::filesystem::rename(source, quarantine, ec);
    if (ec)
    {
        error = "cannot move package to quarantine: " + ec.message();
        return false;
    }
    const RegistryEntry previous = registry_.entries[packageId];
    registry_.entries.erase(packageId);
    if (!registry_.Save(paths_.registry, error))
    {
        registry_.entries[packageId] = previous;
        std::filesystem::rename(quarantine, source, ec);
        return false;
    }
    return Refresh(error);
}

std::string WidgetPackageManager::Sha256File(
    const std::filesystem::path& path)
{
    return detail::HashPackageFile(path, false);
}

std::string WidgetPackageManager::GenerateUuid()
{
    GUID guid{};
    if (CoCreateGuid(&guid) != S_OK) return {};
    char buffer[37]{};
    std::snprintf(buffer, sizeof(buffer),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
        guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
        guid.Data4[6], guid.Data4[7]);
    return Lower(buffer);
}
}
