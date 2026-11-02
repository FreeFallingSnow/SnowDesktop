#include "widget/authoring/widget_author_permissions.h"
#include "widget/packages/widget_package.h"
#include "widget/packages/widget_catalog_refresh.h"
#include "widget/packages/widget_package_file_export.h"
#include "widget/api/widget_api_registry.h"
#include "app/widgets/widget_grid_size.h"
extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}
#include "common/json_value.h"
#include "test_temporary_directory.h"
#include <fstream>
#include <limits>
#include <span>
#include <vector>

#include <cstdlib>
#include <iostream>
#include <string>

void TestReviewedPackageInstallation();

namespace
{
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

void TestCatalogueRefreshCache()
{
    using namespace snowdesktop::widget;
    std::vector<std::pair<bool, const char*>> checks;
    {
        snowdesktop::test::TemporaryDirectory temporary;
        PackagePaths paths;
        paths.builtin = temporary.path / L"builtin";
        paths.installed = temporary.path / L"installed";
        paths.development = temporary.path / L"dev";
        paths.staging = temporary.path / L"staging";
        paths.quarantine = temporary.path / L"quarantine";
        paths.migrations = temporary.path / L"migrations";
        paths.registry = temporary.path / L"packages.json";
        std::filesystem::create_directory(paths.builtin);
        const auto package = paths.builtin / L"component";
        std::filesystem::create_directory(package);
        auto writeManifest = [&](const char* name) {
            std::ofstream file(package / L"widget.json", std::ios::binary | std::ios::trunc);
            file << R"({"schemaVersion":2,"apiVersion":2,"dataVersion":1,
                "id":"3fbb18cd-7c46-4a9f-9fe3-3e2c19facb23","slug":"cache-test",
                "version":"1.0.0","entry":"main.lua","permissions":[],"name":")"
                << name << R"("})";
        };
        writeManifest("Original");
        { std::ofstream file(package / L"main.lua"); file << "return widget.define({})"; }
        WidgetPackageManager manager(paths);
        std::string error;
        checks.push_back({manager.Initialize(error) && manager.ListPackages().size() == 1,
            "startup validates and retains the initial component catalogue"});
        detail::CatalogRefresh cache;
        const auto revision = std::make_shared<const char>('\0');
        const std::array roots{paths.builtin, paths.installed, paths.development};
        cache.Begin(roots);
        cache.Complete(revision);
        checks.push_back({cache.CanReuse(revision),
            "healthy local watches make the first menu reuse its startup validation"});
        checks.push_back({!cache.CanReuse(std::make_shared<const char>('\0')),
            "a copied manager with a different snapshot cannot reuse another revision"});
        auto copied = manager;
        writeManifest("Changed");
        checks.push_back({!cache.CanReuse(revision),
            "nested manifest changes invalidate the catalogue snapshot"});
        checks.push_back({manager.RefreshCatalog(error) && manager.ListPackages().size() == 1 &&
            manager.ListPackages().front().manifest.name == "Changed",
            "refresh reads changed component metadata rather than a cached manifest"});
        checks.push_back({copied.RefreshCatalog(error) && copied.ListPackages().size() == 1 &&
            copied.ListPackages().front().manifest.name == "Changed",
            "a manager copy refreshes its own stale package vector"});
        cache.Begin(roots);
        cache.Complete(revision);
        { std::ofstream file(package / L"native.dll"); file << "forbidden"; }
        checks.push_back({!cache.CanReuse(revision) &&
            !WidgetPackageValidator{}.ValidateDirectory(package).Ok(),
            "new unsafe files invalidate the menu cache and runtime validation remains independent"});
        checks.push_back({manager.RefreshCatalog(error) && manager.ListPackages().empty() &&
            manager.ListInvalidPackages().size() == 1,
            "an invalidated component cannot remain in the available catalogue"});
        std::filesystem::remove(package / L"native.dll");
        checks.push_back({manager.RefreshCatalog(error) && manager.ListPackages().size() == 1,
            "removing an unsafe file restores the validated component"});
        cache.Begin(roots);
        cache.Complete(revision);
        std::filesystem::rename(paths.builtin, temporary.path / L"old-builtin");
        std::filesystem::create_directory(paths.builtin);
        checks.push_back({!cache.CanReuse(revision),
            "replacing a watched root invalidates it through its parent watch"});
        checks.push_back({manager.RefreshCatalog(error) && manager.ListPackages().empty(),
            "replaced roots cannot retain components from the old directory"});
        cache.Begin({temporary.path / L"missing", paths.installed, paths.development});
        cache.Complete(revision);
        checks.push_back({!cache.CanReuse(revision),
            "unavailable watches conservatively fall back to full catalogue refresh"});
    }
    for (const auto& [passed, message] : checks) Check(passed, message);
}

