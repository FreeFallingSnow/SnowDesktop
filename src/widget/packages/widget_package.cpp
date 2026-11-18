#include "widget_package.h"
#include "widget_package_manifest_fields.h"
#include "widget_package_read.h"

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
using detail::PathContainsReparsePoint;
using detail::PathUtf8;
using detail::Timestamp;
using detail::Utf8ToWide;
using detail::WideToUtf8;

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
