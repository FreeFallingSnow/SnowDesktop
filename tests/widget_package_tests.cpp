#include "widget_package.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace snowdesktop::widget;

int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

void Write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

void Write16(std::ofstream& output, std::uint16_t value)
{
    output.put(static_cast<char>(value & 0xff));
    output.put(static_cast<char>((value >> 8) & 0xff));
}

void Write32(std::ofstream& output, std::uint32_t value)
{
    Write16(output, static_cast<std::uint16_t>(value & 0xffff));
    Write16(output, static_cast<std::uint16_t>(value >> 16));
}

void MakeUnsafeArchive(const std::filesystem::path& path,
    const std::vector<std::string>& names)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    for (const auto& name : names)
    {
        Write32(output, 0x04034b50);
        Write16(output, 20);
        Write16(output, 0x0800);
        Write16(output, 0);
        Write16(output, 0);
        Write16(output, 0);
        Write32(output, 0);
        Write32(output, 0);
        Write32(output, 0);
        Write16(output, static_cast<std::uint16_t>(name.size()));
        Write16(output, 0);
        output.write(name.data(), static_cast<std::streamsize>(name.size()));
    }
}

void MakePackage(const std::filesystem::path& root, std::string version,
    std::string id = "3af4c6ab-15d3-4f2a-8b8c-80e57600a87d",
    std::string permissions = "\"ui.input\"",
    std::string networkDomains = "")
{
    Write(root / L"main.lua", "function render() end\n");
    Write(root / L"assets" / L"label.txt", "asset");
    Write(root / L"widget.json",
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"id\": \"" + id + "\",\n"
        "  \"slug\": \"package-test\",\n"
        "  \"version\": \"" + version + "\",\n"
        "  \"apiVersion\": 1,\n"
        "  \"dataVersion\": 1,\n"
        "  \"entry\": \"main.lua\",\n"
        "  \"minHostVersion\": \"1.0.1.0\",\n"
        "  \"name\": \"Package Test\",\n"
        "  \"description\": \"English fallback\",\n"
        "  \"author\": \"SnowDesktop\",\n"
        "  \"license\": \"GPL-3.0-only\",\n"
        "  \"permissions\": [" + permissions + "],\n"
        "  \"networkDomains\": [" + networkDomains + "]\n"
        "}\n");
}

