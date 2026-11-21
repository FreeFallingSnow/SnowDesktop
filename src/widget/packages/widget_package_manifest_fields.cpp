#include "widget_package_manifest_fields.h"
#include "widget_package.h"
#include "widget_package_validation_report.h"
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
std::string JsonEscape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                out += "\\u00";
                out.push_back(hex[(ch >> 4) & 0x0f]);
                out.push_back(hex[ch & 0x0f]);
            }
            else out.push_back(static_cast<char>(ch));
            break;
        }
    }
    return out;
}

bool DecimalIdentifier(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char character)
        {
            return character >= '0' && character <= '9';
        });
}

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

// The caller retains its existing handling of fixed grid sizes.
bool ReadAspectGridSize(const JsonValue* value, int& columns, int& rows,
    std::optional<AspectGridSize>& aspect, std::string& error)
{
    aspect.reset();
    if (!value || !value->IsObject()) return true;
    const auto* ratio = value->Find("aspectRatio");
    if (!ratio) return true;
    const auto* c = value->Find("columns");
    const auto* r = value->Find("rows");
    const auto* fixed = r ? r : c;
    if (!ratio->IsNumber() || !std::isfinite(ratio->number) || ratio->number <= 0 ||
        (c != nullptr) == (r != nullptr) || !fixed || !fixed->IsNumber() ||
        !std::isfinite(fixed->number) || fixed->number != std::floor(fixed->number) ||
        fixed->number < 1 || fixed->number > 8)
    {
        error = "aspectRatio must be a positive finite width/height ratio with exactly one integer rows or columns from 1 to 8";
        return false;
    }
    aspect = AspectGridSize{ratio->number, r != nullptr};
    if (r) rows = static_cast<int>(r->number);
    else columns = static_cast<int>(c->number);
    const auto resolved = ResolveWidgetSize({columns, rows, aspect}, StandardWidgetSizeGridMetrics());
    columns = resolved.first;
    rows = resolved.second;
    return true;
}

void ResolvePreviewVariantDefaults(PreviewVariant& variant,
    const WidgetSizeDefinition& defaults, int minColumns, int minRows, int maxColumns, int maxRows)
{
    if (variant.columns == 0 && variant.rows == 0 && !variant.aspectSize)
        variant.aspectSize = defaults.aspect;
    if (variant.columns == 0) variant.columns = defaults.columns;
    if (variant.rows == 0) variant.rows = defaults.rows;
    if (variant.aspectSize)
    {
        const auto span = ResolveWidgetSize({variant.columns, variant.rows, variant.aspectSize},
            StandardWidgetSizeGridMetrics(), minColumns, minRows, maxColumns, maxRows);
        variant.columns = span.first;
        variant.rows = span.second;
    }
}

namespace
{
struct PreviewFieldReader
{
    const std::filesystem::path& manifestPath;
    ValidationReport* report;

