#include "widget_package.h"
#include "widget_package_manifest_fields.h"
#include "widget_package_read.h"
#include "widget_catalog_refresh.h"

#include "common/json_value.h"
#include "data/atomic_file.h"
#include "common/language_fallback.h"
#include "widget/api/widget_logical_slot_manifest.h"
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
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>

namespace snowdesktop::widget
{
namespace
{
using detail::DecimalIdentifier;
using detail::HasReparsePoint;
using detail::IsBcp47Tag;
using detail::IsSha256;
using detail::JsonEscape;
using detail::Lower;
using detail::PathUtf8;
using detail::ReadInteger;
using detail::ReadPackageFile;
using detail::ReadString;
using detail::ReadStringArray;
using detail::StartsWithPath;
using detail::Timestamp;
using detail::Utf8ToWide;
using detail::WideToUtf8;

bool AtomicWrite(const std::filesystem::path& target, const std::string& data,
    std::string& error)
{
    return atomic_file::WriteAll(target, data, {}, &error);
}

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

std::string ManifestJson(const PackageManifest& manifest)
{
    auto writeSize = [](std::ostringstream& out, int columns, int rows,
        const std::optional<AspectGridSize>& aspect) {
        out << '{';
        if (aspect)
            out << (aspect->fixedRows ? "\"rows\": " : "\"columns\": ")
                << (aspect->fixedRows ? rows : columns)
                << ", \"aspectRatio\": " << std::setprecision(17) << aspect->ratio;
        else out << "\"columns\": " << columns << ", \"rows\": " << rows;
        out << '}';
    };
    auto appendArray = [](std::ostringstream& out,
        const std::vector<std::string>& values) {
        out << '[';
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i) out << ", ";
            out << '"' << JsonEscape(values[i]) << '"';
        }
        out << ']';
    };
    auto appendStringMap = [](std::ostringstream& out,
        const std::unordered_map<std::string, std::string>& values) {
        bool first = true;
        std::vector<std::pair<std::string, std::string>> sorted(
            values.begin(), values.end());
        std::sort(sorted.begin(), sorted.end());
        for (const auto& [key, value] : sorted)
        {
            if (!first) out << ", ";
            first = false;
            out << '"' << JsonEscape(key) << "\": \""
                << JsonEscape(value) << '"';
        }
    };
    std::ostringstream out;
    out << "{\n"
        << "  \"schemaVersion\": " << manifest.schemaVersion << ",\n"
        << "  \"id\": \"" << JsonEscape(manifest.id) << "\",\n"
        << "  \"slug\": \"" << JsonEscape(manifest.slug) << "\",\n"
        << "  \"version\": \"" << JsonEscape(manifest.version) << "\",\n"
        << "  \"apiVersion\": " << manifest.apiVersion << ",\n"
        << "  \"dataVersion\": " << manifest.dataVersion << ",\n"
        << "  \"confirmRemoval\": " << (manifest.confirmRemoval ? "true" : "false") << ",\n"
        << "  \"entry\": \"" << JsonEscape(manifest.entry) << "\",\n"
        << "  \"minHostVersion\": \"" << JsonEscape(manifest.minHostVersion) << "\",\n"
        << "  \"name\": \"" << JsonEscape(manifest.name) << "\",\n"
        << "  \"description\": \"" << JsonEscape(manifest.description) << "\",\n"
        << "  \"author\": \"" << JsonEscape(manifest.author) << "\",\n"
        << "  \"license\": \"" << JsonEscape(manifest.license) << "\",\n"
        << "  \"preview\": \"" << JsonEscape(manifest.preview) << "\",\n";
    if (manifest.previewGallery)
    {
        out << "  \"previewGallery\": [";
        for (std::size_t index = 0; index < manifest.previewGallery->size(); ++index)
        {
            if (index) out << ',';
            out << '"' << JsonEscape((*manifest.previewGallery)[index]) << '"';
        }
        out << "],\n";
    }
    out << "  \"previewData\": {\"storage\": {";
    appendStringMap(out, manifest.previewStorage);
    out << "}, \"storageKeys\": {";
    appendStringMap(out, manifest.previewStorageKeys);
    out << "}, \"introduction\": \""
        << JsonEscape(manifest.previewIntroduction)
        << "\", \"introductionKey\": \""
        << JsonEscape(manifest.previewIntroductionKey)
        << "\", \"variants\": [";
    for (std::size_t i = 0; i < manifest.previewVariants.size(); ++i)
    {
        if (i) out << ", ";
        const PreviewVariant& variant = manifest.previewVariants[i];
        out << "{\"id\": \"" << JsonEscape(variant.id)
            << "\", \"title\": \"" << JsonEscape(variant.title)
            << "\", \"titleKey\": \"" << JsonEscape(variant.titleKey)
            << "\", \"description\": \""
            << JsonEscape(variant.description)
            << "\", \"descriptionKey\": \""
            << JsonEscape(variant.descriptionKey)
            << "\", \"size\": ";
        writeSize(out, variant.columns, variant.rows, variant.aspectSize);
        out << ", \"storage\": {";
        appendStringMap(out, variant.storage);
        out << "}, \"storageKeys\": {";
        appendStringMap(out, variant.storageKeys);
        out << "}}";
    }
    out << "]},\n  \"creationPresets\": [";
    for (std::size_t i = 0; i < manifest.creationPresets.size(); ++i)
    {
        if (i) out << ", ";
        const auto& preset = manifest.creationPresets[i];
        out << "{\"id\": \"" << JsonEscape(preset.id)
            << "\", \"title\": \"" << JsonEscape(preset.title)
            << "\", \"titleKey\": \"" << JsonEscape(preset.titleKey)
            << "\", \"values\": {";
        appendStringMap(out, preset.values);
        out << '}';
        if (preset.size)
        {
            out << ", \"size\": ";
            writeSize(out, preset.size->columns, preset.size->rows, preset.size->aspect);
        }
        out << '}';
    }
    out << "],\n  \"defaultSize\": ";
    writeSize(out, manifest.defaultColumns, manifest.defaultRows, manifest.defaultAspectSize);
    out << ",\n  \"minSize\": {\"columns\": " << manifest.minColumns
        << ", \"rows\": " << manifest.minRows << "},\n"
        << "  \"maxSize\": {\"columns\": " << manifest.maxColumns
        << ", \"rows\": " << manifest.maxRows << "},\n"
        << "  \"resources\": {";
    {
        bool first = true;
        std::vector<std::string> resourceNames;
        resourceNames.reserve(manifest.resources.size());
        for (const auto& [name, resource] : manifest.resources)
        {
            (void)resource;
            resourceNames.push_back(name);
        }
        std::sort(resourceNames.begin(), resourceNames.end());
        for (const auto& name : resourceNames)
        {
            const auto& resource = manifest.resources.at(name);
            if (!first) out << ", ";
            first = false;
            out << '"' << JsonEscape(name) << "\": {\"type\": \""
                << JsonEscape(resource.type) << "\", \"path\": \""
                << JsonEscape(resource.path) << "\", \"license\": \""
                << JsonEscape(resource.license) << "\"}";
        }
    }
    out << "},\n"
        << "  \"slots\": "
        << snowdesktop::widget_runtime::SerializeLogicalSlotDeclarations(
            manifest.logicalSlots) << ",\n"
        << "  \"permissions\": ";
    appendArray(out, manifest.permissions);
    out << ",\n  \"optionalPermissions\": ";
    appendArray(out, manifest.optionalPermissions);
    out << ",\n  \"networkDomains\": ";
    appendArray(out, manifest.networkDomains);
    out << ",\n  \"requiredFeatures\": ";
    appendArray(out, manifest.requiredFeatures);
    out << ",\n  \"optionalFeatures\": ";
    appendArray(out, manifest.optionalFeatures);
    out << "\n}\n";
    return out.str();
}