PackagePaths TestPaths(const std::filesystem::path& root)
{
    PackagePaths paths;
    paths.builtin = root / L"builtin";
    paths.installed = root / L"data" / L"widgets" / L"installed";
    paths.development = root / L"data" / L"widgets" / L"dev";
    paths.staging = root / L"data" / L"widgets" / L"staging";
    paths.quarantine = root / L"data" / L"widgets" / L"quarantine";
    paths.migrations = root / L"data" / L"widgets" / L"migrations";
    paths.registry = root / L"data" / L"widgets" / L"packages.json";
    return paths;
}
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktopWidgetPackageTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);

    const auto sourceV1 = root / L"source-v1";
    MakePackage(sourceV1, "1.0.0");
    WidgetPackageValidator validator;
    PackageManifest manifest;
    auto report = validator.ValidateDirectory(sourceV1, &manifest);
    Expect(report.Ok(), "valid folder package is accepted");
    Expect(manifest.entry == "main.lua", "entry is parsed");
    Expect(WidgetPackageValidator::IsUuid(manifest.id), "UUID is valid");
    Expect(WidgetPackageValidator::IsSemVer("1.2.3-beta.1+build.7"),
        "SemVer prerelease is valid");
    Expect(!WidgetPackageValidator::IsSafeRelativePath(L"../escape.lua"),
        "parent traversal is rejected");
    manifest.locales["en-US"] = { "English title", "English description" };
    manifest.locales["zh-CN"] = { "中文标题", "中文介绍" };
    Expect(LocalizePackageManifest(manifest, "zh-CN").name == "中文标题",
        "package metadata uses the exact requested locale");
    Expect(LocalizePackageManifest(manifest, "ZH-cn").description ==
            "中文介绍",
        "package locale matching is case insensitive");
    Expect(LocalizePackageManifest(manifest, "zh-Hans").name == "中文标题",
        "package metadata falls back to the requested language family");
    Expect(LocalizePackageManifest(manifest, "fr-FR").name == manifest.name,
        "unknown package locale keeps the English manifest fallback");

    const auto badSource = root / L"bad";
    MakePackage(badSource, "1.0.0",
        "not-a-uuid");
    Write(badSource / L"escape.exe", "MZ");
    report = validator.ValidateDirectory(badSource);
    Expect(!report.Ok(), "invalid UUID and executable payload are rejected");
    const auto badNetwork = root / L"bad-network";
    MakePackage(badNetwork, "1.0.0",
        "cb0e23fb-346f-4495-8622-ecad61865167",
        "\"network.http\"", "\"*.example.com\"");
    Expect(!validator.ValidateDirectory(badNetwork).Ok(),
        "wildcard network domains are rejected");

    std::string error;
    const auto managerPaths = TestPaths(root / L"manager");
    std::filesystem::create_directories(managerPaths.builtin);
    std::filesystem::copy(sourceV1, managerPaths.builtin / L"package-test",
        std::filesystem::copy_options::recursive, ec);
    WidgetPackageManager manager(managerPaths);
    Expect(manager.Initialize(error), "package manager initializes");
    Expect(manager.Resolve(manifest.id)->builtin,
        "built-in package is the initial active source");
    InstalledPackage installed;
    report = {};
    Expect(manager.InstallDirectory(sourceV1, { "local", "package-test" },
        false, installed, report, error), "folder package installs");
    Expect(manager.Resolve(manifest.id).has_value(), "installed package resolves");
    Expect(manager.ResolveEntry(manifest.id).value_or(L"").filename() == L"main.lua",
        "entry resolves inside the package");
    Expect(manager.SetEnabled(manifest.id, false, error),
        "installed package can be disabled");
    Expect(!manager.Resolve(manifest.id).has_value(),
        "disabled package does not silently fall back to a built-in source");
    Expect(manager.SetEnabled(manifest.id, true, error),
        "installed package can be re-enabled");

    const auto sourceV2 = root / L"source-v2";
    MakePackage(sourceV2, "1.1.0");
    error.clear();
    Expect(manager.InstallDirectory(sourceV2, { "other-provider", "remote-42" },
        false, installed, report, error) == false,
        "silent cross-provider update is rejected");
    error.clear();
    Expect(manager.InstallDirectory(sourceV2, { "local", "package-test" },
        false, installed, report, error), "same-source update installs");
    Expect(manager.Resolve(manifest.id)->manifest.version == "1.1.0",
        "new version becomes active");

    const auto sourceV3 = root / L"source-v3";
    MakePackage(sourceV3, "1.2.0",
        "3af4c6ab-15d3-4f2a-8b8c-80e57600a87d",
        "\"ui.input\", \"ui.notify\"");
    LocalDirectorySource localSource(root);
    PackageQuery page;
    page.offset = 1;
    page.limit = 1;
    Expect(localSource.Query(page, error).size() == 1,
        "local source applies pagination to matched packages");
    error.clear();
    Expect(!manager.InstallDirectory(sourceV3, { "local", "package-test" },
        false, installed, report, error),
        "permission expansion requires confirmation");
    error.clear();
    Expect(manager.InstallDirectory(sourceV3, { "local", "package-test" },
        false, installed, report, error, true),
        "confirmed permission expansion installs");
    Expect(installed.grantedPermissions.size() == 2,
        "permission snapshot is persisted");
    error.clear();
    Expect(manager.Rollback(manifest.id, "1.0.0", error),
        "known-good version can be restored");
    Expect(manager.Resolve(manifest.id)->manifest.version == "1.0.0",
        "rollback updates active version");

    const auto archive = root / L"exports" / L"package-test.snowwidget";
    PackageArtifact artifact;
    error.clear();
    Expect(manager.ExportArchive(manifest.id, archive, artifact, report, error),
        "folder package exports to .snowwidget");
    Expect(!artifact.sha256.empty(), "export records SHA-256");
    Expect(manager.ValidateArchive(archive).Ok(),
        "archive validation securely extracts and validates the package");
    const auto corruptArchive = root / L"exports" / L"corrupt.snowwidget";
    std::filesystem::copy_file(archive, corruptArchive,
        std::filesystem::copy_options::overwrite_existing, ec);
    {
        std::fstream corrupt(corruptArchive,
            std::ios::binary | std::ios::in | std::ios::out);
        std::vector<char> bytes((std::istreambuf_iterator<char>(corrupt)),
            std::istreambuf_iterator<char>());
        const std::string needle = "function render";
        const auto found = std::search(bytes.begin(), bytes.end(),
            needle.begin(), needle.end());
        if (found != bytes.end())
        {
            const auto position = std::distance(bytes.begin(), found);
            bytes[static_cast<std::size_t>(position)] ^= 0x01;
            corrupt.clear();
            corrupt.seekp(0);
            corrupt.write(bytes.data(),
                static_cast<std::streamsize>(bytes.size()));
        }
    }
    Expect(!manager.ValidateArchive(corruptArchive).Ok(),
        "archive CRC corruption is rejected");
    const auto traversalArchive =
        root / L"exports" / L"traversal.snowwidget";
    MakeUnsafeArchive(traversalArchive, { "../escape.lua" });
    Expect(!manager.ValidateArchive(traversalArchive).Ok(),
        "ZIP path traversal is rejected before extraction");
    const auto collisionArchive =
        root / L"exports" / L"case-collision.snowwidget";
    MakeUnsafeArchive(collisionArchive, { "Assets/icon.png", "assets/icon.png" });
    Expect(!manager.ValidateArchive(collisionArchive).Ok(),
        "case-insensitive ZIP path collisions are rejected");

    WidgetPackageManager importedManager(TestPaths(root / L"imported"));
    error.clear();
    Expect(importedManager.Initialize(error), "second manager initializes");
    InstalledPackage imported;
    Expect(importedManager.InstallArchive(archive,
        { "static-catalog", "remote-42" }, false, imported, report, error),
        "exported archive installs through staging");
    Expect(imported.manifest.id == manifest.id, "archive identity is preserved");

    LocalCatalogPublisher publisher(root / L"catalog");
    PublishRequest request;
    request.artifact = artifact;
    request.title = "Package Test";
    request.description = "Published locally";
    int progressCalls = 0;
    request.progress = [&](std::uint64_t, std::uint64_t)
    {
        ++progressCalls;
        return true;
    };
    const auto publishResult = publisher.Publish(request);
    Expect(publishResult.ok, "local catalog publisher creates an index");
    Expect(progressCalls >= 2, "local publisher reports upload progress");

    error.clear();
    Expect(manager.Rollback(manifest.id, "1.2.0", error),
        "newer version can be selected for a second publication");
    const auto archiveV2 = root / L"exports" / L"package-test-v2.snowwidget";
    PackageArtifact artifactV2;
    Expect(manager.ExportArchive(manifest.id, archiveV2, artifactV2,
        report, error), "second package version exports");
    request.artifact = artifactV2;
    request.externalItemId = publishResult.externalItemId;
    request.changeNotes = "Second test version";
    const auto publishResultV2 = publisher.Publish(request);
    Expect(publishResultV2.ok, "local catalog keeps multiple versions");

    StaticCatalogSource catalog(root / L"catalog" / L"catalog.json");
    Expect(catalog.Status().available, "static catalog reports source status");
    auto entries = catalog.Query({}, error);
    Expect(entries.size() == 1 && entries.front().versions.size() == 2,
        "static catalog groups multiple versions into one item");
    Expect(entries.front().manifest.version == "1.2.0",
        "static catalog selects the newest SemVer");
    const auto updates = catalog.CheckUpdates(
        { { manifest.id, "1.0.0" } }, error);
    Expect(updates.size() == 1 &&
        updates.front().available.manifest.version == "1.2.0",
        "static catalog checks installed versions for updates");
    const auto materialized = root / L"catalog-copy.snowwidget";
    Expect(catalog.Materialize(publishResult.externalItemId,
        artifact.version, materialized, error).has_value(),
        "static catalog materializes the requested older artifact");
    const auto materializedV2 = root / L"catalog-copy-v2.snowwidget";
    Expect(catalog.Materialize(publishResult.externalItemId,
        artifactV2.version, materializedV2, error).has_value(),
        "static catalog materializes the requested newer artifact");
    WidgetPackageManager catalogInstallManager(
        TestPaths(root / L"catalog-installed"));
    Expect(catalogInstallManager.Initialize(error),
        "catalog install manager initializes");
    InstalledPackage catalogInstalled;
    Expect(catalogInstallManager.InstallFromSource(catalog,
        publishResult.externalItemId, artifactV2.version, false,
        catalogInstalled, report, error),
        "package manager installs through the source contract");
    Expect(catalogInstalled.manifest.version == "1.2.0",
        "source installation activates the requested version");

    const auto automaticPaths = TestPaths(root / L"automatic-migration");
    constexpr const char* analogPackageId =
        "64107f41-197a-426a-8f86-6eeb020f56b0";
    MakePackage(automaticPaths.builtin / L"analog-clock", "1.0.0",
        analogPackageId);
    Write(automaticPaths.builtin / L"analog_clock.lua",
        "-- deliberately different from the replacement\n"
        "function render() error('old shipped component') end\n");
    Write(automaticPaths.builtin / L"analog_clock.widget.json",
        "{\n"
        "  \"name\": \"Analog Clock\",\n"
        "  \"nameKey\": \"lua_widget.analog_clock.name\",\n"
        "  \"version\": \"0.9.0\",\n"
        "  \"permissions\": [\"ui.input\"]\n"
        "}\n");
    const auto customLegacyRoot = automaticPaths.installed.parent_path();
    Write(customLegacyRoot / L"my_widget.lua",
        "function render() end\n");
    Write(customLegacyRoot / L"my_widget.widget.json",
        "{\n"
        "  \"name\": \"My Widget\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"permissions\": []\n"
        "}\n");
    const auto legacyStorage =
        automaticPaths.registry.parent_path().parent_path() /
            L"SnowDesktop.storage.json";
    const std::string legacyStorageText =
        "{\n  \"widget-instance.text\": \"keep me\"\n}\n";
    Write(legacyStorage, legacyStorageText);

    WidgetPackageManager automaticManager(automaticPaths);
    error.clear();
    Expect(automaticManager.Initialize(error),
        "manager initializes while replacing shipped loose components");
    const auto& automaticMigrations =
        automaticManager.AutomaticLegacyMigrationResults();
    Expect(automaticMigrations.size() == 1 &&
        automaticMigrations.front().ok,
        "shipped loose component is replaced without user interaction");
    Expect(!std::filesystem::exists(
        automaticPaths.builtin / L"analog_clock.lua") &&
        !std::filesystem::exists(
            automaticPaths.builtin / L"analog_clock.widget.json"),
        "replaced shipped loose files are deleted");
    Expect(automaticManager.ResolveLegacyPackageId(
        L"analog_clock.lua").value_or("") == analogPackageId,
        "legacy layout name resolves to the immutable built-in package id");
    Expect(automaticMigrations.front().backupDirectory.empty(),
        "shipped component files do not create a permanent migration backup");
    Expect(std::filesystem::is_empty(automaticPaths.migrations),
        "migrations directory remains reserved for user-authored components");
    const auto pendingStorage =
        automaticManager.PendingLegacyStoragePath();
    std::ifstream pendingStorageFile(pendingStorage, std::ios::binary);
    const std::string pendingStorageText(
        (std::istreambuf_iterator<char>(pendingStorageFile)),
        std::istreambuf_iterator<char>());
    Expect(pendingStorageText == legacyStorageText,
        "legacy instance storage is transactionally staged for engine import");

    const auto userLegacy = automaticManager.FindLegacyPackages();
    Expect(userLegacy.size() == 1 &&
        userLegacy.front().legacyName == L"my_widget.lua",
        "only user-authored loose components are offered in the migration UI");
    Expect(std::filesystem::exists(customLegacyRoot / L"my_widget.lua"),
        "user-authored loose component is not changed automatically");
    const auto userMigration =
        automaticManager.MigrateLegacy(userLegacy.front());
    Expect(userMigration.ok,
        "user-authored loose component migrates after explicit action");
    Expect(!userMigration.backupDirectory.empty() &&
        std::filesystem::exists(userMigration.backupDirectory),
        "explicit user migration retains its recovery backup");
    Expect(!std::filesystem::exists(customLegacyRoot / L"my_widget.lua"),
        "explicit user migration removes the loose source after backup");

    // MSIX replaces the read-only application directory as one unit. The old
    // official loose files therefore no longer exist when the upgraded
    // process first reads a legacy layout.
    const auto packagedUpgradePaths =
        TestPaths(root / L"packaged-folder-only-upgrade");
    MakePackage(packagedUpgradePaths.builtin / L"analog-clock", "1.0.0",
        analogPackageId);
    WidgetPackageManager packagedUpgradeManager(packagedUpgradePaths);
    error.clear();
    Expect(packagedUpgradeManager.Initialize(error),
        "manager initializes for an MSIX folder-only upgrade");
    Expect(packagedUpgradeManager.AutomaticLegacyMigrationResults().empty(),
        "folder-only MSIX upgrade does not require retired install files");
    Expect(packagedUpgradeManager.ResolveLegacyPackageId(
        L"analog_clock.lua").value_or("") == analogPackageId,
        "MSIX legacy layout maps to the built-in folder package without loose files");

    // A user may have created a component that happens to use an old official
    // filename. Its writable loose pair must remain eligible for the migration
    // wizard instead of being silently rebound to SnowDesktop's package.
    const auto packagedCollisionPaths =
        TestPaths(root / L"packaged-custom-name-collision");
    MakePackage(packagedCollisionPaths.builtin / L"analog-clock", "1.0.0",
        analogPackageId);
    const auto collisionRoot =
        packagedCollisionPaths.installed.parent_path();
    Write(collisionRoot / L"analog_clock.lua",
        "function render() end\n");
    Write(collisionRoot / L"analog_clock.widget.json",
        "{\n"
        "  \"name\": \"My Analog Clock\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"permissions\": []\n"
        "}\n");
    WidgetPackageManager packagedCollisionManager(packagedCollisionPaths);
    error.clear();
    Expect(packagedCollisionManager.Initialize(error),
        "manager initializes with a custom legacy filename collision");
    Expect(!packagedCollisionManager.ResolveLegacyPackageId(
        L"analog_clock.lua").has_value(),
        "custom loose component is not mistaken for an MSIX built-in");
    const auto packagedCollisionLegacy =
        packagedCollisionManager.FindLegacyPackages();
    Expect(packagedCollisionLegacy.size() == 1 &&
        packagedCollisionLegacy.front().legacyName == L"analog_clock.lua",
        "custom filename collision remains visible to the migration wizard");

    std::filesystem::remove_all(root, ec);
    if (failures)
        std::cerr << failures << " widget package test(s) failed\n";
    return failures == 0 ? 0 : 1;
}
