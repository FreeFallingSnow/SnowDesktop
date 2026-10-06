#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace snowdesktop::widget_authoring
{
struct TestCaseResult
{
    std::filesystem::path path;
    std::string name;
    bool passed = false;
    std::string error;
};

struct TestRunIssue
{
    std::string code;
    std::filesystem::path path;
    std::string message;
};

struct TestRunReport
{
    std::vector<TestCaseResult> cases;
    std::vector<TestRunIssue> issues;
    std::size_t fileCount = 0;

    bool Ok() const noexcept;
    std::size_t PassedCount() const noexcept;
    std::size_t FailedCount() const noexcept;
    std::string ToJson() const;
};

TestRunReport RunWidgetTests(const std::filesystem::path& packageRoot);
}
