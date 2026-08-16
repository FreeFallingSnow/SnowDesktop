#include "widget_author_test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        path = std::filesystem::temp_directory_path() /
            ("snowdesktop-author-test-" + std::to_string(
                static_cast<unsigned long long>(std::rand())));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

void Write(const std::filesystem::path& path, std::string_view value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
}

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

bool HasIssue(const snowdesktop::widget_authoring::TestRunReport& report,
    std::string_view code)
{
    for (const auto& issue : report.issues)
        if (issue.code == code) return true;
    return false;
}

void TestIsolatedCasesAndModules()
{
    TemporaryDirectory temporary;
    Write(temporary.path / L"modules" / L"math.lua", R"lua(
return { add = function(left, right) return left + right end }
)lua");
    Write(temporary.path / L"tests" / L"basic.lua", R"lua(
local helper = module.require("modules/math.lua")
return {
    passes = function()
        assert(helper.add(2, 3) == 5)
        assert(os == nil and io == nil and debug == nil and require == nil)
    end,
    explicitFalse = function() return false end,
    throws = function() error("expected failure") end,
}
)lua");
    const auto report = snowdesktop::widget_authoring::
        RunWidgetTests(temporary.path);
    Check(!report.Ok() && report.fileCount == 1 &&
            report.cases.size() == 3 && report.PassedCount() == 1 &&
            report.FailedCount() == 2 && report.issues.empty(),
        "test runner must isolate pure cases, load package modules, and report failures");
    Check(report.ToJson().find("expected failure") != std::string::npos,
        "test JSON must include bounded case failures");
}

void TestContractAndMissingDirectory()
{
    TemporaryDirectory missing;
    const auto missingReport = snowdesktop::widget_authoring::
        RunWidgetTests(missing.path);
    Check(!missingReport.Ok() && HasIssue(missingReport, "test.missing"),
        "packages without tests must receive a stable missing-test issue");

    TemporaryDirectory invalid;
    Write(invalid.path / L"tests" / L"invalid.lua", "return true\n");
    const auto invalidReport = snowdesktop::widget_authoring::
        RunWidgetTests(invalid.path);
    Check(!invalidReport.Ok() && HasIssue(invalidReport, "test.contract"),
        "test files must return a named function table");
}
}

int main()
{
    TestIsolatedCasesAndModules();
    TestContractAndMissingDirectory();
    std::cout << "widget author test runner tests passed\n";
    return 0;
}
