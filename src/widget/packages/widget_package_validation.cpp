#include "widget_package_validation.h"
#include "widget_audio_format.h"
#include "widget_package.h"
#include "widget_package_limits.h"
#include "widget_package_manager.h"
#include "widget_package_manifest_fields.h"
#include "widget_package_read.h"

#include "common/json_value.h"
#include "widget/api/widget_logical_slot_manifest.h"
#include "widget/permissions/widget_permission_broker.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace snowdesktop::widget
{
namespace
{
using detail::HasReparsePoint;
using detail::IsBcp47Tag;
using detail::IsSha256;
using detail::Lower;
using detail::PathUtf8;
using detail::ReadAspectGridSize;
using detail::ReadInteger;
using detail::ReadPackageFile;
using detail::ReadSize;
using detail::ReadString;
using detail::ReadStringArray;
using detail::StartsWithPath;
using detail::Utf8ToWide;

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

bool IsResourceName(std::string_view value)
{
    if (value.empty() || value.size() > 64 ||
        !std::islower(static_cast<unsigned char>(value.front())))
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::islower(ch) || std::isdigit(ch) || ch == '-' || ch == '_';
    });
}

std::uint16_t ReadBig16(const std::string& data, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<unsigned char>(data[offset]) << 8) |
        static_cast<unsigned char>(data[offset + 1]));
}

std::uint32_t ReadBig32(const std::string& data, std::size_t offset)
{
    return (static_cast<std::uint32_t>(
        static_cast<unsigned char>(data[offset])) << 24) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 1])) << 16) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 2])) << 8) |
        static_cast<unsigned char>(data[offset + 3]);
}

std::uint32_t ReadLittle32(const std::string& data, std::size_t offset)
{
    return static_cast<unsigned char>(data[offset]) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 1])) << 8) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 2])) << 16) |
        (static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 3])) << 24);
}

std::uint16_t ReadLittle16(const std::string& data, std::size_t offset)
{
    return static_cast<unsigned char>(data[offset]) |
        (static_cast<std::uint16_t>(
            static_cast<unsigned char>(data[offset + 1])) << 8);
}

bool PixelCountAllowed(std::uint32_t width, std::uint32_t height)
{
    return width > 0 && height > 0 &&
        static_cast<std::uint64_t>(width) * height <= 64ull * 1024ull * 1024ull;
}

bool SkipGifSubBlocks(const std::string& data, std::size_t& offset)
{
    while (offset < data.size())
    {
        const std::size_t size = static_cast<unsigned char>(data[offset++]);
        if (size == 0) return true;
        if (size > data.size() - offset) return false;
        offset += size;
    }
    return false;
}

bool StaticGifIsValid(const std::string& data)
{
    if (data.size() < 13 ||
        (data.compare(0, 6, "GIF87a") != 0 &&
            data.compare(0, 6, "GIF89a") != 0) ||
        !PixelCountAllowed(ReadLittle16(data, 6), ReadLittle16(data, 8)))
        return false;
    std::size_t offset = 13;
    const unsigned char screenFlags =
        static_cast<unsigned char>(data[10]);
    if (screenFlags & 0x80)
    {
        const std::size_t colorTableBytes =
            3u << ((screenFlags & 0x07) + 1);
        if (colorTableBytes > data.size() - offset) return false;
        offset += colorTableBytes;
    }
    unsigned int imageCount = 0;
    while (offset < data.size())
    {
        const unsigned char block =
            static_cast<unsigned char>(data[offset++]);
        if (block == 0x3b) return imageCount == 1;
        if (block == 0x21)
        {
            if (offset >= data.size()) return false;
            ++offset;
            if (!SkipGifSubBlocks(data, offset)) return false;
            continue;
        }
        if (block != 0x2c || offset + 9 > data.size()) return false;
        if (++imageCount > 1 ||
            !PixelCountAllowed(ReadLittle16(data, offset + 4),
                ReadLittle16(data, offset + 6)))
            return false;
        const unsigned char imageFlags =
            static_cast<unsigned char>(data[offset + 8]);
        offset += 9;
        if (imageFlags & 0x80)
        {
            const std::size_t colorTableBytes =
                3u << ((imageFlags & 0x07) + 1);
            if (colorTableBytes > data.size() - offset) return false;
            offset += colorTableBytes;
        }
        if (offset >= data.size()) return false;
        ++offset;
        if (!SkipGifSubBlocks(data, offset)) return false;
    }
    return false;
}

bool IconDirectoryIsValid(const std::string& data)
{
    if (data.size() < 6 || ReadLittle16(data, 0) != 0 ||
        ReadLittle16(data, 2) != 1)
        return false;
    const std::uint16_t count = ReadLittle16(data, 4);
    if (count == 0 || count > 256 ||
        static_cast<std::size_t>(count) > (data.size() - 6) / 16)
        return false;
    for (std::size_t index = 0; index < count; ++index)
    {
        const std::size_t entry = 6 + index * 16;
        const std::uint32_t width =
            static_cast<unsigned char>(data[entry]) == 0 ? 256 :
                static_cast<unsigned char>(data[entry]);
        const std::uint32_t height =
            static_cast<unsigned char>(data[entry + 1]) == 0 ? 256 :
                static_cast<unsigned char>(data[entry + 1]);
        const std::uint32_t bytes = ReadLittle32(data, entry + 8);
        const std::uint32_t imageOffset = ReadLittle32(data, entry + 12);
        if (!PixelCountAllowed(width, height) || bytes == 0 ||
            imageOffset > data.size() || bytes > data.size() - imageOffset)
            return false;
    }
    return true;
}

