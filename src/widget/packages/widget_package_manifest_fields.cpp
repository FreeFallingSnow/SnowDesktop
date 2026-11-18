#include "widget_package_manifest_fields.h"
#include "widget_package.h"
#include "widget_size_rules.h"

#include "common/json_value.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace snowdesktop::widget::detail
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool IsBcp47Tag(const std::string& locale)
{
    if (locale.size() < 2 || locale.size() > 63) return false;
    std::size_t begin = 0;
    int part = 0;
    while (begin < locale.size())
    {
        const auto end = locale.find('-', begin);
        const auto value = locale.substr(begin,
            end == std::string::npos ? locale.size() - begin : end - begin);
        if (value.empty() || value.size() > 8) return false;
        if (part == 0)
        {
            if (value.size() < 2 ||
                !std::all_of(value.begin(), value.end(),
                    [](unsigned char ch) { return std::isalpha(ch) != 0; }))
                return false;
        }
        else if (!std::all_of(value.begin(), value.end(),
            [](unsigned char ch) { return std::isalnum(ch) != 0; }))
            return false;
        ++part;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

bool IsSha256(std::string_view value)
{
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch)
        {
            return std::isxdigit(ch) != 0;
        });
}

bool ReadString(const JsonValue& object, const char* name, std::string& output)
{
    const JsonValue* value = object.Find(name);
    if (!value || !value->IsString()) return false;
    output = value->string;
    return true;
}

bool ReadInteger(const JsonValue& object, const char* name, int& output)
{
    const JsonValue* value = object.Find(name);
    if (!value || !value->IsNumber() || !std::isfinite(value->number) ||
        value->number != std::floor(value->number) ||
        value->number < static_cast<double>((std::numeric_limits<int>::min)()) ||
        value->number > static_cast<double>((std::numeric_limits<int>::max)()))
        return false;
    output = static_cast<int>(value->number);
    return true;
}

std::vector<std::string> ReadStringArray(const JsonValue& object,
    const char* name, bool& valid)
{
    std::vector<std::string> result;
    const JsonValue* value = object.Find(name);
    if (!value) return result;
    if (!value->IsArray())
    {
        valid = false;
        return result;
    }
    for (const auto& item : value->array)
    {
        if (!item.IsString())
        {
            valid = false;
            continue;
        }
        result.push_back(item.string);
    }
    return result;
}

void ReadSize(const JsonValue& root, const char* name, int& columns, int& rows)
{
    const JsonValue* value = root.Find(name);
    if (!value || !value->IsObject()) return;
    ReadInteger(*value, "columns", columns);
    ReadInteger(*value, "rows", rows);
}