// Exercise the exact file-export core used by the settings worker: real ZIP
// validation, identity changes, and Windows replacement failures, without a UI picker.
void TestCreationPresetManifest()
{
    using namespace snowdesktop::widget;
    snowdesktop::test::TemporaryDirectory temporary;
    const auto manifestPath = temporary.path / L"widget.json";
    const auto write = [&](std::string presets, std::string features) {
        std::ofstream file(manifestPath, std::ios::binary | std::ios::trunc);
        file << R"({"schemaVersion":2,"apiVersion":2,"dataVersion":1,
            "id":"3fbb18cd-7c46-4a9f-9fe3-3e2c19facb23","slug":"preset-test",
            "version":"1.0.0","entry":"main.lua","name":"Preset Test",
            "creationPresets":)" << presets << ",\"requiredFeatures\":" << features << "}";
    };
    const std::string valid = R"([{"id":"cpu","title":"CPU","values":{"metric":"cpu"}},
        {"id":"gpu","title":"GPU","values":{"metric":"gpu"}},
        {"id":"memory","title":"Memory","values":{"metric":"memory"}},
        {"id":"vram","title":"VRAM","values":{"metric":"vram"}},
        {"id":"battery","title":"Battery","values":{"metric":"battery"}},
        {"id":"disk","title":"Disk","values":{"metric":"disk"}},
        {"id":"network","title":"Network","values":{"metric":"network"}}])";
    write(valid, R"(["widget.creationPresets"])");
    PackageManifest manifest;
    ValidationReport report;
    Check(WidgetPackageValidator{}.ReadManifest(manifestPath, manifest, report) &&
        report.Ok() && manifest.creationPresets.size() == 7 &&
        manifest.creationPresets.back().values.at("metric") == "network",
        "all seven registered creation presets survive manifest parsing");
    write(valid, "[]");
    report = {};
    Check(!WidgetPackageValidator{}.ReadManifest(manifestPath, manifest, report),
        "creation presets cannot silently target an unaware host");
    write(R"([{"id":"cpu","title":"CPU","values":{"__contentTheme":"0"}}])",
        R"(["widget.creationPresets"])");
    report = {};
    Check(!WidgetPackageValidator{}.ReadManifest(manifestPath, manifest, report),
        "creation presets reject host appearance metadata");
    write(R"([{"id":"cpu","title":"CPU","values":{"metric":"cpu"}},
        {"id":"cpu","title":"GPU","values":{"metric":"gpu"}}])",
        R"(["widget.creationPresets"])");
    report = {};
    Check(!WidgetPackageValidator{}.ReadManifest(manifestPath, manifest, report),
        "duplicate creation preset identities are rejected");
    Check(snowdesktop::widget_api::SupportsFeature("widget.creationPresets"),
        "host advertises content creation presets separately from themes");
}

void TestPreviewGalleryManifest()
{
    using namespace snowdesktop::widget;
    snowdesktop::test::TemporaryDirectory temporary;
    const auto write = [&](const std::string& field) {
        std::ofstream manifest(temporary.path / "widget.json");
        manifest << R"({"schemaVersion":2,"apiVersion":2,"dataVersion":1,
            "id":"3fbb18cd-7c46-4a9f-9fe3-3e2c19facb23","slug":"gallery-test",
            "version":"1.0.0","entry":"main.lua","name":"Gallery Test")" << field << '}';
    };
    { std::ofstream lua(temporary.path / "main.lua"); lua << "return {}"; }
    { std::ofstream image(temporary.path / "gpu.png", std::ios::binary); image << "image"; }
    PackageManifest manifest;
    write("");
    Check(WidgetPackageValidator{}.ValidateDirectory(temporary.path, &manifest).Ok() &&
        !manifest.previewGallery, "legacy packages preserve the remote gallery");
    write(R"(,"previewGallery":[])");
    Check(WidgetPackageValidator{}.ValidateDirectory(temporary.path, &manifest).Ok() &&
        manifest.previewGallery && manifest.previewGallery->empty(), "explicit empty gallery requests managed-image removal");
    write(R"(,"previewGallery":["gpu.png"])");
    Check(WidgetPackageValidator{}.ValidateDirectory(temporary.path, &manifest).Ok() &&
        manifest.previewGallery->size() == 1, "gallery paths survive package validation");
    for (const auto& invalid : {R"(null)", R"([2])", R"(["../gpu.png"])",
            R"(["missing.png"])", R"(["main.lua"])", R"(["gpu.png","GPU.png"])",
            R"(["gpu.png","gpu.png","gpu.png","gpu.png","gpu.png","gpu.png","gpu.png","gpu.png"])"})
    {
        write(std::string(",\"previewGallery\":") + invalid);
        Check(!WidgetPackageValidator{}.ValidateDirectory(temporary.path).Ok(), "invalid gallery is rejected before packaging");
    }
    { std::ofstream image(temporary.path / "gpu.png", std::ios::binary | std::ios::trunc);
      const std::string bytes(1024 * 1024, 'x'); image << bytes; }
    write(R"(,"previewGallery":["gpu.png"])");
    Check(!WidgetPackageValidator{}.ValidateDirectory(temporary.path).Ok(), "Steam gallery size limit is exclusive at 1 MiB");
}