std::uint32_t Crc32(const unsigned char* data, std::size_t size)
{
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void Write16(std::ostream& output, std::uint16_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
    };
    output.write(bytes, 2);
}

void Write32(std::ostream& output, std::uint32_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    output.write(bytes, 4);
}

std::uint16_t Read16(const unsigned char* data)
{
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t Read32(const unsigned char* data)
{
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

struct ZipItem
{
    std::string path;
    std::string data;
    std::uint32_t crc = 0;
    std::uint32_t offset = 0;
};

bool WriteStoreZip(const std::filesystem::path& root,
    const std::filesystem::path& output, std::string& error,
    std::string& sha256)
{
    std::vector<ZipItem> items;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
        !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec)) continue;
        ZipItem item;
        item.path = PathUtf8(std::filesystem::relative(it->path(), root, ec));
        if (ec || item.path.empty())
        {
            error = "cannot create archive-relative path";
            return false;
        }
        std::replace(item.path.begin(), item.path.end(), '\\', '/');
        if (!ReadPackageFile(it->path(), item.data, kMaxExtractedBytes))
        {
            error = "cannot read package file: " + item.path;
            return false;
        }
        item.crc = Crc32(reinterpret_cast<const unsigned char*>(item.data.data()),
            item.data.size());
        items.push_back(std::move(item));
    }
    if (ec)
    {
        error = "cannot enumerate package: " + ec.message();
        return false;
    }
    std::sort(items.begin(), items.end(),
        [](const ZipItem& a, const ZipItem& b) { return a.path < b.path; });

    const auto absoluteOutput = std::filesystem::absolute(output, ec).lexically_normal();
    if (ec)
    {
        error = "cannot resolve archive output: " + ec.message();
        return false;
    }
    std::filesystem::create_directories(absoluteOutput.parent_path(), ec);
    if (ec)
    {
        error = "cannot create archive directory: " + ec.message();
        return false;
    }
    const auto temporary = absoluteOutput.wstring() + L".tmp-" +
        Utf8ToWide(WidgetPackageManager::GenerateUuid());
    struct TemporaryArchive
    {
        std::filesystem::path path;
        ~TemporaryArchive() noexcept
        {
            try { if (!path.empty()) { std::error_code ignored; std::filesystem::remove(path, ignored); } }
            catch (...) {}
        }
    } cleanup{temporary};
    std::ofstream zip(temporary, std::ios::binary | std::ios::trunc);
    if (!zip)
    {
        error = "cannot create archive";
        return false;
    }
    for (auto& item : items)
    {
        const auto position = zip.tellp();
        if (position < 0 || position > std::numeric_limits<std::uint32_t>::max() ||
            item.data.size() > std::numeric_limits<std::uint32_t>::max() ||
            item.path.size() > std::numeric_limits<std::uint16_t>::max())
        {
            error = "package is too large for ZIP32";
            return false;
        }
        item.offset = static_cast<std::uint32_t>(position);
        Write32(zip, 0x04034b50);
        Write16(zip, 20);
        Write16(zip, 0x0800);
        Write16(zip, 0);
        Write16(zip, 0); Write16(zip, 0);
        Write32(zip, item.crc);
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write16(zip, static_cast<std::uint16_t>(item.path.size()));
        Write16(zip, 0);
        zip.write(item.path.data(), static_cast<std::streamsize>(item.path.size()));
        zip.write(item.data.data(), static_cast<std::streamsize>(item.data.size()));
    }
    const auto centralOffsetValue = zip.tellp();
    if (centralOffsetValue < 0 ||
        centralOffsetValue > std::numeric_limits<std::uint32_t>::max())
    {
        error = "package is too large for ZIP32";
        return false;
    }
    const auto centralOffset = static_cast<std::uint32_t>(centralOffsetValue);
    for (const auto& item : items)
    {
        Write32(zip, 0x02014b50);
        Write16(zip, 20); Write16(zip, 20);
        Write16(zip, 0x0800); Write16(zip, 0);
        Write16(zip, 0); Write16(zip, 0);
        Write32(zip, item.crc);
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write16(zip, static_cast<std::uint16_t>(item.path.size()));
        Write16(zip, 0); Write16(zip, 0); Write16(zip, 0); Write16(zip, 0);
        Write32(zip, 0);
        Write32(zip, item.offset);
        zip.write(item.path.data(), static_cast<std::streamsize>(item.path.size()));
    }
    const auto endValue = zip.tellp();
    if (endValue < 0 || endValue > std::numeric_limits<std::uint32_t>::max() ||
        items.size() > std::numeric_limits<std::uint16_t>::max())
    {
        error = "package is too large for ZIP32";
        return false;
    }
    const auto centralSize = static_cast<std::uint32_t>(endValue) - centralOffset;
    Write32(zip, 0x06054b50);
    Write16(zip, 0); Write16(zip, 0);
    Write16(zip, static_cast<std::uint16_t>(items.size()));
    Write16(zip, static_cast<std::uint16_t>(items.size()));
    Write32(zip, centralSize); Write32(zip, centralOffset); Write16(zip, 0);
    zip.flush();
    if (!zip)
    {
        error = "cannot finish archive";
        zip.close();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    zip.close();
    const auto bytes = std::filesystem::file_size(temporary, ec);
    if (!zip || ec || bytes > kMaxArchiveBytes)
    {
        error = !zip ? "cannot close archive" : ec ? "cannot measure archive" :
            "exported package exceeds 20 MiB";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    const auto digest = WidgetPackageManager::Sha256File(temporary);
    if (digest.empty())
    {
        error = "cannot hash archive";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), absoluteOutput.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "cannot atomically publish archive";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    sha256 = digest;
    cleanup.path.clear();
    return true;
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

bool QueryMatches(const PackageManifest& manifest, const PackageQuery& query)
{
    const std::string needle = Lower(query.text);
    if (needle.empty()) return true;
    return Lower(manifest.name).find(needle) != std::string::npos ||
        Lower(manifest.slug).find(needle) != std::string::npos ||
        Lower(manifest.description).find(needle) != std::string::npos;
}

std::vector<std::string_view> SplitIdentifiers(std::string_view value)
{
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (begin <= value.size())
    {
        const auto end = value.find('.', begin);
        parts.push_back(value.substr(begin,
            end == std::string_view::npos ? value.size() - begin : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return parts;
}

int CompareNumericIdentifier(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    if (left == right) return 0;
    return left < right ? -1 : 1;
}

bool IsNumericIdentifier(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool IsNewerSemVer(std::string_view candidate, std::string_view current)
{
    const auto candidatePlus = candidate.find('+');
    const auto currentPlus = current.find('+');
    candidate = candidate.substr(0, candidatePlus);
    current = current.substr(0, currentPlus);

    const auto candidateDash = candidate.find('-');
    const auto currentDash = current.find('-');
    const auto candidateCore = SplitIdentifiers(candidate.substr(0, candidateDash));
    const auto currentCore = SplitIdentifiers(current.substr(0, currentDash));
    for (std::size_t i = 0; i < 3; ++i)
    {
        const int comparison =
            CompareNumericIdentifier(candidateCore[i], currentCore[i]);
        if (comparison != 0) return comparison > 0;
    }

    const bool candidateHasPre = candidateDash != std::string_view::npos;
    const bool currentHasPre = currentDash != std::string_view::npos;
    if (candidateHasPre != currentHasPre) return !candidateHasPre;
    if (!candidateHasPre) return false;

    const auto candidatePre =
        SplitIdentifiers(candidate.substr(candidateDash + 1));
    const auto currentPre = SplitIdentifiers(current.substr(currentDash + 1));
    const auto count = std::min(candidatePre.size(), currentPre.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        if (candidatePre[i] == currentPre[i]) continue;
        const bool candidateNumeric = IsNumericIdentifier(candidatePre[i]);
        const bool currentNumeric = IsNumericIdentifier(currentPre[i]);
        if (candidateNumeric != currentNumeric) return !candidateNumeric;
        if (candidateNumeric)
            return CompareNumericIdentifier(candidatePre[i], currentPre[i]) > 0;
        return candidatePre[i] > currentPre[i];
    }
    return candidatePre.size() > currentPre.size();
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
                !IsNewerSemVer(entry.manifest.version, current.version))
                continue;
            if (!newest || IsNewerSemVer(entry.manifest.version,
                    newest->manifest.version))
                newest = &entry;
        }
        if (newest) updates.push_back({ current, *newest });
    }
    return updates;
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

bool WidgetPackageValidator::IsNewerSemVer(
    std::string_view candidate, std::string_view current)
{
    if (!IsSemVer(candidate) || !IsSemVer(current))
        return false;
    return snowdesktop::widget::IsNewerSemVer(candidate, current);
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
        !IsNewerSemVer(manifest.version, existing->second.activeVersion) &&
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

bool WidgetPackageManager::ExtractArchive(
    const std::filesystem::path& archive,
    const std::filesystem::path& destination, ValidationReport& report,
    std::string& error, bool* malformedArchive) const
{
    const auto malformed = [&](std::string message) {
        if (malformedArchive) *malformedArchive = true;
        error = std::move(message);
        return false;
    };
    std::string data;
    if (!ReadPackageFile(archive, data, kMaxArchiveBytes))
    {
        error = "cannot read package archive";
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t position = 0;
    std::set<std::string> names;
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    while (position + 4 <= data.size() && Read32(bytes + position) == 0x04034b50)
    {
        if (position + 30 > data.size())
            return malformed("truncated ZIP local header");
        const std::uint16_t flags = Read16(bytes + position + 6);
        const std::uint16_t method = Read16(bytes + position + 8);
        const std::uint32_t expectedCrc = Read32(bytes + position + 14);
        const std::uint32_t compressedSize = Read32(bytes + position + 18);
        const std::uint32_t uncompressedSize = Read32(bytes + position + 22);
        const std::uint16_t nameSize = Read16(bytes + position + 26);
        const std::uint16_t extraSize = Read16(bytes + position + 28);
        if ((flags & 0x0001) || (flags & 0x0008) || method != 0)
            return malformed("v1 accepts unencrypted ZIP entries using the store method");
        const std::size_t payload = position + 30 + nameSize + extraSize;
        if (payload > data.size() ||
            compressedSize > data.size() - payload ||
            compressedSize != uncompressedSize)
            return malformed("invalid ZIP entry bounds");
        const std::string name(data.data() + position + 30, nameSize);
        if (name.find('\0') != std::string::npos || name.ends_with('/'))
        {
            position = payload + compressedSize;
            continue;
        }
        const std::filesystem::path relative(Utf8ToWide(name));
        const std::string canonicalName =
            Lower(PathUtf8(relative.lexically_normal()));
        if (!WidgetPackageValidator::IsSafeRelativePath(relative) ||
            canonicalName.empty() || !names.insert(canonicalName).second)
            return malformed("archive contains an unsafe or duplicate path: " + name);
        ++report.fileCount;
        report.totalBytes += uncompressedSize;
        if (report.fileCount > kMaxPackageFiles ||
            report.totalBytes > kMaxExtractedBytes)
            return malformed("archive exceeds extraction quotas");
        const auto target = destination / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec)
        {
            error = "cannot create extracted directory: " + ec.message();
            return false;
        }
        const auto* payloadBytes = bytes + payload;
        if (Crc32(payloadBytes, uncompressedSize) != expectedCrc)
            return malformed("archive entry CRC mismatch: " + name);
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(payloadBytes),
            static_cast<std::streamsize>(uncompressedSize));
        if (!file)
        {
            error = "cannot extract archive entry: " + name;
            return false;
        }
        position = payload + compressedSize;
    }
    if (report.fileCount == 0)
        return malformed("archive contains no files");
    return true;
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

bool WidgetPackageManager::ExportArchive(const std::string& packageId,
    const std::filesystem::path& output, PackageArtifact& artifact,
    ValidationReport& report, std::string& error) const
{
    const auto package = Resolve(packageId);
    if (!package)
    {
        error = "package is not installed or is disabled";
        return false;
    }
    report = validator_.ValidateDirectory(package->root);
    if (!report.Ok())
    {
        error = "package validation failed";
        return false;
    }
    if (Lower(output.extension().string()) != ".snowwidget")
    {
        error = "output must use the .snowwidget extension";
        return false;
    }
    std::string digest;
    if (!WriteStoreZip(package->root, output, error, digest)) return false;
    artifact.localPath = output;
    artifact.packageId = package->manifest.id;
    artifact.version = package->manifest.version;
    artifact.sha256 = std::move(digest);
    return true;
}

bool WidgetPackageManager::ExportDirectory(
    const std::filesystem::path& source,
    const std::filesystem::path& output, PackageArtifact& artifact,
    ValidationReport& report, std::string& error) const
{
    PackageManifest manifest;
    report = validator_.ValidateDirectory(source, &manifest);
    if (!report.Ok())
    {
        error = "package validation failed";
        return false;
    }
    if (Lower(output.extension().string()) != ".snowwidget")
    {
        error = "output must use the .snowwidget extension";
        return false;
    }
    std::string digest;
    if (!WriteStoreZip(source, output, error, digest)) return false;
    artifact.localPath = output;
    artifact.packageId = manifest.id;
    artifact.version = manifest.version;
    artifact.sha256 = std::move(digest);
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

bool WidgetPackageManager::CreateDevelopmentProject(
    const std::string& packageId, std::filesystem::path& projectRoot,
    std::string& error)
{
    projectRoot.clear();
    const auto existingDevelopment = std::find_if(
        packages_->begin(), packages_->end(), [&](const auto& package)
        {
            return package.development && package.manifest.id == packageId;
        });
    if (existingDevelopment != packages_->end())
    {
        error = "a development project already exists for this component";
        return false;
    }

    const auto installed = std::find_if(
        packages_->begin(), packages_->end(), [&](const auto& package)
        {
            return package.manifest.id == packageId && package.active &&
                !package.builtin && !package.development;
        });
    if (installed == packages_->end())
    {
        error = "the installed component is unavailable";
        return false;
    }

    PackageManifest sourceManifest;
    const ValidationReport sourceReport = validator_.ValidateDirectory(
        installed->root, &sourceManifest);
    if (!sourceReport.Ok() || sourceManifest.id != packageId)
    {
        error = "the installed component failed validation";
        return false;
    }

    const std::filesystem::path staging =
        CreateStagingPath("development");
    std::error_code ec;
    auto discardStaging = [&]
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(staging, cleanupError);
    };
    if (!CopyPackageTree(installed->root, staging, error))
    {
        discardStaging();
        return false;
    }

    PackageManifest copiedManifest;
    const ValidationReport copiedReport = validator_.ValidateDirectory(
        staging, &copiedManifest);
    if (!copiedReport.Ok() || copiedManifest.id != sourceManifest.id ||
        copiedManifest.version != sourceManifest.version)
    {
        discardStaging();
        error = "the development copy failed validation";
        return false;
    }

    const std::wstring baseName = Utf8ToWide(
        copiedManifest.slug.empty() ? copiedManifest.id : copiedManifest.slug);
    std::filesystem::path destination = paths_.development / baseName;
    for (unsigned suffix = 2; std::filesystem::exists(destination, ec);
        ++suffix)
    {
        if (suffix > 10000)
        {
            destination = paths_.development /
                (baseName + L"-" + Utf8ToWide(GenerateUuid()));
            break;
        }
        destination = paths_.development /
            (baseName + L"-" + std::to_wstring(suffix));
    }
    if (ec)
    {
        discardStaging();
        error = "cannot inspect the development workspace: " + ec.message();
        return false;
    }

    std::filesystem::rename(staging, destination, ec);
    if (ec)
    {
        discardStaging();
        error = "cannot create the development project: " + ec.message();
        return false;
    }
    if (!Refresh(error))
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(destination, cleanupError);
        std::string refreshError;
        Refresh(refreshError);
        return false;
    }

    const auto created = std::find_if(
        packages_->begin(), packages_->end(), [&](const auto& package)
        {
            return package.development && package.manifest.id == packageId &&
                package.root == destination;
        });
    if (created == packages_->end())
    {
        std::filesystem::remove_all(destination, ec);
        std::string refreshError;
        Refresh(refreshError);
        error = "the development project could not be loaded";
        return false;
    }
    projectRoot = destination;
    return true;
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
        auto manifest = LocalizedManifest(package.manifest, query.locale);
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
        manifest = LocalizedManifest(std::move(manifest), query.locale);
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
        if (IsNewerSemVer(entry.manifest.version, existing->manifest.version))
            existing->manifest = entry.manifest;
    }

    std::vector<PackageDetails> result;
    std::size_t matched = 0;
    for (auto entry : grouped)
    {
        entry.manifest =
            LocalizedManifest(std::move(entry.manifest), query.locale);
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
        if (IsNewerSemVer(entry.manifest.version, result->manifest.version))
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
