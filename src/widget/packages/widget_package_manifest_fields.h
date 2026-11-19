#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct JsonValue;

namespace snowdesktop::widget
{
struct PackageManifest;
struct PreviewVariant;
struct ValidationReport;

namespace detail
{
std::string JsonEscape(std::string_view value);
bool DecimalIdentifier(std::string_view value);
std::string Lower(std::string value);
bool IsBcp47Tag(const std::string& locale);
bool IsSha256(std::string_view value);

// Inputs have already passed package version validation.
bool IsNewerValidSemVer(std::string_view candidate, std::string_view current);

bool ReadString(const JsonValue& object, const char* name, std::string& output);
bool ReadInteger(const JsonValue& object, const char* name, int& output);
std::vector<std::string> ReadStringArray(const JsonValue& object,
    const char* name, bool& valid);
void ReadSize(const JsonValue& root, const char* name, int& columns, int& rows);

struct PreviewManifestFields
{
    std::string& introduction;
    std::string& introductionKey;
    std::unordered_map<std::string, std::string>& storage;
    std::unordered_map<std::string, std::string>& storageKeys;
    std::vector<PreviewVariant>& variants;
};

// The runtime reads legacy manifests tolerantly. The package entry additionally
// validates the same input and reports errors in source order.
void ReadRuntimePreviewFields(const JsonValue& root, PreviewManifestFields output);

void ReadPreviewManifestFields(const JsonValue& root,
    const std::filesystem::path& manifestPath, PackageManifest& manifest,
    ValidationReport& report);
}
}
