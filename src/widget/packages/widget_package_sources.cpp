#include "widget_package_sources.h"
#include "widget_package_manager.h"
#include "widget_package_manifest_fields.h"
#include "widget_package_read.h"

#include "common/json_value.h"
#include "widget/api/widget_logical_slot_manifest.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snowdesktop::widget
{
namespace
{
using detail::IsBcp47Tag;
using detail::IsNewerValidSemVer;
using detail::IsSha256;
using detail::Lower;
using detail::PathUtf8;
using detail::ReadInteger;
using detail::ReadPackageFile;
using detail::ReadString;
using detail::ReadStringArray;
using detail::Utf8ToWide;

bool QueryMatches(const PackageManifest& manifest, const PackageQuery& query)
{
    const std::string needle = Lower(query.text);
    if (needle.empty()) return true;
    return Lower(manifest.name).find(needle) != std::string::npos ||
        Lower(manifest.slug).find(needle) != std::string::npos ||
        Lower(manifest.description).find(needle) != std::string::npos;
}

std::vector<PackageUpdate> FindAvailableUpdates(
    const std::vector<PackageDetails>& entries,
    const std::vector<PackageVersionRef>& installed)
{
    std::vector<PackageUpdate> updates;
    for (const auto& current : installed)
    {
        const PackageDetails* newest = nullptr;
        for (const auto& entry : entries)
        {
            if (entry.manifest.id != current.packageId ||
                !IsNewerValidSemVer(entry.manifest.version, current.version))
                continue;
            if (!newest || IsNewerValidSemVer(entry.manifest.version,
                    newest->manifest.version))
                newest = &entry;
        }
        if (newest) updates.push_back({ current, *newest });
    }
    return updates;
}
}

std::optional<PackageDetails> IWidgetPackageSource::GetVersionDetails(
    const std::string& externalItemId, const std::string& version,
    std::string& error)
{
    auto details = GetDetails(externalItemId, error);
    if (details && details->manifest.version == version)
        return details;
    if (details)
        error = "package source did not provide metadata for the requested version";
    return std::nullopt;
}

std::string BuiltinPackageSource::ProviderId() const { return "builtin"; }
ProviderCapabilities BuiltinPackageSource::Capabilities() const
{
    return { true, true, true, false, false, false };
}

ProviderStatus BuiltinPackageSource::Status()
{
    return { true, "built-in packages are available" };
}

std::vector<PackageDetails> BuiltinPackageSource::Query(
    const PackageQuery& query, std::string& error)
{
    error.clear();
    std::vector<PackageDetails> result;
    std::size_t matched = 0;
    for (const auto& package : manager_.ListPackages())
    {
        if (!package.builtin) continue;
        auto manifest = LocalizePackageManifest(package.manifest, query.locale);
        if (!QueryMatches(manifest, query)) continue;
        if (matched++ < query.offset) continue;
        if (result.size() >= query.limit) break;
        result.push_back({ std::move(manifest), package.source,
            { package.manifest.version }, false });
    }
    return result;
}

std::optional<PackageDetails> BuiltinPackageSource::GetDetails(
    const std::string& externalItemId, std::string& error)
{
    error.clear();
    for (const auto& package : manager_.ListPackages())
        if (package.builtin &&
            (package.manifest.id == externalItemId ||
             package.source.externalItemId == externalItemId))
            return PackageDetails{ package.manifest, package.source,
                { package.manifest.version }, false };
    error = "built-in package not found";
    return std::nullopt;
}

std::optional<PackageArtifact> BuiltinPackageSource::Materialize(
    const std::string& externalItemId, const std::string& version,
    const std::filesystem::path& destination, std::string& error)
{
    for (const auto& package : manager_.ListPackages())
    {
        if (!package.builtin || package.manifest.version != version ||
            (package.manifest.id != externalItemId &&
             package.source.externalItemId != externalItemId))
            continue;
        std::error_code ec;
        std::filesystem::copy(package.root, destination,
            std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { error = ec.message(); return std::nullopt; }
        return PackageArtifact{ destination, package.manifest.id,
            package.manifest.version, {} };
    }
    error = "built-in package version not found";
    return std::nullopt;
}

std::vector<PackageUpdate> BuiltinPackageSource::CheckUpdates(
    const std::vector<PackageVersionRef>& installed, std::string& error)
{
    PackageQuery query;
    query.limit = std::numeric_limits<std::size_t>::max();
    return FindAvailableUpdates(Query(query, error), installed);
}

LocalDirectorySource::LocalDirectorySource(std::filesystem::path root)
    : root_(std::move(root)) {}
std::string LocalDirectorySource::ProviderId() const { return "local-directory"; }
ProviderCapabilities LocalDirectorySource::Capabilities() const
{
    return { true, true, true, true, false, false };
}

ProviderStatus LocalDirectorySource::Status()
{
    std::error_code ec;
    if (!std::filesystem::is_directory(root_, ec))
        return { false, ec ? ec.message() : "directory is unavailable" };
    return { true, "directory is available" };
}

std::vector<PackageDetails> LocalDirectorySource::Query(
    const PackageQuery& query, std::string& error)
{
    error.clear();
    std::vector<PackageDetails> result;
    std::size_t matched = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root_, ec), end;
        !ec && it != end; it.increment(ec))
    {
        if (!it->is_directory(ec)) continue;
        PackageManifest manifest;
        if (!validator_.ValidateDirectory(it->path(), &manifest).Ok())
            continue;
        manifest = LocalizePackageManifest(std::move(manifest), query.locale);
        if (!QueryMatches(manifest, query)) continue;
        if (matched++ < query.offset) continue;
        if (result.size() >= query.limit) break;
        result.push_back({ manifest,
            { ProviderId(), PathUtf8(it->path().filename()) },
            { manifest.version }, false });
    }
    if (ec) error = ec.message();
    return result;
}

