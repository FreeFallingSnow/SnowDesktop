#include "widget_author_permissions.h"
#include "widget_package.h"

#include <cstdlib>
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

}

int main()
{
    TestPermissionReport();
    std::cout << "widget author tools tests passed\n";
    return 0;
}