bool JpegDimensionsAllowed(const std::string& data)
{
    std::size_t offset = 2;
    while (offset + 4 <= data.size())
    {
        while (offset < data.size() &&
            static_cast<unsigned char>(data[offset]) != 0xff)
            ++offset;
        while (offset < data.size() &&
            static_cast<unsigned char>(data[offset]) == 0xff)
            ++offset;
        if (offset >= data.size()) break;
        const unsigned char marker =
            static_cast<unsigned char>(data[offset++]);
        if (marker == 0xd8 || marker == 0xd9 ||
            (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (offset + 2 > data.size()) break;
        const std::uint16_t length = ReadBig16(data, offset);
        if (length < 2 || offset + length > data.size()) break;
        const bool startOfFrame =
            (marker >= 0xc0 && marker <= 0xc3) ||
            (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) ||
            (marker >= 0xcd && marker <= 0xcf);
        if (startOfFrame && length >= 7)
        {
            const std::uint32_t height = ReadBig16(data, offset + 3);
            const std::uint32_t width = ReadBig16(data, offset + 5);
            return PixelCountAllowed(width, height);
        }
        offset += length;
    }
    return false;
}

bool ResourceContentIsValid(const std::filesystem::path& path,
    const PackageResource& resource)
{
    std::string data;
    if (!ReadPackageFile(path, data, kMaxResourceBytes)) return false;
    const std::string extension = Lower(path.extension().string());
    if (resource.type == "font")
    {
        if (data.size() < 4) return false;
        return (extension == ".ttf" &&
                static_cast<unsigned char>(data[0]) == 0x00 &&
                static_cast<unsigned char>(data[1]) == 0x01 &&
                static_cast<unsigned char>(data[2]) == 0x00 &&
                static_cast<unsigned char>(data[3]) == 0x00) ||
            (extension == ".otf" && data.compare(0, 4, "OTTO") == 0);
    }
    if (resource.type == "audio")
        return WidgetAudioContentIsValid(extension, data);
    if (resource.type != "image") return false;
    if (extension == ".png")
    {
        return data.size() >= 24 &&
            data.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 &&
            data.compare(12, 4, "IHDR") == 0 &&
            PixelCountAllowed(ReadBig32(data, 16), ReadBig32(data, 20));
    }
    if (extension == ".jpg" || extension == ".jpeg")
    {
        return data.size() >= 4 &&
            static_cast<unsigned char>(data[0]) == 0xff &&
            static_cast<unsigned char>(data[1]) == 0xd8 &&
            JpegDimensionsAllowed(data);
    }
    if (extension == ".gif")
        return StaticGifIsValid(data);
    if (extension == ".bmp")
    {
        if (data.size() < 26 || data.compare(0, 2, "BM") != 0) return false;
        const std::int32_t width = static_cast<std::int32_t>(
            ReadLittle32(data, 18));
        const std::int32_t height = static_cast<std::int32_t>(
            ReadLittle32(data, 22));
        if (width <= 0 || height == 0 || height ==
            (std::numeric_limits<std::int32_t>::min)())
            return false;
        return PixelCountAllowed(static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height < 0 ? -height : height));
    }
    if (extension == ".ico")
        return IconDirectoryIsValid(data);
    return false;
}

bool IsKnownPermission(const std::string& permission)
{
    return IsKnownWidgetPermission(permission);
}

bool IsExplicitDnsName(const std::string& domain)
{
    if (domain.empty() || domain.size() > 253 ||
        domain != Lower(domain) || domain.find('.') == std::string::npos)
        return false;
    bool hasLetter = false;
    std::size_t begin = 0;
    while (begin < domain.size())
    {
        const auto end = domain.find('.', begin);
        const auto label = domain.substr(begin,
            end == std::string::npos ? domain.size() - begin : end - begin);
        if (label.empty() || label.size() > 63 ||
            !std::isalnum(static_cast<unsigned char>(label.front())) ||
            !std::isalnum(static_cast<unsigned char>(label.back())))
            return false;
        for (unsigned char ch : label)
        {
            if (std::isalpha(ch)) hasLetter = true;
            if (!(std::isalnum(ch) || ch == '-')) return false;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return hasLetter;
}

bool IsFeatureId(const std::string& feature)
{
    if (feature.size() < 3 || feature.size() > 96 ||
        feature.find('.') == std::string::npos)
        return false;
    std::size_t begin = 0;
    while (begin < feature.size())
    {
        const auto end = feature.find('.', begin);
        const auto length = (end == std::string::npos
            ? feature.size() : end) - begin;
        if (length == 0 || length > 32 ||
            !std::islower(static_cast<unsigned char>(feature[begin])))
            return false;
        for (std::size_t index = begin; index < begin + length; ++index)
        {
            const unsigned char ch = feature[index];
            if (!(std::isalpha(ch) || std::isdigit(ch) || ch == '-'))
                return false;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

bool IsNativeExecutable(const std::filesystem::path& path)
{
    // Inspect PE headers as data; validation must never load a component image.
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 64> dos{};
    if (!input.read(reinterpret_cast<char*>(dos.data()), dos.size()) || dos[0] != 'M' || dos[1] != 'Z') return false;
    std::uint32_t offset = 0;
    for (unsigned index = 0; index < 4; ++index)
        offset |= static_cast<std::uint32_t>(dos[60 + index]) << (index * 8);
    if (offset < dos.size() || offset > kMaxExtractedBytes - 24) return false;
    input.seekg(offset);
    std::array<unsigned char, 24> pe{};
    if (!input.read(reinterpret_cast<char*>(pe.data()), pe.size()) ||
        pe[0] != 'P' || pe[1] != 'E' || pe[2] || pe[3]) return false;
    const unsigned flags = pe[22] | (static_cast<unsigned>(pe[23]) << 8);
    return (flags & 0x0002) != 0 && (flags & 0x2000) == 0;
}
}

bool WidgetPackageValidator::IsUuid(std::string_view value)
{
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (value[i] != '-') return false;
        }
        else if (!std::isxdigit(static_cast<unsigned char>(value[i])))
            return false;
    }
    return true;
}

bool WidgetPackageValidator::IsSemVer(std::string_view value)
{
    if (value.empty() || value.size() > 128) return false;
    const std::size_t plus = value.find('+');
    if (plus != std::string_view::npos &&
        value.find('+', plus + 1) != std::string_view::npos)
        return false;
    const std::size_t dash = value.find('-');
    if (dash != std::string_view::npos && plus != std::string_view::npos &&
        dash > plus)
        return false;
    const std::size_t coreEnd = std::min(
        plus == std::string_view::npos ? value.size() : plus,
        dash == std::string_view::npos ? value.size() : dash);

    auto numericIdentifier = [](std::string_view identifier) {
        if (identifier.empty() ||
            (identifier.size() > 1 && identifier.front() == '0'))
            return false;
        return std::all_of(identifier.begin(), identifier.end(),
            [](unsigned char ch) { return std::isdigit(ch) != 0; });
    };
    std::size_t start = 0;
    for (int component = 0; component < 3; ++component)
    {
        const std::size_t end = component == 2
            ? coreEnd : value.find('.', start);
        if (end == std::string_view::npos || end > coreEnd ||
            !numericIdentifier(value.substr(start, end - start)))
            return false;
        start = end + 1;
    }
    if (start != coreEnd + 1) return false;

    auto validSuffix = [&](std::size_t begin, std::size_t end,
        bool enforceNumericLeadingZero) {
        if (begin >= end) return false;
        while (begin < end)
        {
            const std::size_t dot = value.find('.', begin);
            const std::size_t identifierEnd =
                dot == std::string_view::npos || dot > end ? end : dot;
            const auto identifier =
                value.substr(begin, identifierEnd - begin);
            if (identifier.empty() ||
                !std::all_of(identifier.begin(), identifier.end(),
                    [](unsigned char ch) {
                        return std::isalnum(ch) != 0 || ch == '-';
                    }))
                return false;
            const bool numeric = std::all_of(identifier.begin(),
                identifier.end(), [](unsigned char ch) {
                    return std::isdigit(ch) != 0;
                });
            if (enforceNumericLeadingZero && numeric &&
                identifier.size() > 1 && identifier.front() == '0')
                return false;
            begin = identifierEnd + 1;
        }
        return true;
    };
    if (dash != std::string_view::npos &&
        !validSuffix(dash + 1,
            plus == std::string_view::npos ? value.size() : plus, true))
        return false;
    if (plus != std::string_view::npos &&
        !validSuffix(plus + 1, value.size(), false))
        return false;
    return true;
}

bool detail::IsNewerValidSemVer(std::string_view candidate, std::string_view current)
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

bool WidgetPackageValidator::IsNewerSemVer(
    std::string_view candidate, std::string_view current)
{
    if (!IsSemVer(candidate) || !IsSemVer(current))
        return false;
    return detail::IsNewerValidSemVer(candidate, current);
}

bool WidgetPackageValidator::IsSafeRelativePath(
    const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory()) return false;
    for (const auto& part : path)
    {
        if (part == L"." || part == L".." || part.empty()) return false;
    }
    return true;
}

bool WidgetPackageValidator::ReadManifest(
    const std::filesystem::path& manifestPath, PackageManifest& manifest,
    ValidationReport& report) const
{
    std::string text;
    if (!ReadPackageFile(manifestPath, text, 1024 * 1024))
    {
        report.Add(ValidationSeverity::Error, "manifest.read", manifestPath,
            "widget.json is missing, too large, or unreadable");
        return false;
    }
    JsonValue root;
    std::string parseError;
    if (!ParseJson(text, root, &parseError) || !root.IsObject())
    {
        report.Add(ValidationSeverity::Error, "manifest.json", manifestPath,
            "invalid JSON: " + parseError);
        return false;
    }

    manifest = {};
    ReadInteger(root, "schemaVersion", manifest.schemaVersion);
    ReadString(root, "id", manifest.id);
    ReadString(root, "slug", manifest.slug);
    ReadString(root, "version", manifest.version);
    ReadInteger(root, "apiVersion", manifest.apiVersion);
    ReadInteger(root, "dataVersion", manifest.dataVersion);
    if (const JsonValue* confirmation = root.Find("confirmRemoval"))
    {
        if (confirmation->IsBoolean())
            manifest.confirmRemoval = confirmation->boolean;
        else
            report.Add(ValidationSeverity::Error, "manifest.confirmRemoval",
                manifestPath, "confirmRemoval must be a boolean");
    }
    ReadString(root, "entry", manifest.entry);
    ReadString(root, "minHostVersion", manifest.minHostVersion);
    ReadString(root, "name", manifest.name);
    ReadString(root, "nameKey", manifest.nameKey);
    ReadString(root, "description", manifest.description);
    ReadString(root, "descriptionKey", manifest.descriptionKey);
    if (!ReadString(root, "author", manifest.author))
        ReadString(root, "publisher", manifest.author);
    ReadString(root, "license", manifest.license);
    ReadString(root, "preview", manifest.preview);
    detail::ReadPreviewManifestFields(root, manifestPath, manifest, report);
    if (const JsonValue* presets = root.Find("creationPresets"))
    {
        if (!presets->IsArray() || presets->array.size() > 32)
            report.Add(ValidationSeverity::Error, "manifest.creationPresets",
                manifestPath, "creationPresets must be an array of at most 32 entries");
        else
        {
            std::set<std::string> ids;
            for (const auto& item : presets->array)
            {
                CreationPreset preset;
                ReadString(item, "id", preset.id);
                ReadString(item, "title", preset.title);
                ReadString(item, "titleKey", preset.titleKey);
                if (const auto* size = item.Find("size"))
                {
                    WidgetSizeDefinition definition;
                    ReadSize(item, "size", definition.columns, definition.rows);
                    std::string sizeError;
                    const auto* c = size->Find("columns");
                    const auto* r = size->Find("rows");
                    const bool validGrid = size->Find("aspectRatio") ||
                        (c && r && c->IsNumber() && r->IsNumber() &&
                         std::isfinite(c->number) && std::isfinite(r->number) &&
                         c->number >= 1 && c->number <= 8 && r->number >= 1 && r->number <= 8 &&
                         c->number == std::floor(c->number) && r->number == std::floor(r->number));
                    if (!size->IsObject() || !ReadAspectGridSize(size, definition.columns,
                            definition.rows, definition.aspect, sizeError) ||
                        !validGrid ||
                        definition.columns < 1 || definition.columns > 8 ||
                        definition.rows < 1 || definition.rows > 8)
                        report.Add(ValidationSeverity::Error, "manifest.creationPresetSize",
                            manifestPath, sizeError.empty() ? "creation preset size must define a valid grid or aspect size" : sizeError);
                    preset.size = definition;
                }
                if (!item.IsObject() || preset.id.empty() || preset.id.size() > 64 ||
                    !std::all_of(preset.id.begin(), preset.id.end(), [](unsigned char ch) {
                        return std::isalnum(ch) || ch == '-' || ch == '_'; }) ||
                    !ids.insert(preset.id).second || preset.title.empty())
                {
                    report.Add(ValidationSeverity::Error, "manifest.creationPreset",
                        manifestPath, "creation presets require a unique ASCII id and fallback title");
                    continue;
                }
                const auto* values = item.Find("values");
                if (!values || !values->IsObject() || values->object.empty() || values->object.size() > 64)
                {
                    report.Add(ValidationSeverity::Error, "manifest.creationPresetValues",
                        manifestPath, "creation preset values must contain 1 to 64 ordinary setting strings");
                    continue;
                }
                std::size_t bytes = 0;
                for (const auto& [key, value] : values->object)
                {
                    if (key.empty() || key.size() > 128 || key.find('\0') != std::string::npos || key.starts_with("__") ||
                        !value.IsString() || value.string.size() > 16 * 1024)
                    {
                        report.Add(ValidationSeverity::Error, "manifest.creationPresetValue",
                            manifestPath, "creation presets accept only ordinary setting names and string values");
                        continue;
                    }
                    bytes += key.size() + value.string.size();
                    preset.values[key] = value.string;
                }
                if (bytes > 64 * 1024)
                    report.Add(ValidationSeverity::Error, "manifest.creationPresetQuota",
                        manifestPath, "creation preset values exceed 64 KiB");
                manifest.creationPresets.push_back(std::move(preset));
            }
        }
    }
    ReadInteger(root, "refreshIntervalMs", manifest.refreshIntervalMs);
    ReadSize(root, "defaultSize", manifest.defaultColumns, manifest.defaultRows);
    {
        std::string sizeError;
        if (!ReadAspectGridSize(root.Find("defaultSize"), manifest.defaultColumns,
                manifest.defaultRows, manifest.defaultAspectSize, sizeError))
            report.Add(ValidationSeverity::Error, "manifest.aspectSize", manifestPath, sizeError);
    }
    ReadSize(root, "minSize", manifest.minColumns, manifest.minRows);
    ReadSize(root, "maxSize", manifest.maxColumns, manifest.maxRows);
    bool arraysValid = true;
    manifest.permissions = ReadStringArray(root, "permissions", arraysValid);
    manifest.optionalPermissions =
        ReadStringArray(root, "optionalPermissions", arraysValid);
    manifest.networkDomains =
        ReadStringArray(root, "networkDomains", arraysValid);
    manifest.requiredFeatures =
        ReadStringArray(root, "requiredFeatures", arraysValid);
    const bool adaptiveSize = manifest.defaultAspectSize.has_value() ||
        std::any_of(manifest.previewVariants.begin(), manifest.previewVariants.end(),
            [](const auto& variant) { return variant.aspectSize.has_value(); }) ||
        std::any_of(manifest.creationPresets.begin(), manifest.creationPresets.end(),
            [](const auto& preset) { return preset.size.has_value(); });
    if (adaptiveSize && std::find(manifest.requiredFeatures.begin(), manifest.requiredFeatures.end(),
            "widget.size.aspectRatio") == manifest.requiredFeatures.end())
        report.Add(ValidationSeverity::Error, "manifest.aspectSizeFeature", manifestPath,
            "aspect sizes and per-preset sizes require widget.size.aspectRatio");
    if (!manifest.creationPresets.empty() &&
        std::find(manifest.requiredFeatures.begin(), manifest.requiredFeatures.end(),
            "widget.creationPresets") == manifest.requiredFeatures.end())
        report.Add(ValidationSeverity::Error, "manifest.creationPresetFeature",
            manifestPath, "creationPresets require widget.creationPresets");
    manifest.optionalFeatures =
        ReadStringArray(root, "optionalFeatures", arraysValid);
    {
        std::vector<snowdesktop::widget_runtime::
            LogicalSlotManifestError> slotErrors;
        (void)snowdesktop::widget_runtime::ParseLogicalSlotDeclarations(
            root.Find("slots"), manifest.logicalSlots, slotErrors);
        for (const auto& slotError : slotErrors)
            report.Add(ValidationSeverity::Error, slotError.code,
                manifestPath, slotError.message);
    }
    if (const JsonValue* resources = root.Find("resources"))
    {
        if (!resources->IsObject())
        {
            report.Add(ValidationSeverity::Error, "manifest.resources",
                manifestPath, "resources must be an object");
        }
        else
        {
            if (resources->object.size() > kMaxPackageResources)
            {
                report.Add(ValidationSeverity::Error,
                    "manifest.resourceCount", manifestPath,
                    "resources cannot contain more than 32 entries");
            }
            for (const auto& [name, value] : resources->object)
            {
                if (!IsResourceName(name))
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.resourceName", manifestPath,
                        "resource names must start with a lowercase letter and contain only lowercase letters, digits, '-' or '_': " +
                            name);
                    continue;
                }
                if (!value.IsObject())
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.resource", manifestPath,
                        "resource descriptors must be objects: " + name);
                    continue;
                }
                PackageResource resource;
                ReadString(value, "type", resource.type);
                ReadString(value, "path", resource.path);
                ReadString(value, "license", resource.license);
                if (resource.type != "image" && resource.type != "font" && resource.type != "audio")
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.resourceType", manifestPath,
                        "resource type must be image, font, or audio: " + name);
                }
                const std::filesystem::path resourcePath =
                    Utf8ToWide(resource.path);
                const std::string extension = Lower(
                    resourcePath.extension().string());
                const bool extensionValid = resource.type == "image"
                    ? (extension == ".png" || extension == ".jpg" ||
                        extension == ".jpeg" || extension == ".bmp" ||
                        extension == ".gif" || extension == ".ico")
                    : resource.type == "audio" ? FindWidgetAudioFormat(std::string_view(extension)) != nullptr
                    : (extension == ".ttf" || extension == ".otf");
                if (!IsSafeRelativePath(resourcePath) || !extensionValid)
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.resourcePath", manifestPath,
                        "resource path is unsafe or has an unsupported extension: " +
                            name);
                }
                if (resource.type == "font" && resource.license.empty())
                {
                    report.Add(ValidationSeverity::Error,
                        "manifest.resourceLicense", manifestPath,
                        "font resources must declare a license: " + name);
                }
                manifest.resources.emplace(name, std::move(resource));
            }
        }
    }

    if (const JsonValue* locales = root.Find("locales");
        locales && locales->IsObject())
    {
        for (const auto& [locale, values] : locales->object)
        {
            if (!IsBcp47Tag(locale))
            {
                report.Add(ValidationSeverity::Error, "manifest.locale",
                    manifestPath, "invalid BCP-47 locale tag: " + locale);
                continue;
            }
            if (!values.IsObject())
            {
                report.Add(ValidationSeverity::Error, "manifest.locale",
                    manifestPath, "locale values must be an object: " + locale);
                continue;
            }
            bool localeValuesValid = true;
            for (const auto& [key, value] : values.object)
                if (key.empty() || !value.IsString())
                    localeValuesValid = false;
            if (!localeValuesValid)
            {
                report.Add(ValidationSeverity::Error, "manifest.locale",
                    manifestPath,
                    "locale entries must contain string translations: " +
                        locale);
                continue;
            }
            LocalizedMetadata localized;
            for (const auto& [key, value] : values.object)
                localized.strings.emplace(key, value.string);
            ReadString(values, "name", localized.title);
            if (localized.title.empty()) ReadString(values, "title", localized.title);
            ReadString(values, "description", localized.description);
            if (localized.title.empty() && !manifest.nameKey.empty())
                ReadString(values, manifest.nameKey.c_str(), localized.title);
            if (localized.description.empty() &&
                !manifest.descriptionKey.empty())
                ReadString(values, manifest.descriptionKey.c_str(),
                    localized.description);
            manifest.locales.emplace(locale, std::move(localized));
        }
    }
    if (const auto* helpers = root.Find("nativeHelpers"))
    {
        if (!helpers->IsObject() || helpers->object.empty() || helpers->object.size() > 4)
            report.Add(ValidationSeverity::Error, "manifest.nativeHelpers", manifestPath,
                "nativeHelpers must contain one to four named helpers");
        else for (const auto& [id, value] : helpers->object)
        {
            NativeHelper helper;
            helper.id = id;
            bool valid = IsResourceName(id) && id.size() <= 64 && value.IsObject();
            valid = ReadString(value, "path", helper.path) && valid;
            valid = ReadString(value, "sha256", helper.sha256) && valid;
            valid = ReadString(value, "purposeKey", helper.purposeKey) && valid;
            valid = ReadString(value, "operationsKey", helper.operationsKey) && valid;
            valid = ReadString(value, "reasonKey", helper.reasonKey) && valid;
            valid = ReadString(value, "deniedKey", helper.deniedKey) && valid;
            valid = valid && IsSafeRelativePath(Utf8ToWide(helper.path)) &&
                Lower(std::filesystem::path(Utf8ToWide(helper.path)).extension().string()) == ".exe" &&
                IsSha256(helper.sha256) && helper.sha256 == Lower(helper.sha256);
            for (const auto& [key, unused] : value.object)
            {
                (void)unused;
                if (key != "path" && key != "sha256" && key != "purposeKey" &&
                    key != "operationsKey" && key != "reasonKey" && key != "deniedKey") valid = false;
            }
            for (const auto* key : { &helper.purposeKey, &helper.operationsKey,
                    &helper.reasonKey, &helper.deniedKey })
            {
                if (key->empty() || key->size() > 256) valid = false;
                for (const auto& [locale, metadata] : manifest.locales)
                {
                    (void)locale;
                    const auto translation = metadata.strings.find(*key);
                    if (translation == metadata.strings.end() || translation->second.empty() ||
                        translation->second.size() > 4096 || translation->second.find('\0') != std::string::npos) valid = false;
                }
            }
            if (!manifest.locales.contains("en-US") && !manifest.locales.contains("en")) valid = false;
            if (!valid)
                report.Add(ValidationSeverity::Error, "manifest.nativeHelper", manifestPath,
                    "helper requires a safe .exe path, lowercase SHA-256, and four translated explanation keys: " + id);
            manifest.nativeHelpers.push_back(std::move(helper));
        }
        std::sort(manifest.nativeHelpers.begin(), manifest.nativeHelpers.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
        manifest = LocalizePackageManifest(std::move(manifest), "en-US");
        const auto declared = DeclaredPermissions(manifest);
        if (std::find(declared.begin(), declared.end(), "native.execute") == declared.end() ||
            (std::find(manifest.requiredFeatures.begin(), manifest.requiredFeatures.end(),
                "task.native.request") == manifest.requiredFeatures.end() &&
             std::find(manifest.requiredFeatures.begin(), manifest.requiredFeatures.end(),
                "task.native.watch") == manifest.requiredFeatures.end()))
            report.Add(ValidationSeverity::Error, "manifest.nativeContract", manifestPath,
                "nativeHelpers requires native.execute and required feature task.native.request or task.native.watch");
    }
    else
    {
        const auto declared = DeclaredPermissions(manifest);
        if (std::find(declared.begin(), declared.end(), "native.execute") != declared.end())
            report.Add(ValidationSeverity::Error, "manifest.nativeContract", manifestPath,
                "native.execute requires nativeHelpers");
    }
    const bool currentContract =
        manifest.schemaVersion == kPackageSchemaVersion &&
        manifest.apiVersion == kHostApiVersion;
    if (manifest.schemaVersion != kPackageSchemaVersion)
        report.Add(ValidationSeverity::Error, "manifest.schemaVersion",
            manifestPath, "schemaVersion must be 2");
    if (!IsUuid(manifest.id))
        report.Add(ValidationSeverity::Error, "manifest.id", manifestPath,
            "id must be an immutable UUID");
    if (manifest.slug.empty() ||
        std::any_of(manifest.slug.begin(), manifest.slug.end(),
            [](unsigned char ch) {
                return !(std::islower(ch) || std::isdigit(ch) || ch == '-');
            }))
        report.Add(ValidationSeverity::Error, "manifest.slug", manifestPath,
            "slug must contain lowercase letters, digits, and hyphens only");
    if (!IsSemVer(manifest.version))
        report.Add(ValidationSeverity::Error, "manifest.version", manifestPath,
            "version must be SemVer");
    if (manifest.apiVersion != kHostApiVersion)
        report.Add(ValidationSeverity::Error, "manifest.apiVersion",
            manifestPath, "apiVersion is not supported by this host");
    if (!currentContract &&
        (manifest.schemaVersion == kPackageSchemaVersion ||
            manifest.apiVersion == kHostApiVersion))
    {
        report.Add(ValidationSeverity::Error, "manifest.contractVersion",
            manifestPath,
            "schemaVersion and apiVersion must both be 2");
    }
    if (manifest.dataVersion < 1)
        report.Add(ValidationSeverity::Error, "manifest.dataVersion",
            manifestPath, "dataVersion must be a positive integer");
    if (!IsSafeRelativePath(Utf8ToWide(manifest.entry)) ||
        Lower(std::filesystem::path(Utf8ToWide(manifest.entry))
            .extension().string()) != ".lua")
        report.Add(ValidationSeverity::Error, "manifest.entry", manifestPath,
            "entry must be a package-relative .lua path");
    if (manifest.name.empty())
        report.Add(ValidationSeverity::Error, "manifest.name", manifestPath,
            "an English fallback name is required");
    if (manifest.description.empty())
        report.Add(ValidationSeverity::Warning, "manifest.description",
            manifestPath, "an English fallback description is recommended");
    if (manifest.license.empty())
        report.Add(ValidationSeverity::Warning, "manifest.license",
            manifestPath, "a license identifier is recommended");
    for (auto& variant : manifest.previewVariants)
    {
        detail::ResolvePreviewVariantDefaults(variant,
            {manifest.defaultColumns, manifest.defaultRows, manifest.defaultAspectSize},
            manifest.minColumns, manifest.minRows, manifest.maxColumns, manifest.maxRows);
        if (variant.columns < manifest.minColumns ||
            variant.rows < manifest.minRows ||
            (manifest.maxColumns > 0 &&
                variant.columns > manifest.maxColumns) ||
            (manifest.maxRows > 0 && variant.rows > manifest.maxRows))
        {
            report.Add(ValidationSeverity::Error,
                "manifest.previewVariantSize", manifestPath,
                "preview variant sizes must stay within the component's minSize and maxSize");
        }
    }
    if (!arraysValid)
        report.Add(ValidationSeverity::Error, "manifest.array", manifestPath,
            "permissions, domains, and feature declarations must be string arrays");
    for (const auto& preset : manifest.creationPresets)
        if (preset.size && !preset.size->aspect &&
            (preset.size->columns < manifest.minColumns || preset.size->rows < manifest.minRows ||
             (manifest.maxColumns > 0 && preset.size->columns > manifest.maxColumns) ||
             (manifest.maxRows > 0 && preset.size->rows > manifest.maxRows)))
            report.Add(ValidationSeverity::Error, "manifest.creationPresetSize", manifestPath,
                "creation preset grid sizes must stay within minSize and maxSize");
    std::set<std::string> uniquePermissions;
    for (const auto& permission : manifest.permissions)
    {
        if (!IsKnownPermission(permission))
            report.Add(ValidationSeverity::Error, "manifest.permission",
                manifestPath, "unknown permission: " + permission);
        if (!uniquePermissions.insert(permission).second)
            report.Add(ValidationSeverity::Error, "manifest.permissionDuplicate",
                manifestPath, "duplicate permission: " + permission);
    }
    for (const auto& permission : manifest.optionalPermissions)
    {
        if (!IsKnownPermission(permission))
            report.Add(ValidationSeverity::Error,
                "manifest.optionalPermission", manifestPath,
                "unknown optional permission: " + permission);
        if (!uniquePermissions.insert(permission).second)
            report.Add(ValidationSeverity::Error,
                "manifest.permissionDuplicate", manifestPath,
                "required and optional permissions must be unique: " +
                    permission);
    }
    if (!manifest.networkDomains.empty() &&
        !uniquePermissions.contains("network.http") &&
        !uniquePermissions.contains("network.internet"))
        report.Add(ValidationSeverity::Error, "manifest.networkDomains",
            manifestPath,
            "networkDomains requires network.http or network.internet permission");
    std::set<std::string> uniqueDomains;
    for (const auto& domain : manifest.networkDomains)
    {
        if (!IsExplicitDnsName(domain))
            report.Add(ValidationSeverity::Error, "manifest.networkDomain",
                manifestPath, "network domains must be explicit DNS names");
        if (!uniqueDomains.insert(domain).second)
            report.Add(ValidationSeverity::Error,
                "manifest.networkDomainDuplicate", manifestPath,
                "duplicate network domain: " + domain);
    }
    std::set<std::string> uniqueFeatures;
    if (manifest.confirmRemoval &&
        std::find(manifest.requiredFeatures.begin(),
            manifest.requiredFeatures.end(), "widget.confirmRemoval") ==
            manifest.requiredFeatures.end())
        report.Add(ValidationSeverity::Error, "manifest.confirmRemovalFeature",
            manifestPath,
            "confirmRemoval requires widget.confirmRemoval in requiredFeatures");
    for (const auto& feature : manifest.requiredFeatures)
    {
        if (!IsFeatureId(feature))
            report.Add(ValidationSeverity::Error, "manifest.feature",
                manifestPath, "invalid required feature id: " + feature);
        if (!uniqueFeatures.insert(feature).second)
            report.Add(ValidationSeverity::Error,
                "manifest.featureDuplicate", manifestPath,
                "duplicate feature: " + feature);
    }
    for (const auto& feature : manifest.optionalFeatures)
    {
        if (!IsFeatureId(feature))
            report.Add(ValidationSeverity::Error,
                "manifest.optionalFeature", manifestPath,
                "invalid optional feature id: " + feature);
        if (!uniqueFeatures.insert(feature).second)
            report.Add(ValidationSeverity::Error,
                "manifest.featureDuplicate", manifestPath,
                "required and optional features must be unique: " + feature);
    }
    return report.Ok();
}

