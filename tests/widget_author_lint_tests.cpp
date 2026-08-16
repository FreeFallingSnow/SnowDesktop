#include "widget_author_lint.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using snowdesktop::widget::PackageManifest;
using snowdesktop::widget_authoring::LintWidgetSource;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

bool HasIssue(const snowdesktop::widget_authoring::LintReport& report,
    std::string_view code)
{
    for (const auto& issue : report.issues)
        if (issue.code == code) return true;
    return false;
}

void TestCleanLocalizedSource()
{
    PackageManifest manifest;
    manifest.apiVersion = 2;
    manifest.permissions = { "system.performance.read" };
    const auto report = LintWidgetSource(manifest, "main.lua", R"lua(
local cpu = data.subscribe("system.cpu", { intervalMs = 1000 })
return widget.define({
    view = function()
        return view.column({
            key = "root",
            children = {
                view.text({ key = "title", text = l10n.t("title") }),
            },
        })
    end,
})
)lua");
    Check(report.Ok() && report.issues.empty() && report.fileCount == 1,
        "localized source with declared capability permission must lint cleanly");
}

void TestApiPermissionAndSandboxFailures()
{
    PackageManifest manifest;
    manifest.apiVersion = 2;
    const auto report = LintWidgetSource(manifest, "main.lua", R"lua(
-- os.execute("ignored comment")
local cpu = data.subscribe("system.cpu", {})
local future = data.subscribe("system.future", {})
task.start("network.request", { url = "https://example.test" })
view.future({ key = "future" })
require("legacy")
io.open("secret")
)lua");
    Check(!report.Ok() &&
            HasIssue(report, "permission.undeclared") &&
            HasIssue(report, "api.unknown-capability") &&
            HasIssue(report, "api.unknown") &&
            HasIssue(report, "api.forbidden-global") &&
            HasIssue(report, "api.forbidden-library"),
        "lint must reject undeclared permissions, unknown APIs/capabilities, and forbidden sandbox calls");
}

void TestViewKeysAndLiteralText()
{
    PackageManifest manifest;
    manifest.apiVersion = 2;
    const auto report = LintWidgetSource(manifest, "main.lua", R"lua(
return view.column({
    key = "root",
    children = {
        view.text({ text = "Hard coded" }),
        view.button({ key = "same", label = "Open" }),
        view.button({ key = "same", label = l10n.t("close") }),
        view.text({ key = "" , text = l10n.t("empty") }),
    },
})
)lua");
    Check(!report.Ok() &&
            HasIssue(report, "view.key.missing") &&
            HasIssue(report, "view.key.empty") &&
            HasIssue(report, "view.key.duplicate-literal") &&
            HasIssue(report, "l10n.hardcoded") &&
            report.WarningCount() >= 2,
        "lint must report missing, empty, duplicate literal keys and hard-coded UI text");
    const std::string json = report.ToJson();
    Check(json.find("\"errorCount\"") != std::string::npos &&
            json.find("\"line\"") != std::string::npos &&
            json.find("main.lua") != std::string::npos,
        "lint JSON must expose counts, source paths, and line numbers");
}
}

int main()
{
    TestCleanLocalizedSource();
    TestApiPermissionAndSandboxFailures();
    TestViewKeysAndLiteralText();
    std::cout << "widget author lint tests passed\n";
    return 0;
}