void TestAspectGridSizes()
{
    using namespace snowdesktop::widget;
    // Real page geometry, including configurable gaps and integer pitch.
    GridPage page;
    page.id = L"aspect-page";
    page.columns = 10; page.rows = 8;
    page.workArea = {0, 0, 1000, 1200};
    page.cellWidth = 90; page.cellHeight = 140;
    page.gapX = page.gapY = 10; page.marginX = page.marginY = 5;
    GridCell cell; cell.pageId = page.id;
    const WidgetSizeDefinition square{1, 2, AspectGridSize{1, true}};
    Check(ResolveWidgetSize(square, WidgetSizeMetricsForPage(&page, cell)) == std::pair{3, 2},
        "two rows resolve to three columns on a tall-cell screen");
    const auto frame = GetGridRect({page}, cell, {3, 2});
    Check(frame.right - frame.left == frame.bottom - frame.top,
        "resolved square uses actual outer grid frame dimensions");
    GridPage wide = page;
    wide.columns = 8; wide.workArea = {0, 0, 1200, 800};
    wide.cellWidth = 140; wide.cellHeight = 90;
    Check(ResolveWidgetSize(square, WidgetSizeMetricsForPage(&wide, cell)) == std::pair{1, 2},
        "the same definition resolves differently on a wide-cell screen");
    GridPage scaled = page;
    scaled.workArea = {0, 0, 2000, 2400};
    scaled.cellWidth *= 2; scaled.cellHeight *= 2;
    scaled.gapX *= 2; scaled.gapY *= 2; scaled.marginX *= 2; scaled.marginY *= 2;
    Check(ResolveWidgetSize(square, WidgetSizeMetricsForPage(&scaled, cell)) == std::pair{3, 2},
        "uniform DPI scaling preserves the chosen integer span");
    GridPage gaps = page;
    gaps.columns = 8; gaps.workArea = {0, 0, 1120, 880};
    gaps.cellWidth = 80; gaps.cellHeight = 110;
    gaps.gapX = 60; gaps.marginX = 30; gaps.gapY = gaps.marginY = 0;
    Check(ResolveWidgetSize(square, WidgetSizeMetricsForPage(&gaps, cell)) == std::pair{2, 2},
        "large horizontal spacing participates in the aspect calculation");
    const auto metrics = WidgetSizeMetricsForPage(&page, cell);
    Check(ResolveWidgetSize(square, metrics, 4) == std::pair{4, 2} &&
        ResolveWidgetSize(square, metrics, 1, 1, 2) == std::pair{2, 2},
        "aspect choice respects declared minimum and maximum columns");
    auto limited = metrics; limited.columns = 2; limited.rows = 1;
    Check(ResolveWidgetSize(square, limited).first <= 2 &&
        ResolveWidgetSize(square, limited).second == 1,
        "fixed axes and derived axes fit the destination page");
    Check(ResolveWidgetSize({3, 1, AspectGridSize{1, false}}, metrics) == std::pair{3, 2},
        "fixed columns derive the closest row count");
    Check(ResolveWidgetSize({2, 3, {}}, metrics) == std::pair{2, 3},
        "legacy grid sizes are independent of screen proportions");

    snowdesktop::test::TemporaryDirectory temporary;
    const auto source = temporary.path / L"component";
    std::filesystem::create_directory(source);
    { std::ofstream file(source / L"main.lua"); file << "return widget.define({})"; }
    const auto write = [&](const std::string& size, bool feature,
        const std::string& presetSize = R"({"rows":2,"aspectRatio":1})") {
        std::ofstream file(source / L"widget.json", std::ios::binary | std::ios::trunc);
        file << R"({"schemaVersion":2,"apiVersion":2,"dataVersion":1,
            "id":"3fbb18cd-7c46-4a9f-9fe3-3e2c19facb23","slug":"aspect-test",
            "version":"1.0.0","entry":"main.lua","name":"Aspect Test",
            "description":"Aspect size contract","license":"MIT","permissions":[],
            "defaultSize":)" << size << R"(,"requiredFeatures":["widget.creationPresets")"
            << (feature ? ",\"widget.size.aspectRatio\"" : "") << R"(],
            "previewData":{"variants":[{"id":"wide","title":"Wide","size":{"columns":3,"aspectRatio":2}}]},
            "creationPresets":[{"id":"cpu","title":"CPU","values":{"metric":"cpu"},
            "size":)" << presetSize << "}]}";
    };
    write(R"({"rows":2,"aspectRatio":1})", true);
    PackageManifest manifest;
    ValidationReport report;
    WidgetPackageValidator validator;
    Check(validator.ReadManifest(source / L"widget.json", manifest, report) && report.Ok() &&
        manifest.defaultAspectSize && manifest.defaultAspectSize->fixedRows &&
        manifest.previewVariants.size() == 1 && manifest.creationPresets.size() == 1 &&
        manifest.previewVariants.front().aspectSize && !manifest.previewVariants.front().aspectSize->fixedRows &&
        manifest.creationPresets.front().size && manifest.creationPresets.front().size->aspect,
        "default, preview and content-preset aspect sizes survive manifest parsing");
    const auto archive = temporary.path / L"aspect.snowwidget";
    PackagePaths paths; paths.staging = temporary.path / L"staging";
    WidgetPackageManager manager(paths);
    PackageArtifact artifact;
    std::string error;
    Check(manager.ExportDirectory(source, archive, artifact, report, error),
        "aspect package exports without flattening screen-dependent sizes");
    PackageManifest roundtrip;
    Check(manager.ValidateArchive(archive, &roundtrip).Ok() && roundtrip.defaultAspectSize &&
        roundtrip.defaultAspectSize->ratio == 1 && roundtrip.creationPresets.size() == 1 &&
        roundtrip.creationPresets.front().size && roundtrip.creationPresets.front().size->aspect,
        "archive roundtrip retains adaptive size definitions");
    write(R"({"rows":2,"aspectRatio":1})", false);
    report = {};
    Check(!validator.ReadManifest(source / L"widget.json", manifest, report),
        "new size semantics require an explicit host capability");
    for (const std::string invalid : {R"({"rows":2,"columns":2,"aspectRatio":1})",
        R"({"aspectRatio":1})", R"({"rows":0,"aspectRatio":1})",
        R"({"rows":2.5,"aspectRatio":1})", R"({"rows":9,"aspectRatio":1})",
        R"({"rows":2,"aspectRatio":0})", R"({"rows":2,"aspectRatio":-1})",
        R"({"rows":2,"aspectRatio":"1:1"})"})
    {
        write(invalid, true); report = {};
        Check(!validator.ReadManifest(source / L"widget.json", manifest, report),
            "invalid ratio or ambiguous/fractional anchor axes are rejected");
    }
    Check(snowdesktop::widget_api::SupportsFeature("widget.size.aspectRatio"),
        "host advertises aspect-ratio grid sizing");
    for (const std::string invalid : {R"({"columns":1e100,"rows":2})",
        R"({"columns":2,"rows":1e100})", R"({"columns":1.5,"rows":2})"})
    {
        write(R"({"rows":2,"aspectRatio":1})", true, invalid); report = {};
        Check(!validator.ReadManifest(source / L"widget.json", manifest, report),
            "invalid grid preset axes cannot silently become default dimensions");
    }
}