std::optional<PackageDetails> LocalDirectorySource::GetDetails(
    const std::string& externalItemId, std::string& error)
{
    PackageManifest manifest;
    const auto directory = root_ / Utf8ToWide(externalItemId);
    if (!validator_.ValidateDirectory(directory, &manifest).Ok())
    {
        error = "local package not found or invalid";
        return std::nullopt;
    }
    return PackageDetails{ manifest, { ProviderId(), externalItemId },
        { manifest.version }, false };
}

std::optional<PackageArtifact> LocalDirectorySource::Materialize(
    const std::string& externalItemId, const std::string& version,
    const std::filesystem::path& destination, std::string& error)
{
    const auto details = GetDetails(externalItemId, error);
    if (!details || details->manifest.version != version) return std::nullopt;
    std::error_code ec;
    std::filesystem::copy(root_ / Utf8ToWide(externalItemId), destination,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) { error = ec.message(); return std::nullopt; }
    return PackageArtifact{ destination, details->manifest.id, version, {} };
}

std::vector<PackageUpdate> LocalDirectorySource::CheckUpdates(
    const std::vector<PackageVersionRef>& installed, std::string& error)
{
    PackageQuery query;
    query.limit = std::numeric_limits<std::size_t>::max();
    return FindAvailableUpdates(Query(query, error), installed);
}

StaticCatalogSource::StaticCatalogSource(std::filesystem::path catalogPath)
    : catalogPath_(std::move(catalogPath)) {}
std::string StaticCatalogSource::ProviderId() const { return "static-catalog"; }
ProviderCapabilities StaticCatalogSource::Capabilities() const
{
    return { true, true, true, true, false, false };
}

ProviderStatus StaticCatalogSource::Status()
{
    std::vector<PackageDetails> entries;
    std::unordered_map<std::string, PackageArtifact> artifacts;
    std::string error;
    if (!ReadCatalog(entries, artifacts, error)) return { false, error };
    return { true, "catalog is available" };
}

