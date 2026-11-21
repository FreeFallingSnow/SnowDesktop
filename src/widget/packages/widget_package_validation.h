#pragma once

#include "widget_package_validation_report.h"

#include <filesystem>
#include <string_view>

namespace snowdesktop::widget
{
struct PackageManifest;

class WidgetPackageValidator
{
public:
    ValidationReport ValidateDirectory(const std::filesystem::path& root,
        PackageManifest* manifest = nullptr) const;
    ValidationReport ValidateArchive(
        const std::filesystem::path& archive) const;
    bool ReadManifest(const std::filesystem::path& manifestPath,
        PackageManifest& manifest, ValidationReport& report) const;

    static bool IsUuid(std::string_view value);
    static bool IsSemVer(std::string_view value);
    static bool IsNewerSemVer(
        std::string_view candidate, std::string_view current);
    static bool IsSafeRelativePath(const std::filesystem::path& path);
};
}