namespace
{
void ReadPreviewStorage(const JsonValue* storage,
    std::unordered_map<std::string, std::string>& output,
    const std::filesystem::path& manifestPath, ValidationReport& report,
    const std::string& path)
{
    if (!storage) return;
    if (!storage->IsObject())
    {
        report.Add(ValidationSeverity::Error,
            "manifest.previewStorage", manifestPath,
            path + " must be an object");
        return;
    }
    std::size_t totalPreviewBytes = 0;
    for (const auto& [key, value] : storage->object)
    {
        std::string serialized;
        if (value.IsString()) serialized = value.string;
        else if (value.IsBoolean())
            serialized = value.boolean ? "true" : "false";
        else if (value.IsNumber())
        {
            std::ostringstream stream;
            stream << value.number;
            serialized = stream.str();
        }
        else
        {
            report.Add(ValidationSeverity::Error,
                "manifest.previewStorageValue", manifestPath,
                path + " values must be strings, numbers, or booleans");
            continue;
        }
        totalPreviewBytes += key.size() + serialized.size();
        if (key.empty() || key.size() > 128 ||
            serialized.size() > 16 * 1024)
        {
            report.Add(ValidationSeverity::Error,
                "manifest.previewStorageSize", manifestPath,
                path + " keys or values exceed their size limit");
            continue;
        }
        output[key] = std::move(serialized);
    }
    if (output.size() > 64 ||
        totalPreviewBytes > 64 * 1024)
        report.Add(ValidationSeverity::Error,
            "manifest.previewStorageQuota", manifestPath,
            path + " exceeds the 64-entry or 64-KiB limit");
}

void ReadPreviewStorageKeys(const JsonValue* storageKeys,
    const std::unordered_map<std::string, std::string>& storage,
    std::unordered_map<std::string, std::string>& output,
    const std::filesystem::path& manifestPath, ValidationReport& report,
    const std::string& path)
{
    if (!storageKeys) return;
    if (!storageKeys->IsObject())
    {
        report.Add(ValidationSeverity::Error,
            "manifest.previewStorageKeys", manifestPath,
            path + " must be an object");
        return;
    }
    for (const auto& [storageKey, value] : storageKeys->object)
    {
        if (storageKey.empty() || storageKey.size() > 128 ||
            !value.IsString() || value.string.empty() ||
            value.string.size() > 256)
        {
            report.Add(ValidationSeverity::Error,
                "manifest.previewStorageKey", manifestPath,
                path + " must map valid preview storage names to non-empty localization keys");
            continue;
        }
        if (!storage.contains(storageKey))
        {
            report.Add(ValidationSeverity::Error,
                "manifest.previewStorageFallback", manifestPath,
                path + " references a storage name without an English fallback value: " +
                    storageKey);
            continue;
        }
        output[storageKey] = value.string;
    }
    if (output.size() > 64)
        report.Add(ValidationSeverity::Error,
            "manifest.previewStorageKeysQuota", manifestPath,
            path + " supports at most 64 entries");
}

void ReadPreviewVariants(const JsonValue* variants,
    const std::filesystem::path& manifestPath, PackageManifest& manifest,
    ValidationReport& report)
{
    if (!variants) return;
    if (!variants->IsArray())
    {
        report.Add(ValidationSeverity::Error, "manifest.previewVariants",
            manifestPath, "previewData.variants must be an array");
        return;
    }
    if (variants->array.size() > 4)
    {
        report.Add(ValidationSeverity::Error,
            "manifest.previewVariantsQuota", manifestPath,
            "previewData.variants supports at most four entries");
    }
    std::set<std::string> ids;
    for (const JsonValue& value : variants->array)
    {
        if (!value.IsObject())
        {
            report.Add(ValidationSeverity::Error,
                "manifest.previewVariant", manifestPath,
                "each preview variant must be an object");
            continue;
        }
        PreviewVariant variant;
        ReadString(value, "id", variant.id);
        ReadString(value, "title", variant.title);
        ReadString(value, "titleKey", variant.titleKey);
        ReadString(value, "description", variant.description);
        ReadString(value, "descriptionKey",
            variant.descriptionKey);
        ReadSize(value, "size",
            variant.columns, variant.rows);
        std::string sizeError;
        if (!ReadAspectGridSize(value.Find("size"), variant.columns,
                variant.rows, variant.aspectSize, sizeError))
            report.Add(ValidationSeverity::Error,
                "manifest.aspectSize", manifestPath, sizeError);
        if (variant.id.empty() || variant.id.size() > 64 ||
            !std::all_of(variant.id.begin(), variant.id.end(),
                [](unsigned char ch) {
                    return std::isalnum(ch) || ch == '-' ||
                        ch == '_';
                }) || !ids.insert(variant.id).second)
        {
            report.Add(ValidationSeverity::Error,
                "manifest.previewVariantId", manifestPath,
                "preview variant ids must be unique and contain only letters, digits, '-' or '_'");
        }
        ReadPreviewStorage(value.Find("storage"),
            variant.storage, manifestPath, report,
            "previewData.variants[].storage");
        ReadPreviewStorageKeys(value.Find("storageKeys"),
            variant.storage, variant.storageKeys, manifestPath, report,
            "previewData.variants[].storageKeys");
        if (manifest.previewVariants.size() < 4)
            manifest.previewVariants.push_back(
                std::move(variant));
    }
}
}

void ReadPreviewManifestFields(const JsonValue& root,
    const std::filesystem::path& manifestPath, PackageManifest& manifest,
    ValidationReport& report)
{
    if (root.Find("previewGallery"))
    {
        bool valid = true;
        manifest.previewGallery = ReadStringArray(root, "previewGallery", valid);
        if (!valid || manifest.previewGallery->size() > 7)
            report.Add(ValidationSeverity::Error, "manifest.previewGallery", manifestPath,
                "previewGallery must be an array of at most seven image paths");
    }

    const JsonValue* previewData = root.Find("previewData");
    if (!previewData) return;
    if (!previewData->IsObject())
    {
        report.Add(ValidationSeverity::Error, "manifest.previewData",
            manifestPath, "previewData must be an object");
        return;
    }

    ReadString(*previewData, "introduction", manifest.previewIntroduction);
    ReadString(*previewData, "introductionKey", manifest.previewIntroductionKey);
    ReadPreviewStorage(previewData->Find("storage"), manifest.previewStorage,
        manifestPath, report, "previewData.storage");
    ReadPreviewStorageKeys(previewData->Find("storageKeys"), manifest.previewStorage,
        manifest.previewStorageKeys, manifestPath, report, "previewData.storageKeys");
    ReadPreviewVariants(previewData->Find("variants"), manifestPath, manifest, report);
}
}