bool StaticCatalogSource::ReadCatalog(std::vector<PackageDetails>& entries,
    std::unordered_map<std::string, PackageArtifact>& artifacts,
    std::string& error) const
{
    std::string text;
    if (!ReadPackageFile(catalogPath_, text, 8 * 1024 * 1024))
    {
        error = "catalog is offline or unreadable";
        return false;
    }
    JsonValue root;
    if (!ParseJson(text, root, &error) || !root.IsObject())
    {
        error = "invalid static catalog: " + error;
        return false;
    }
    const JsonValue* packages = root.Find("packages");
    if (!packages || !packages->IsArray()) return true;
    for (const auto& value : packages->array)
    {
        if (!value.IsObject()) continue;
        PackageDetails detail;
        ReadString(value, "id", detail.manifest.id);
        ReadString(value, "slug", detail.manifest.slug);
        ReadString(value, "version", detail.manifest.version);
        ReadString(value, "name", detail.manifest.name);
        ReadString(value, "description", detail.manifest.description);
        ReadString(value, "author", detail.manifest.author);
        ReadString(value, "license", detail.manifest.license);
        ReadString(value, "minHostVersion", detail.manifest.minHostVersion);
        ReadInteger(value, "schemaVersion", detail.manifest.schemaVersion);
        ReadInteger(value, "apiVersion", detail.manifest.apiVersion);
        ReadInteger(value, "dataVersion", detail.manifest.dataVersion);
        if (const auto* confirmation = value.Find("confirmRemoval");
            confirmation && confirmation->IsBoolean())
            detail.manifest.confirmRemoval = confirmation->boolean;
        bool arraysValid = true;
        detail.manifest.permissions =
            ReadStringArray(value, "permissions", arraysValid);
        detail.manifest.optionalPermissions =
            ReadStringArray(value, "optionalPermissions", arraysValid);
        detail.manifest.networkDomains =
            ReadStringArray(value, "networkDomains", arraysValid);
        detail.manifest.requiredFeatures =
            ReadStringArray(value, "requiredFeatures", arraysValid);
        detail.manifest.optionalFeatures =
            ReadStringArray(value, "optionalFeatures", arraysValid);
        {
            std::vector<snowdesktop::widget_runtime::
                LogicalSlotManifestError> slotErrors;
            if (!snowdesktop::widget_runtime::ParseLogicalSlotDeclarations(
                    value.Find("slots"), detail.manifest.logicalSlots,
                    slotErrors))
                arraysValid = false;
        }
        if (const JsonValue* locales = value.Find("locales");
            locales && locales->IsObject())
        {
            for (const auto& [locale, metadata] : locales->object)
            {
                if (!IsBcp47Tag(locale) || !metadata.IsObject()) continue;
                LocalizedMetadata localized;
                ReadString(metadata, "title", localized.title);
                ReadString(metadata, "description", localized.description);
                detail.manifest.locales.emplace(
                    locale, std::move(localized));
            }
        }
        ReadString(value, "externalItemId", detail.source.externalItemId);
        detail.source.providerId = ProviderId();
        std::string artifact;
        std::string sha256;
        ReadString(value, "artifact", artifact);
        ReadString(value, "sha256", sha256);
        if (detail.source.externalItemId.empty())
            detail.source.externalItemId = detail.manifest.id;
        if (arraysValid &&
            WidgetPackageValidator::IsUuid(detail.manifest.id) &&
            WidgetPackageValidator::IsSemVer(detail.manifest.version) &&
            WidgetPackageValidator::IsSafeRelativePath(Utf8ToWide(artifact)) &&
            IsSha256(sha256))
        {
            detail.versions.push_back(detail.manifest.version);
            artifacts[detail.source.externalItemId + "\n" +
                detail.manifest.version] = {
                    catalogPath_.parent_path() / Utf8ToWide(artifact),
                    detail.manifest.id, detail.manifest.version, Lower(sha256) };
            entries.push_back(std::move(detail));
        }
    }
    return true;
}

