#include "widget_package.h"
#include "widget_package_manifest_fields.h"
#include "widget_package_read.h"

#include "common/json_value.h"
#include "data/atomic_file.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snowdesktop::widget
{
namespace
{
using detail::IsBcp47Tag;
using detail::JsonEscape;
using detail::Lower;
using detail::PathUtf8;
using detail::ReadInteger;
using detail::ReadPackageFile;
using detail::ReadString;
using detail::ReadStringArray;
using detail::Utf8ToWide;

bool AtomicWrite(const std::filesystem::path& target, const std::string& data,
    std::string& error)
{
    return atomic_file::WriteAll(target, data, {}, &error);
}

bool CopyArtifactWithProgress(const std::filesystem::path& source,
    const std::filesystem::path& target,
    const std::function<bool(std::uint64_t, std::uint64_t)>& progress,
    std::string& error, const std::string& expectedSha256)
{
    std::error_code ec;
    const auto total = std::filesystem::file_size(source, ec);
    if (ec || total > kMaxArchiveBytes)
    {
        error = "artifact is missing or exceeds 20 MiB";
        return false;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec)
    {
        error = "cannot create catalog package directory: " + ec.message();
        return false;
    }
    const auto temporary =
        std::filesystem::path(target.wstring() + L".uploading-" +
            Utf8ToWide(WidgetPackageManager::GenerateUuid()));
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!input || !output)
    {
        error = "cannot open artifact copy streams";
        return false;
    }
    auto reportProgress = [&](std::uint64_t complete)
    {
        if (!progress) return true;
        try { return progress(complete, total); }
        catch (...) { return false; }
    };
    if (!reportProgress(0))
    {
        error = "publication cancelled";
        output.close();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t complete = 0;
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        output.write(buffer.data(), count);
        complete += static_cast<std::uint64_t>(count);
        if (!output || !reportProgress(complete))
        {
            error = output ? "publication cancelled" :
                "cannot copy catalog artifact";
            output.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    output.flush();
    if (!input.eof() || !output)
    {
        error = "cannot finish catalog artifact copy";
        output.close();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    output.close();
    if (!output || WidgetPackageManager::Sha256File(temporary) != expectedSha256)
    {
        error = "cannot close artifact or copied content changed";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        // Another successful publication may already own this immutable name.
        // It is safe to reuse only the exact expected bytes, never overwrite it.
        if (WidgetPackageManager::Sha256File(target) == expectedSha256)
        {
            std::filesystem::remove(temporary, ec);
            return true;
        }
        error = "cannot atomically publish catalog artifact";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}
}

LocalCatalogPublisher::LocalCatalogPublisher(
    std::filesystem::path catalogDirectory,
    std::filesystem::path validationStaging)
    : catalogDirectory_(std::move(catalogDirectory)),
      validationStaging_(std::move(validationStaging)) {}
std::string LocalCatalogPublisher::ProviderId() const { return "local-catalog"; }
ProviderCapabilities LocalCatalogPublisher::Capabilities() const
{
    return { false, false, false, false, true, true };
}

PublishResult LocalCatalogPublisher::Publish(const PublishRequest& request)
{
    PublishResult result;
    if (!WidgetPackageValidator::IsUuid(request.artifact.packageId) ||
        !WidgetPackageValidator::IsSemVer(request.artifact.version))
    {
        result.error = "artifact identity is invalid";
        return result;
    }
    PackagePaths validationPaths;
    validationPaths.staging = validationStaging_;
    WidgetPackageManager validationManager(std::move(validationPaths));
    PackageManifest publishedManifest;
    const auto validation = validationManager.ValidateArchive(
        request.artifact.localPath, &publishedManifest);
    if (!validation.Ok() ||
        publishedManifest.id != request.artifact.packageId ||
        publishedManifest.version != request.artifact.version)
    {
        result.error = "publish artifact failed package validation";
        return result;
    }
    const std::string externalId = request.externalItemId.value_or(
        request.artifact.packageId);
    const std::string actualSha256 =
        WidgetPackageManager::Sha256File(request.artifact.localPath);
    if (actualSha256.empty())
    {
        result.error = "cannot hash the publish artifact";
        return result;
    }
    if (!request.artifact.sha256.empty() &&
        actualSha256 != Lower(request.artifact.sha256))
    {
        result.error = "artifact SHA-256 does not match the publish request";
        return result;
    }
    // Content-addressed artifacts are immutable. A failed index commit leaves
    // every previously published version paired with its original bytes.
    const auto relative = std::filesystem::path(L"packages") /
        Utf8ToWide(actualSha256 + ".snowwidget");
    const auto target = catalogDirectory_ / relative;
    struct CatalogRecord
    {
        std::string id;
        std::string slug;
        std::string version;
        std::string externalId;
        std::string name;
        std::string description;
        std::string author;
        std::string license;
        std::string minHostVersion;
        int schemaVersion = 0;
        int apiVersion = 0;
        int dataVersion = 0;
        bool confirmRemoval = false;
        std::vector<std::string> permissions;
        std::vector<std::string> optionalPermissions;
        std::vector<std::string> networkDomains;
        std::vector<std::string> requiredFeatures;
        std::vector<std::string> optionalFeatures;
        std::unordered_map<std::string, LocalizedMetadata> locales;
        std::vector<std::string> tags;
        std::string changeNotes;
        std::string artifact;
        std::string sha256;
    };
    std::vector<CatalogRecord> records;
    const auto catalogPath = catalogDirectory_ / L"catalog.json";
    std::string existingText;
    JsonValue existingRoot;
    const DWORD catalogAttributes = GetFileAttributesW(catalogPath.c_str());
    const bool catalogExists = catalogAttributes != INVALID_FILE_ATTRIBUTES;
    const DWORD catalogStatusError = catalogExists ? ERROR_SUCCESS : GetLastError();
    if (!catalogExists && catalogStatusError != ERROR_FILE_NOT_FOUND && catalogStatusError != ERROR_PATH_NOT_FOUND)
    {
        result.error = "cannot inspect existing catalog: " + std::to_string(catalogStatusError);
        return result;
    }
    if (catalogExists)
    {
        if (!ReadPackageFile(catalogPath, existingText, 8 * 1024 * 1024) ||
            !ParseJson(existingText, existingRoot) || !existingRoot.IsObject())
        {
            result.error = "cannot read or parse existing catalog";
            return result;
        }
        const auto* schema = existingRoot.Find("schemaVersion");
        const auto* entries = existingRoot.Find("packages");
        if (!schema || !schema->IsNumber() || schema->number != 1 || !entries || !entries->IsArray())
        {
            result.error = "existing catalog schema is invalid";
            return result;
        }
        if (const JsonValue* packages = existingRoot.Find("packages");
            packages && packages->IsArray())
        {
            for (const auto& value : packages->array)
            {
                if (!value.IsObject())
                {
                    result.error = "existing catalog entry is invalid";
                    return result;
                }
                CatalogRecord record;
                ReadString(value, "id", record.id);
                ReadString(value, "slug", record.slug);
                ReadString(value, "version", record.version);
                ReadString(value, "externalItemId", record.externalId);
                ReadString(value, "name", record.name);
                ReadString(value, "description", record.description);
                ReadString(value, "author", record.author);
                ReadString(value, "license", record.license);
                ReadString(value, "minHostVersion", record.minHostVersion);
                ReadInteger(value, "schemaVersion", record.schemaVersion);
                ReadInteger(value, "apiVersion", record.apiVersion);
                ReadInteger(value, "dataVersion", record.dataVersion);
                if (const auto* confirmation = value.Find("confirmRemoval");
                    confirmation && confirmation->IsBoolean())
                    record.confirmRemoval = confirmation->boolean;
                bool arraysValid = true;
                record.permissions =
                    ReadStringArray(value, "permissions", arraysValid);
                record.optionalPermissions =
                    ReadStringArray(value, "optionalPermissions",
                        arraysValid);
                record.networkDomains =
                    ReadStringArray(value, "networkDomains", arraysValid);
                record.requiredFeatures =
                    ReadStringArray(value, "requiredFeatures", arraysValid);
                record.optionalFeatures =
                    ReadStringArray(value, "optionalFeatures", arraysValid);
                if (const JsonValue* locales = value.Find("locales");
                    locales && locales->IsObject())
                {
                    for (const auto& [locale, metadata] : locales->object)
                    {
                        if (!IsBcp47Tag(locale) || !metadata.IsObject())
                            continue;
                        LocalizedMetadata localized;
                        ReadString(metadata, "title", localized.title);
                        ReadString(metadata, "description",
                            localized.description);
                        record.locales.emplace(
                            locale, std::move(localized));
                    }
                }
                record.tags = ReadStringArray(value, "tags", arraysValid);
                ReadString(value, "changeNotes", record.changeNotes);
                ReadString(value, "artifact", record.artifact);
                ReadString(value, "sha256", record.sha256);
                if (arraysValid && WidgetPackageValidator::IsUuid(record.id) &&
                    WidgetPackageValidator::IsSemVer(record.version) &&
                    !record.externalId.empty() && !record.artifact.empty() &&
                    !record.sha256.empty())
                    records.push_back(std::move(record));
                else
                {
                    result.error = "existing catalog entry has invalid identity or fields";
                    return result;
                }
            }
        }
    }
    std::erase_if(records, [&](const CatalogRecord& record) {
        return record.externalId == externalId &&
            record.version == request.artifact.version;
    });
    records.push_back({ request.artifact.packageId,
        publishedManifest.slug, request.artifact.version, externalId,
        request.title, request.description, publishedManifest.author,
        publishedManifest.license, publishedManifest.minHostVersion,
        publishedManifest.schemaVersion,
        publishedManifest.apiVersion, publishedManifest.dataVersion,
        publishedManifest.confirmRemoval,
        publishedManifest.permissions,
        publishedManifest.optionalPermissions,
        publishedManifest.networkDomains,
        publishedManifest.requiredFeatures,
        publishedManifest.optionalFeatures,
        publishedManifest.locales, request.tags, request.changeNotes,
        PathUtf8(relative), actualSha256 });
    std::sort(records.begin(), records.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.externalId, left.version) <
                std::tie(right.externalId, right.version);
        });

    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 1,\n  \"packages\": [";
    for (std::size_t index = 0; index < records.size(); ++index)
    {
        if (index) out << ',';
        const auto& record = records[index];
        out << "\n    {\"id\":\"" << JsonEscape(record.id)
            << "\",\"slug\":\"" << JsonEscape(record.slug)
            << "\",\"version\":\"" << JsonEscape(record.version)
            << "\",\"externalItemId\":\"" << JsonEscape(record.externalId)
            << "\",\"name\":\"" << JsonEscape(record.name)
            << "\",\"description\":\"" << JsonEscape(record.description)
            << "\",\"author\":\"" << JsonEscape(record.author)
            << "\",\"license\":\"" << JsonEscape(record.license)
            << "\",\"minHostVersion\":\""
            << JsonEscape(record.minHostVersion)
            << "\",\"schemaVersion\":" << record.schemaVersion
            << ",\"apiVersion\":" << record.apiVersion
            << ",\"dataVersion\":" << record.dataVersion
            << ",\"confirmRemoval\":" << (record.confirmRemoval ? "true" : "false")
            << ",\"permissions\":[";
        for (std::size_t permission = 0;
            permission < record.permissions.size(); ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(record.permissions[permission]) << '"';
        }
        out << "],\"optionalPermissions\":[";
        for (std::size_t permission = 0;
            permission < record.optionalPermissions.size(); ++permission)
        {
            if (permission) out << ',';
            out << '"' << JsonEscape(
                record.optionalPermissions[permission]) << '"';
        }
        out << "],\"networkDomains\":[";
        for (std::size_t domain = 0;
            domain < record.networkDomains.size(); ++domain)
        {
            if (domain) out << ',';
            out << '"' << JsonEscape(record.networkDomains[domain]) << '"';
        }
        out << "],\"requiredFeatures\":[";
        for (std::size_t feature = 0;
            feature < record.requiredFeatures.size(); ++feature)
        {
            if (feature) out << ',';
            out << '"' << JsonEscape(record.requiredFeatures[feature]) << '"';
        }
        out << "],\"optionalFeatures\":[";
        for (std::size_t feature = 0;
            feature < record.optionalFeatures.size(); ++feature)
        {
            if (feature) out << ',';
            out << '"' << JsonEscape(record.optionalFeatures[feature]) << '"';
        }
        out << "],\"locales\":{";
        std::vector<std::string> localeNames;
        localeNames.reserve(record.locales.size());
        for (const auto& [locale, metadata] : record.locales)
            localeNames.push_back(locale);
        std::sort(localeNames.begin(), localeNames.end());
        for (std::size_t locale = 0; locale < localeNames.size(); ++locale)
        {
            if (locale) out << ',';
            const auto& name = localeNames[locale];
            const auto& metadata = record.locales.at(name);
            out << '"' << JsonEscape(name) << "\":{\"title\":\""
                << JsonEscape(metadata.title)
                << "\",\"description\":\""
                << JsonEscape(metadata.description) << "\"}";
        }
        out << "},\"tags\":[";
        for (std::size_t tag = 0; tag < record.tags.size(); ++tag)
        {
            if (tag) out << ',';
            out << '"' << JsonEscape(record.tags[tag]) << '"';
        }
        out << "],\"changeNotes\":\"" << JsonEscape(record.changeNotes)
            << "\",\"artifact\":\"" << JsonEscape(record.artifact)
            << "\",\"sha256\":\"" << JsonEscape(record.sha256) << "\"}";
    }
    if (!records.empty()) out << '\n';
    out << "  ]\n}\n";
    std::string error;
    const auto catalogText = out.str();
    if (catalogText.size() > 8 * 1024 * 1024)
    {
        result.error = "catalog exceeds the readable 8 MiB limit";
        return result;
    }
    if (!CopyArtifactWithProgress(request.artifact.localPath, target,
        request.progress, result.error, actualSha256))
        return result;
    if (WidgetPackageManager::Sha256File(target) != actualSha256)
    {
        result.error = "copied artifact SHA-256 does not match";
        return result;
    }
    if (!AtomicWrite(catalogPath, catalogText, error))
    {
        result.error = error;
        return result;
    }
    result.ok = true;
    result.externalItemId = externalId;
    return result;
}
}