    void Error(const char* code, const std::string& message) const
    {
        if (report) report->Add(ValidationSeverity::Error, code, manifestPath, message);
    }
};

void ReadPreviewStorage(const JsonValue* storage,
    std::unordered_map<std::string, std::string>& output,
    const PreviewFieldReader& reader,
    const std::string& path)
{
    if (!storage) return;
    if (!storage->IsObject())
    {
        reader.Error("manifest.previewStorage",
            path + " must be an object");
        return;
    }
    std::size_t totalPreviewBytes = 0;
    for (const auto& [key, value] : storage->object)
    {
        std::string serialized;
        if (value.IsString()) serialized = value.string;
        else if (value.IsBoolean())
            // Package diagnostics use JSON spelling. Lua preview storage uses numeric flags.
            serialized = reader.report ? (value.boolean ? "true" : "false")
                                       : (value.boolean ? "1" : "0");
        else if (value.IsNumber())
        {
            std::ostringstream stream;
            stream << value.number;
            serialized = stream.str();
        }
        else
        {
            reader.Error("manifest.previewStorageValue",
                path + " values must be strings, numbers, or booleans");
            continue;
        }
        totalPreviewBytes += key.size() + serialized.size();
        if (key.empty() || (reader.report && (key.size() > 128 || serialized.size() > 16 * 1024)))
        {
            reader.Error("manifest.previewStorageSize",
                path + " keys or values exceed their size limit");
            continue;
        }
        output[key] = std::move(serialized);
    }
    if (output.size() > 64 ||
        totalPreviewBytes > 64 * 1024)
        reader.Error("manifest.previewStorageQuota",
            path + " exceeds the 64-entry or 64-KiB limit");
}

void ReadPreviewStorageKeys(const JsonValue* storageKeys,
    const std::unordered_map<std::string, std::string>& storage,
    std::unordered_map<std::string, std::string>& output,
    const PreviewFieldReader& reader,
    const std::string& path)
{
    if (!storageKeys) return;
    if (!storageKeys->IsObject())
    {
        reader.Error("manifest.previewStorageKeys",
            path + " must be an object");
        return;
    }
    for (const auto& [storageKey, value] : storageKeys->object)
    {
        if (storageKey.empty() || (reader.report && storageKey.size() > 128) ||
            !value.IsString() || value.string.empty() ||
            (reader.report && value.string.size() > 256))
        {
            reader.Error("manifest.previewStorageKey",
                path + " must map valid preview storage names to non-empty localization keys");
            continue;
        }
        if (!storage.contains(storageKey))
        {
            reader.Error("manifest.previewStorageFallback",
                path + " references a storage name without an English fallback value: " +
                    storageKey);
            continue;
        }
        output[storageKey] = value.string;
    }
    if (output.size() > 64)
        reader.Error("manifest.previewStorageKeysQuota",
            path + " supports at most 64 entries");
}

void ReadPreviewVariants(const JsonValue* variants,
    std::vector<PreviewVariant>& output, const PreviewFieldReader& reader)
{
    if (!variants) return;
    if (!variants->IsArray())
    {
        reader.Error("manifest.previewVariants", "previewData.variants must be an array");
        return;
    }
    if (variants->array.size() > 4)
    {
        reader.Error("manifest.previewVariantsQuota",
            "previewData.variants supports at most four entries");
    }
    std::set<std::string> ids;
    for (const JsonValue& value : variants->array)
    {
        if (!reader.report && output.size() == 4) break;
        if (!value.IsObject())
        {
            reader.Error("manifest.previewVariant",
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
        const JsonValue* size = value.Find("size");
        if (reader.report)
            ReadSize(value, "size", variant.columns, variant.rows);
        else if (size && size->IsObject())
        {
            auto readLegacySpan = [&](const char* field, int& span) {
                const auto* number = size->Find(field);
                if (number && number->IsNumber() && std::isfinite(number->number))
                    span = std::clamp(static_cast<int>(number->number), 1, 8);
            };
            readLegacySpan("columns", variant.columns);
            readLegacySpan("rows", variant.rows);
        }
        std::string sizeError;
        if (!ReadAspectGridSize(size, variant.columns,
                variant.rows, variant.aspectSize, sizeError))
            reader.Error("manifest.aspectSize", sizeError);
        if (reader.report && (variant.id.empty() || variant.id.size() > 64 ||
            !std::all_of(variant.id.begin(), variant.id.end(),
                [](unsigned char ch) {
                    return std::isalnum(ch) || ch == '-' ||
                        ch == '_';
                }) || !ids.insert(variant.id).second))
        {
            reader.Error("manifest.previewVariantId",
                "preview variant ids must be unique and contain only letters, digits, '-' or '_'");
        }
        ReadPreviewStorage(value.Find("storage"),
            variant.storage, reader,
            "previewData.variants[].storage");
        ReadPreviewStorageKeys(value.Find("storageKeys"),
            variant.storage, variant.storageKeys, reader,
            "previewData.variants[].storageKeys");
        if (output.size() < 4)
            output.push_back(std::move(variant));
    }
}

void ReadPreviewData(const JsonValue& root, PreviewManifestFields output,
    const PreviewFieldReader& reader)
{
    const JsonValue* previewData = root.Find("previewData");
    if (!previewData) return;
    if (!previewData->IsObject())
    {
        reader.Error("manifest.previewData", "previewData must be an object");
        return;
    }

    ReadString(*previewData, "introduction", output.introduction);
    ReadString(*previewData, "introductionKey", output.introductionKey);
    ReadPreviewStorage(previewData->Find("storage"), output.storage,
        reader, "previewData.storage");
    ReadPreviewStorageKeys(previewData->Find("storageKeys"), output.storage,
        output.storageKeys, reader, "previewData.storageKeys");
    ReadPreviewVariants(previewData->Find("variants"), output.variants, reader);
}
}

void ReadRuntimePreviewFields(const JsonValue& root, PreviewManifestFields output)
{
    ReadPreviewData(root, output, {{}, nullptr});
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

    ReadPreviewData(root, {manifest.previewIntroduction, manifest.previewIntroductionKey,
        manifest.previewStorage, manifest.previewStorageKeys, manifest.previewVariants},
        {manifestPath, &report});
}
}