void TestStandalonePackageExport()
{
    using namespace snowdesktop::widget;
    using snowdesktop::widget::detail::ExportDevelopmentPackageFile;
    std::vector<std::pair<bool, const char*>> checks;
    {
        snowdesktop::test::TemporaryDirectory temporary;
        const auto source = temporary.path / L"component";
        std::filesystem::create_directory(source);
        const std::string id = "3fbb18cd-7c46-4a9f-9fe3-3e2c19facb23";
        const std::string manifest = R"json({
            "schemaVersion":2,"apiVersion":2,"dataVersion":1,
            "id":"3fbb18cd-7c46-4a9f-9fe3-3e2c19facb23","slug":"export-test",
            "name":"Export Test","version":"1.0.0","entry":"main.lua",
            "minHostVersion":"1.0.7.0","author":"Test","license":"MIT",
            "description":"Export fixture","defaultSize":{"columns":1,"rows":1},
            "permissions":[],"optionalPermissions":[],"requiredFeatures":[],"optionalFeatures":[]
        })json";
        { std::ofstream file(source / L"widget.json", std::ios::binary); file << manifest; }
        { std::ofstream file(source / L"main.lua", std::ios::binary); file << "return widget.define({})"; }
        const auto output = temporary.path / L"standalone.snowwidget";
        auto result = ExportDevelopmentPackageFile(source, output, id, "1.0.0");
        if (!result.succeeded) std::cerr << result.error << "\n" << result.report.ToJson() << '\n';
        checks.push_back({result.succeeded, "settings export produces an independent .snowwidget file"});
        PackagePaths validationPaths;
        validationPaths.staging = temporary.path / L"validation";
        WidgetPackageManager validator(std::move(validationPaths));
        PackageManifest exported;
        checks.push_back({validator.ValidateArchive(output, &exported).Ok() &&
            exported.id == id && exported.version == "1.0.0",
            "exported archive passes the real installer validator with its selected identity"});
        const std::string originalHash = WidgetPackageManager::Sha256File(output);
        checks.push_back({!originalHash.empty(), "exported file has a SHA256 fingerprint"});
        result = ExportDevelopmentPackageFile(source, output,
            "11111111-1111-4111-8111-111111111111", "1.0.0");
        checks.push_back({!result.succeeded && WidgetPackageManager::Sha256File(output) == originalHash,
            "a changed component identity cannot overwrite the previous exported file"});
        { std::ofstream file(source / L"widget.json", std::ios::binary | std::ios::trunc); file << "{}"; }
        result = ExportDevelopmentPackageFile(source, output, id, "1.0.0");
        checks.push_back({!result.succeeded && !result.report.Ok() &&
            WidgetPackageManager::Sha256File(output) == originalHash,
            "invalid source reports validation errors and preserves the old export"});
        { std::ofstream file(source / L"widget.json", std::ios::binary | std::ios::trunc); file << manifest; }
        HANDLE reader = CreateFileW(output.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        result = ExportDevelopmentPackageFile(source, output, id, "1.0.0");
        if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
        checks.push_back({reader != INVALID_HANDLE_VALUE && !result.succeeded &&
            WidgetPackageManager::Sha256File(output) == originalHash,
            "a destination locked by another reader fails without truncating it"});
        { std::ofstream file(source / L"main.lua", std::ios::binary | std::ios::trunc); file << "return widget.define({model = {exported = true}})"; }
        result = ExportDevelopmentPackageFile(source, output, id, "1.0.0");
        checks.push_back({result.succeeded && validator.ValidateArchive(output).Ok() &&
            WidgetPackageManager::Sha256File(output) != originalHash,
            "confirmed replacement publishes a complete updated package"});
        result = ExportDevelopmentPackageFile(source, temporary.path / L"wrong.zip", id, "1.0.0");
        checks.push_back({!result.succeeded && !std::filesystem::exists(temporary.path / L"wrong.zip"),
            "independent export enforces the installer package extension"});
        result = ExportDevelopmentPackageFile(source, source / L"nested.snowwidget", id, "1.0.0");
        checks.push_back({!result.succeeded && !std::filesystem::exists(source / L"nested.snowwidget") &&
            std::distance(std::filesystem::directory_iterator(source),
                std::filesystem::directory_iterator{}) == 2,
            "export cannot modify its source or recursively include a previous package"});
        bool temporaryFilesRemain = false;
        for (const auto& item : std::filesystem::directory_iterator(temporary.path))
            temporaryFilesRemain |= item.path().filename().wstring().starts_with(L".snowwidget-export-");
        checks.push_back({!temporaryFilesRemain, "success and failure both clean their owned export staging files"});
    }
    for (const auto& [passed, message] : checks) Check(passed, message);
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

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string(argv[1]) == "--package-export")
    {
        TestStandalonePackageExport();
        std::cout << "standalone package export tests passed\n";
        return 0;
    }
    TestReviewedPackageInstallation();
    TestCatalogueRefreshCache();
    TestPermissionReport();
    TestCreationPresetManifest();
    TestAspectGridSizes();
    TestPreviewGalleryManifest();
    Check(snowdesktop::widget_api::SupportsFeature("widget.confirmRemoval"),
        "host advertises removal confirmation for protected components");
    std::cout << "widget author tools tests passed\n";
    return 0;
}
