#pragma once

#include "widget_package.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_authoring
{
enum class LintSeverity
{
    Warning,
    Error,
};

struct LintIssue
{
    LintSeverity severity = LintSeverity::Error;
    std::string code;
    std::filesystem::path path;
    std::size_t line = 1;
    std::string message;
};

struct LintReport
{
    std::vector<LintIssue> issues;
    std::size_t fileCount = 0;

    bool Ok() const noexcept;
    std::size_t ErrorCount() const noexcept;
    std::size_t WarningCount() const noexcept;
    std::string ToJson() const;
};

LintReport LintWidgetSource(
    const snowdesktop::widget::PackageManifest& manifest,
    const std::filesystem::path& relativePath,
    std::string_view source);

LintReport LintWidgetDirectory(
    const std::filesystem::path& root,
    const snowdesktop::widget::PackageManifest& manifest);
}
