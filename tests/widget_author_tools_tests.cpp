#include "widget_author_migrate.h"
#include "widget_author_permissions.h"
#include "widget_package.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        wchar_t root[MAX_PATH]{};
        Check(GetTempPathW(MAX_PATH, root) != 0,
            "temporary root is available");
        path = std::filesystem::path(root) /
            (L"SnowDesktopAuthorTools-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()));
        std::error_code error;
        Check(std::filesystem::create_directory(path, error),
            "temporary author-tools directory is created");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

void Write(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    Check(static_cast<bool>(output), "test fixture is written");
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void TestPermissionReport()
{
    snowdesktop::widget::PackageManifest manifest;
    manifest.id = "3fbb18cd-7c46-4a9f-9fe3-3e2c19facb23";
    manifest.permissions = { "network.internet" };
    manifest.optionalPermissions = { "shell.launch" };
    manifest.networkDomains = { "feeds.example.com" };
    const auto report = snowdesktop::widget_authoring::
        BuildPermissionReport(manifest);
    Check(report.ok &&
            report.json.find("\"schemaVersion\":1") != std::string::npos &&
            report.json.find("\"risk\":\"externalCommunication\"") !=
                std::string::npos &&
            report.json.find("\"risk\":\"modification\"") !=
                std::string::npos &&
            report.json.find("\"id\":\"network.request\"") !=
                std::string::npos &&
            report.json.find("feeds.example.com") != std::string::npos &&
            report.json.find("\"requiresConsent\":true") !=
                std::string::npos,
        "permission report uses shared risk, task, and origin contracts");
}

void TestMigrationDraft()
{
    TemporaryDirectory temporary;
    const auto source = temporary.path / L"legacy-widget";
    const auto output = temporary.path / L"legacy-widget-v2-draft";
    std::error_code error;
    Check(std::filesystem::create_directory(source, error),
        "legacy fixture directory is created");
    const std::string original =
        "function render() drawText('legacy') end\n";
    Write(source / L"main.lua", original);
    Write(source / L"widget.json", R"json({
  "schemaVersion": 1,
  "apiVersion": 1,
  "dataVersion": 1,
  "id": "64107f41-197a-426a-8f86-6eeb020f56b0",
  "slug": "legacy-widget",
  "version": "1.2.3",
  "entry": "main.lua",
  "name": "Legacy widget",
  "description": "Migration fixture",
  "author": "SnowDesktop",
  "license": "MIT",
  "defaultSize": {"columns": 2, "rows": 1},
  "permissions": [],
  "optionalPermissions": [],
  "networkDomains": []
})json");

    const auto migrated = snowdesktop::widget_authoring::
        CreateV2MigrationDraft(source, output);
    Check(migrated.ok && migrated.stage == "complete" &&
            migrated.originalEntry == "main.lua" &&
            migrated.draftEntry == "main-v2.lua" &&
            Read(source / L"main.lua") == original &&
            Read(output / L"main.lua") == original &&
            std::filesystem::is_regular_file(output / L"main-v2.lua") &&
            std::filesystem::is_regular_file(output / L"MIGRATION-V2.md"),
        "migrate-v2 creates a separate draft and preserves the source entry");
    snowdesktop::widget::WidgetPackageValidator validator;
    snowdesktop::widget::PackageManifest manifest;
    const auto validation = validator.ValidateDirectory(output, &manifest);
    Check(validation.Ok() && manifest.schemaVersion == 2 &&
            manifest.apiVersion == 2 && manifest.entry == "main-v2.lua",
        "generated migration draft is a valid API v2 package scaffold");

    const auto repeated = snowdesktop::widget_authoring::
        CreateV2MigrationDraft(source, output);
    Check(!repeated.ok && repeated.stage == "output.exists" &&
            Read(source / L"main.lua") == original &&
            Read(output / L"main.lua") == original,
        "migrate-v2 never overwrites the source or an existing draft");
}
}

int main()
{
    TestPermissionReport();
    TestMigrationDraft();
    std::cout << "widget author tools tests passed\n";
    return 0;
}