std::vector<PackageDetails> StaticCatalogSource::Query(
    const PackageQuery& query, std::string& error)
{
    std::vector<PackageDetails> entries;
    std::unordered_map<std::string, PackageArtifact> artifacts;
    if (!ReadCatalog(entries, artifacts, error)) return {};

    std::vector<PackageDetails> grouped;
    for (const auto& entry : entries)
    {
        auto existing = std::find_if(grouped.begin(), grouped.end(),
            [&](const PackageDetails& value)
            {
                return value.source.externalItemId ==
                    entry.source.externalItemId;
            });
        if (existing == grouped.end())
        {
            grouped.push_back(entry);
            continue;
        }
        if (std::find(existing->versions.begin(), existing->versions.end(),
                entry.manifest.version) == existing->versions.end())
            existing->versions.push_back(entry.manifest.version);
        if (IsNewerValidSemVer(entry.manifest.version, existing->manifest.version))
            existing->manifest = entry.manifest;
    }

    std::vector<PackageDetails> result;
    std::size_t matched = 0;
    for (auto entry : grouped)
    {
        entry.manifest =
            LocalizePackageManifest(std::move(entry.manifest), query.locale);
        if (!QueryMatches(entry.manifest, query)) continue;
        if (matched++ < query.offset) continue;
        if (result.size() >= query.limit) break;
        result.push_back(entry);
    }
    return result;
}

std::optional<PackageDetails> StaticCatalogSource::GetDetails(
    const std::string& externalItemId, std::string& error)
{
    std::vector<PackageDetails> entries;
    std::unordered_map<std::string, PackageArtifact> artifacts;
    if (!ReadCatalog(entries, artifacts, error)) return std::nullopt;
    std::optional<PackageDetails> result;
    for (const auto& entry : entries)
    {
        if (entry.source.externalItemId != externalItemId) continue;
        if (!result)
        {
            result = entry;
            continue;
        }
        if (std::find(result->versions.begin(), result->versions.end(),
                entry.manifest.version) == result->versions.end())
            result->versions.push_back(entry.manifest.version);
        if (IsNewerValidSemVer(entry.manifest.version, result->manifest.version))
            result->manifest = entry.manifest;
    }
    if (result) return result;
    error = "catalog item not found";
    return std::nullopt;
}

std::optional<PackageDetails> StaticCatalogSource::GetVersionDetails(
    const std::string& externalItemId, const std::string& version,
    std::string& error)
{
    std::vector<PackageDetails> entries;
    std::unordered_map<std::string, PackageArtifact> artifacts;
    if (!ReadCatalog(entries, artifacts, error)) return std::nullopt;
    for (const auto& entry : entries)
    {
        if (entry.source.externalItemId == externalItemId &&
            entry.manifest.version == version)
            return entry;
    }
    error = "catalog item version not found";
    return std::nullopt;
}

std::optional<PackageArtifact> StaticCatalogSource::Materialize(
    const std::string& externalItemId, const std::string& version,
    const std::filesystem::path& destination, std::string& error)
{
    std::vector<PackageDetails> entries;
    std::unordered_map<std::string, PackageArtifact> artifacts;
    if (!ReadCatalog(entries, artifacts, error)) return std::nullopt;
    for (const auto& entry : entries)
    {
        if (entry.source.externalItemId != externalItemId ||
            entry.manifest.version != version) continue;
        const auto artifact = artifacts.find(externalItemId + "\n" + version);
        if (artifact == artifacts.end()) break;
        std::error_code ec;
        std::filesystem::copy_file(artifact->second.localPath, destination,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { error = ec.message(); return std::nullopt; }
        const auto sha256 = WidgetPackageManager::Sha256File(destination);
        if (sha256 != artifact->second.sha256)
        {
            std::filesystem::remove(destination, ec);
            error = "catalog artifact SHA-256 mismatch";
            return std::nullopt;
        }
        return PackageArtifact{ destination, entry.manifest.id, version, sha256 };
    }
    error = "catalog artifact not found";
    return std::nullopt;
}

std::vector<PackageUpdate> StaticCatalogSource::CheckUpdates(
    const std::vector<PackageVersionRef>& installed, std::string& error)
{
    PackageQuery query;
    query.limit = std::numeric_limits<std::size_t>::max();
    return FindAvailableUpdates(Query(query, error), installed);
}
}