ValidationReport WidgetPackageValidator::ValidateDirectory(
    const std::filesystem::path& root, PackageManifest* outputManifest) const
{
    ValidationReport report;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
    {
        report.Add(ValidationSeverity::Error, "package.directory", root,
            "package root is not a directory");
        return report;
    }
    if (HasReparsePoint(root))
    {
        report.Add(ValidationSeverity::Error, "package.reparse", root,
            "package root cannot be a reparse point");
        return report;
    }
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec)
    {
        report.Add(ValidationSeverity::Error, "package.canonical", root,
            "cannot canonicalize package root");
        return report;
    }
    PackageManifest manifest;
    ReadManifest(root / L"widget.json", manifest, report);
    struct NativeValidationLock
    {
        std::vector<HANDLE> handles;
        ~NativeValidationLock() { for (const auto handle : handles) CloseHandle(handle); }
        bool Pin(const std::filesystem::path& path, bool directory)
        {
            const auto handle = CreateFileW(path.c_str(), directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
                directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT |
                    (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL), nullptr);
            if (handle == INVALID_HANDLE_VALUE) return false;
            handles.push_back(handle);
            BY_HANDLE_FILE_INFORMATION info{};
            return GetFileInformationByHandle(handle, &info) &&
                !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                static_cast<bool>(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
        }
    } nativeLock;
    if (!manifest.nativeHelpers.empty())
    {
        const auto absoluteRoot = std::filesystem::absolute(root, ec);
        bool locked = !ec && nativeLock.Pin(absoluteRoot.root_path(), true);
        auto directory = absoluteRoot.root_path();
        for (const auto& part : absoluteRoot.relative_path())
        {
            directory /= part;
            if (locked) locked = nativeLock.Pin(directory, true);
        }
        if (locked) locked = nativeLock.Pin(absoluteRoot / L"widget.json", false);
        if (!locked)
            report.Add(ValidationSeverity::Error, "package.nativeIdentity", root,
                "cannot lock native component metadata and directory identity");
        else
        {
            // Metadata and its content hash must describe the same immutable
            // bytes, even while an author is updating a development package.
            report = {};
            manifest = {};
            ReadManifest(absoluteRoot / L"widget.json", manifest, report);
        }
        ec.clear();
    }
    std::vector<std::string> nativeContent;

    for (std::filesystem::recursive_directory_iterator it(root,
        std::filesystem::directory_options::none, ec), end;
        !ec && it != end; it.increment(ec))
    {
        const auto& entry = *it;
        const auto path = entry.path();
        const auto relative = std::filesystem::relative(path, root, ec);
        if (ec || !IsSafeRelativePath(relative))
        {
            report.Add(ValidationSeverity::Error, "package.path", path,
                "package contains an unsafe path");
            ec.clear();
            continue;
        }
        if (entry.is_symlink(ec) || HasReparsePoint(path))
        {
            report.Add(ValidationSeverity::Error, "package.reparse", relative,
                "symbolic links, junctions, and reparse points are forbidden");
            if (entry.is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        const auto canonical = std::filesystem::weakly_canonical(path, ec);
        if (ec || !StartsWithPath(canonical, canonicalRoot))
        {
            report.Add(ValidationSeverity::Error, "package.escape", relative,
                "package entry escapes its root");
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        ++report.fileCount;
        const auto size = entry.file_size(ec);
        if (ec)
        {
            report.Add(ValidationSeverity::Error, "package.fileSize", relative,
                "cannot read file size");
            ec.clear();
            continue;
        }
        report.totalBytes += size;
        if (report.fileCount > kMaxPackageFiles)
            report.Add(ValidationSeverity::Error, "package.fileCount", relative,
                "package contains more than 512 files");
        if (report.totalBytes > kMaxExtractedBytes)
            report.Add(ValidationSeverity::Error, "package.totalBytes", relative,
                "package exceeds the 64 MiB extracted limit");
        if (relative == Utf8ToWide(manifest.entry) &&
            size > kMaxEntryLuaBytes)
            report.Add(ValidationSeverity::Error, "package.entrySize", relative,
                "Lua entry exceeds 1 MiB");
        if (!manifest.preview.empty() &&
            relative == Utf8ToWide(manifest.preview) &&
            size > kMaxPreviewBytes)
            report.Add(ValidationSeverity::Error, "package.previewSize", relative,
                "preview exceeds 2 MiB");
        const std::string extension = Lower(path.extension().string());
        const auto native = std::find_if(manifest.nativeHelpers.begin(), manifest.nativeHelpers.end(),
            [&](const auto& helper) { return relative == Utf8ToWide(helper.path); });
        if (extension == ".exe" && native != manifest.nativeHelpers.end())
        {
            const auto hash = WidgetPackageManager::Sha256File(path);
            if (hash.empty() || hash != native->sha256 || !IsNativeExecutable(path))
                report.Add(ValidationSeverity::Error, "package.nativeHash", relative,
                    "native helper must be a PE executable matching its declared SHA-256");
        }
        else if (extension == ".dll" || extension == ".exe" || extension == ".com" ||
            extension == ".bat" || extension == ".cmd" || extension == ".ps1" ||
            extension == ".msi" || extension == ".scr")
            report.Add(ValidationSeverity::Error, "package.executable", relative,
                "native libraries and executable files are forbidden");
        if (!manifest.nativeHelpers.empty() && report.Ok())
        {
            const auto hash = WidgetPackageManager::Sha256File(path);
            if (hash.empty())
                report.Add(ValidationSeverity::Error, "package.nativeContent", relative,
                    "cannot fingerprint native component content");
            else nativeContent.push_back(PathUtf8(relative) + ":" + hash);
        }
    }
    if (ec)
        report.Add(ValidationSeverity::Error, "package.enumerate", root,
            "cannot enumerate package: " + ec.message());
    if (!std::filesystem::is_regular_file(
        root / Utf8ToWide(manifest.entry), ec))
        report.Add(ValidationSeverity::Error, "package.entryMissing",
            manifest.entry, "Lua entry file is missing");
    if (!manifest.preview.empty() &&
        (!IsSafeRelativePath(Utf8ToWide(manifest.preview)) ||
         !std::filesystem::is_regular_file(
            root / Utf8ToWide(manifest.preview), ec)))
        report.Add(ValidationSeverity::Error, "package.previewMissing",
            manifest.preview, "preview path is unsafe or missing");
    if (manifest.previewGallery)
    {
        std::set<std::wstring> unique;
        for (const auto& image : *manifest.previewGallery)
        {
            const std::filesystem::path relative = Utf8ToWide(image);
            auto normalized = relative.lexically_normal().generic_wstring();
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            auto extension = relative.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            ec.clear();
            const auto path = root / relative;
            const bool regular = std::filesystem::is_regular_file(path, ec);
            const auto size = regular && !ec ? std::filesystem::file_size(path, ec) : 0;
            if (!IsSafeRelativePath(relative) || !unique.insert(normalized).second ||
                (extension != L".png" && extension != L".jpg" &&
                 extension != L".jpeg" && extension != L".gif") ||
                !regular || ec || size == 0 || size >= 1024 * 1024)
                report.Add(ValidationSeverity::Error, "package.previewGallery", relative,
                    "gallery images must be unique safe relative PNG/JPG/GIF files, nonempty and smaller than 1 MiB");
        }
    }
    std::size_t fontCount = 0;
    for (const auto& [name, resource] : manifest.resources)
    {
        if (resource.type == "font") ++fontCount;
        const std::filesystem::path relative = Utf8ToWide(resource.path);
        const std::filesystem::path fullPath = root / relative;
        ec.clear();
        if (!IsSafeRelativePath(relative) ||
            !std::filesystem::is_regular_file(fullPath, ec))
        {
            report.Add(ValidationSeverity::Error,
                "package.resourceMissing", relative,
                "declared resource is missing: " + name);
            continue;
        }
        const auto size = std::filesystem::file_size(fullPath, ec);
        if (ec || size > kMaxResourceBytes)
        {
            report.Add(ValidationSeverity::Error,
                "package.resourceSize", relative,
                "resource is unreadable or exceeds the 8 MiB limit: " +
                    name);
            continue;
        }
        if (!ResourceContentIsValid(fullPath, resource))
        {
            report.Add(ValidationSeverity::Error,
                "package.resourceContent", relative,
                "resource signature, dimensions, or declared type is invalid: " +
                    name);
        }
    }
    if (fontCount > 8)
    {
        report.Add(ValidationSeverity::Error, "package.resourceFontCount",
            root / L"widget.json",
            "a package cannot declare more than 8 fonts");
    }
    for (const auto& helper : manifest.nativeHelpers)
        if (!std::filesystem::is_regular_file(root / Utf8ToWide(helper.path), ec))
            report.Add(ValidationSeverity::Error, "package.nativeMissing", helper.path,
                "declared native helper is missing");
    if (!manifest.nativeHelpers.empty() && report.Ok())
        manifest.nativeContentFingerprint = WidgetPermissionBroker::ScopeFingerprint(nativeContent, {});
    if (outputManifest) *outputManifest = std::move(manifest);
    return report;
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
            << detail::JsonEscape(issues[i].code) << "\",\"path\":\""
            << detail::JsonEscape(PathUtf8(issues[i].path)) << "\",\"message\":\""
            << detail::JsonEscape(issues[i].message) << "\"}";
    }
    out << "]}";
    return out.str();
}

ValidationReport WidgetPackageManager::ValidateDirectory(
    const std::filesystem::path& root, PackageManifest* manifest) const
{
    return validator_.ValidateDirectory(root, manifest);
}
}
