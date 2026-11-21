#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace snowdesktop::widget
{
enum class ValidationSeverity
{
    Info,
    Warning,
    Error,
};

struct ValidationIssue
{
    ValidationSeverity severity = ValidationSeverity::Error;
    std::string code;
    std::filesystem::path path;
    std::string message;
};

struct ValidationReport
{
    std::vector<ValidationIssue> issues;
    std::uint64_t totalBytes = 0;
    std::size_t fileCount = 0;

    bool Ok() const;
    void Add(ValidationSeverity severity, std::string code,
        std::filesystem::path path, std::string message);
    std::string ToJson() const;
};
}
