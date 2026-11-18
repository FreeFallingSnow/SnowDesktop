#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct JsonValue;

namespace snowdesktop::widget
{
struct PackageManifest;
struct ValidationReport;

namespace detail
{
bool ReadString(const JsonValue& object, const char* name, std::string& output);
bool ReadInteger(const JsonValue& object, const char* name, int& output);
std::vector<std::string> ReadStringArray(const JsonValue& object,
    const char* name, bool& valid);
void ReadSize(const JsonValue& root, const char* name, int& columns, int& rows);

void ReadPreviewManifestFields(const JsonValue& root,
    const std::filesystem::path& manifestPath, PackageManifest& manifest,
    ValidationReport& report);
}
}
