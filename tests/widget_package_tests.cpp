#include "full_data_backup.h"
#include "widget_package.h"
#include "portable_data_migration.h"
#include "single_instance.h"

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

std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

bool CorruptArchivePayload(const std::filesystem::path& path,
    const std::string& payload)
{
    std::fstream file(path,
        std::ios::binary | std::ios::in | std::ios::out);
    if (!file)
        return false;
    std::vector<char> bytes;
    bytes.assign(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
    const auto found = std::search(
        bytes.begin(), bytes.end(), payload.begin(), payload.end());
    if (found == bytes.end())
        return false;
    const auto position =
        static_cast<std::streamoff>(found - bytes.begin());
    file.clear();
    file.seekp(position);
    const char replacement =
        payload.front() == 'X' ? 'Y' : 'X';
    file.write(&replacement, 1);
    return static_cast<bool>(file);
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

    Expect(snowdesktop::single_instance::
            ParseRestartPredecessorProcessId(
                L"\"C:\\Apps\\SnowDesktop.exe\" --wait-for-pid=4321") ==
            4321,
        "restart predecessor PID is parsed from the internal command line");
    Expect(snowdesktop::single_instance::
            ParseRestartPredecessorProcessId(
                L"\"C:\\Apps\\SnowDesktop.exe\" --wait-for-pid=0") == 0 &&
        snowdesktop::single_instance::
            ParseRestartPredecessorProcessId(
                L"\"C:\\Apps\\SnowDesktop.exe\" x--wait-for-pid=42") == 0,
        "invalid or embedded restart predecessor arguments are rejected");
    Expect(snowdesktop::single_instance::VersionsMatch(
            L"1.0.1", L"1.0.1.0") &&
        !snowdesktop::single_instance::VersionsMatch(
            L"1.0.1.0", L"1.0.2.0") &&
        !snowdesktop::single_instance::VersionsMatch(
            L"1.0.1.", L"1.0.1.0"),
        "instance versions compare normalized numeric parts");
    Expect(snowdesktop::single_instance::DataDirectoriesMatch(
            L"C:\\SnowDesktop\\data\\",
            L"c:/snowdesktop/data") &&
        !snowdesktop::single_instance::DataDirectoriesMatch(
            L"C:\\SnowDesktop\\data",
            L"D:\\SnowDesktop\\data"),
        "instance data directories compare canonical path spelling");
    const std::wstring testMutexName =
        L"Local\\SnowDesktop.SingleInstance.Test." +
        std::to_wstring(GetCurrentProcessId());
    {
        snowdesktop::single_instance::Guard firstInstance;
        snowdesktop::single_instance::Guard secondInstance;
        Expect(firstInstance.Acquire(testMutexName.c_str()) ==
                snowdesktop::single_instance::AcquireResult::Primary,
            "the first executable location owns the shared instance lock");
        Expect(secondInstance.Acquire(testMutexName.c_str()) ==
                snowdesktop::single_instance::AcquireResult::Existing,
            "a second executable location sees the existing instance");
    }
    {
        snowdesktop::single_instance::Guard replacementInstance;
        Expect(replacementInstance.Acquire(testMutexName.c_str()) ==
                snowdesktop::single_instance::AcquireResult::Primary,
            "the shared instance lock is released when the owner exits");
    }

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

    const auto portableWidgets = root / L"portable-widget-import" / L"widgets";
    Write(portableWidgets / L"my_legacy.lua",
        "function render() end\n");
    Write(portableWidgets / L"my_legacy.widget.json",
        "{ \"name\": \"My Legacy Widget\", \"version\": \"1.0.0\" }\n");
    Write(portableWidgets / L"orphan.lua",
        "function render() end\n");
    MakePackage(portableWidgets / L"folder-package", "1.0.0");
    Write(portableWidgets / L"snowdesktop-lua-widget" / L"SKILL.md",
        "# Authoring tool\n");
    Write(portableWidgets / L"README.txt", "not component data\n");
    const auto importedPortableWidgets =
        root / L"portable-widget-import" / L"staging-data" / L"widgets";
    const auto portableImport = ImportLegacyLooseWidgetPairs(
        portableWidgets, importedPortableWidgets);
    Expect(portableImport.ok && portableImport.copiedPairs == 1,
        "portable migration imports only complete legacy loose pairs");
    Expect(std::filesystem::is_regular_file(
        importedPortableWidgets / L"my_legacy.lua") &&
        std::filesystem::is_regular_file(
            importedPortableWidgets / L"my_legacy.widget.json"),
        "portable migration preserves the user-authored legacy pair");
    Expect(!std::filesystem::exists(
        importedPortableWidgets / L"orphan.lua"),
        "portable migration ignores orphaned Lua files");
    Expect(!std::filesystem::exists(
        importedPortableWidgets / L"folder-package") &&
        !std::filesystem::exists(
            importedPortableWidgets / L"snowdesktop-lua-widget") &&
        !std::filesystem::exists(
            importedPortableWidgets / L"README.txt"),
        "portable migration does not copy folder packages or authoring files");
    const auto missingPortableImport = ImportLegacyLooseWidgetPairs(
        root / L"portable-widget-import" / L"missing",
        importedPortableWidgets);
    Expect(missingPortableImport.ok &&
        missingPortableImport.copiedPairs == 0,
        "portable migration accepts a missing legacy widgets directory");

    const auto longPathSource =
        root / L"portable-long-path-source";
    std::filesystem::path longRelative;
    for (int i = 0; i < 5; ++i)
    {
        longRelative /=
            std::wstring(24, static_cast<wchar_t>(L'a' + i));
    }
    Write(longPathSource / longRelative /
            L"SnowDesktop.layout.json",
        "{ \"source\": \"portable-long-path\" }\n");
    const auto longPathDestination =
        root / L"portable-long-path-destination" /
        std::wstring(72, L'd');
    const auto longCopy = snowdesktop::migration::CopyDataTree(
        longPathSource, longPathDestination);
    const auto longCopiedFile = std::filesystem::absolute(
        longPathDestination / longRelative /
            L"SnowDesktop.layout.json");
    const std::wstring extendedLongCopiedFile =
        std::wstring(LR"(\\?\)") + longCopiedFile.wstring();
    Expect(longCopiedFile.wstring().size() > MAX_PATH,
        "portable migration long-path test exceeds legacy MAX_PATH");
    Expect(longCopy.ok && longCopy.files == 1,
        "portable migration copies data through extended-length paths");
    Expect(GetFileAttributesW(extendedLongCopiedFile.c_str()) !=
            INVALID_FILE_ATTRIBUTES,
        "portable migration creates the long destination file");

    const auto portableState =
        root / L"restart-safe-portable-migration";
    Write(portableState / L"data" / L"SnowDesktop.layout.json",
        "{ \"source\": \"installed\" }\n");
    const std::wstring portableToken = L"20260730-120000-42-100";
    const auto portableStage = portableState / L"TempState" /
        L"PortableMigration" / (L"staging-" + portableToken);
    Write(portableStage / L"SnowDesktop.layout.json",
        "{ \"source\": \"portable\" }\n");
    Write(portableStage / L"widgets" / L"packages.json",
        "{ \"schemaVersion\": 1, \"packages\": [] }\n");
    error.clear();
    Expect(snowdesktop::migration::Queue(
        portableState, portableToken, error),
        "portable data replacement is queued for the next startup");
    const auto pendingApply =
        snowdesktop::migration::ApplyPending(portableState);
    Expect(pendingApply.ok && pendingApply.pending &&
        pendingApply.applied,
        "queued portable data is applied before runtime initialization");
    Expect(std::filesystem::is_regular_file(
        portableState / L"data" / L"widgets" / L"packages.json"),
        "portable data becomes the active data directory");
    Expect(std::filesystem::is_regular_file(
        pendingApply.backup / L"SnowDesktop.layout.json"),
        "the previous installed data is retained as a complete backup");
    Expect(!std::filesystem::exists(
        portableState / L"TempState" / L"PortableMigration" /
            L"pending.txt"),
        "the pending marker is retired after a successful exchange");
    const auto noPendingApply =
        snowdesktop::migration::ApplyPending(portableState);
    Expect(noPendingApply.ok && !noPendingApply.pending &&
        !noPendingApply.applied,
        "completed portable migration is not repeated");

    const auto invalidPortableState =
        root / L"invalid-restart-safe-portable-migration";
    const std::wstring invalidToken = L"20260730-120100-42-200";
    std::filesystem::create_directories(invalidPortableState / L"data");
    std::filesystem::create_directories(invalidPortableState /
        L"TempState" / L"PortableMigration" /
        (L"staging-" + invalidToken));
    error.clear();
    Expect(!snowdesktop::migration::Queue(
        invalidPortableState, invalidToken, error),
        "invalid staged data is rejected before active data is touched");
    Expect(std::filesystem::is_directory(
        invalidPortableState / L"data"),
        "failed queue validation preserves current installed data");

    const auto fullBackupState =
        root / L"complete-data-backup";
    const auto fullBackupData =
        fullBackupState / L"data";
    const std::string originalLayout =
        "{ \"source\": \"complete-backup-original\" }\n";
    const std::string modifiedLayout =
        "{ \"source\": \"complete-backup-modified\" }\n";
    Write(fullBackupData / L"SnowDesktop.layout.json",
        originalLayout);
    Write(fullBackupData / L"SnowDesktop.general.json",
        "{ \"language\": \"zh-CN\" }\n");
    Write(fullBackupData / L"widgets" / L"installed" /
        L"package-id" / L"1.0.0" / L"main.lua",
        "function render() end\n");
    Write(fullBackupData / L"widgets" / L"storage" /
        L"package-id" / L"instance-id.json",
        "{ \"counter\": 27 }\n");
    Write(fullBackupData / L"SnowDesktop_crash.log",
        "excluded log\n");
    Write(fullBackupData / L"crashdumps" / L"test.dmp",
        "excluded dump\n");
    Write(fullBackupData / L"widgets" / L"staging" /
        L"temporary.txt", "excluded staging\n");
    Write(fullBackupData / L"widgets" / L"quarantine" /
        L"bad.txt", "excluded quarantine\n");
    std::filesystem::path longBackupRelative;
    for (int index = 0; index < 4; ++index)
    {
        longBackupRelative /=
            std::wstring(32, static_cast<wchar_t>(L'k' + index));
    }
    longBackupRelative /= L"long-state.json";
    Write(fullBackupData / longBackupRelative,
        "{ \"longPath\": true }\n");

    snowdesktop::backup::FullDataBackupManager fullBackupManager(
        fullBackupState, fullBackupData, "1.0.1.0", "portable");
    const auto createdFullBackup = fullBackupManager.Create();
    Expect(createdFullBackup.ok &&
        std::filesystem::is_regular_file(
            createdFullBackup.backup.root / L"backup.json"),
        "complete data backup is created with a manifest");
    Expect(std::filesystem::is_regular_file(
            createdFullBackup.backup.data /
                L"SnowDesktop.layout.json") &&
        std::filesystem::is_regular_file(
            createdFullBackup.backup.data / L"widgets" / L"storage" /
                L"package-id" / L"instance-id.json"),
        "complete backup preserves layout, settings, packages, and storage");
    Expect(!std::filesystem::exists(
            createdFullBackup.backup.data /
                L"SnowDesktop_crash.log") &&
        !std::filesystem::exists(
            createdFullBackup.backup.data / L"crashdumps") &&
        !std::filesystem::exists(
            createdFullBackup.backup.data / L"widgets" / L"staging") &&
        !std::filesystem::exists(
            createdFullBackup.backup.data / L"widgets" / L"quarantine"),
        "complete backup excludes logs, dumps, staging, and quarantine");
    const auto longBackupFile = std::filesystem::absolute(
        createdFullBackup.backup.data / longBackupRelative);
    const std::wstring extendedLongBackupFile =
        longBackupFile.wstring().starts_with(LR"(\\?\)")
            ? longBackupFile.wstring()
            : std::wstring(LR"(\\?\)") +
                longBackupFile.wstring();
    Expect(longBackupFile.wstring().size() > MAX_PATH &&
        GetFileAttributesW(extendedLongBackupFile.c_str()) !=
            INVALID_FILE_ATTRIBUTES,
        "complete backup supports destination paths beyond MAX_PATH");
    const auto completeBackupList = fullBackupManager.List();
    Expect(!completeBackupList.empty() &&
        !completeBackupList.front().migrationRollback &&
        completeBackupList.front().fileCount >= 4,
        "complete backup appears in the managed backup list");

    const auto exportedBackup =
        root / L"exports" / L"complete.snowbackup";
    const auto exportResult = fullBackupManager.Export(
        createdFullBackup.backup, exportedBackup);
    Expect(exportResult.ok &&
        std::filesystem::is_regular_file(exportedBackup),
        "complete backup exports as a standard snowbackup archive");

    Write(fullBackupData / L"SnowDesktop.layout.json",
        modifiedLayout);
    const auto queuedRestore =
        fullBackupManager.QueueRestore(createdFullBackup.backup);
    Expect(queuedRestore.ok,
        "complete backup restore is queued without touching active data");
    Expect(Read(fullBackupData / L"SnowDesktop.layout.json") ==
            modifiedLayout,
        "queued complete backup restore leaves active data unchanged");
    const auto appliedRestore =
        snowdesktop::migration::ApplyPending(fullBackupState);
    Expect(appliedRestore.ok && appliedRestore.applied &&
        Read(fullBackupData / L"SnowDesktop.layout.json") ==
            originalLayout,
        "complete backup is atomically restored on the next startup");
    Expect(Read(appliedRestore.backup /
            L"SnowDesktop.layout.json") == modifiedLayout,
        "pre-restore active data is retained as a rollback backup");

    const auto backupsAfterRestore = fullBackupManager.List();
    const auto rollbackBackup = std::find_if(
        backupsAfterRestore.begin(), backupsAfterRestore.end(),
        [](const snowdesktop::backup::BackupInfo& backup) {
            return backup.migrationRollback;
        });
    Expect(rollbackBackup != backupsAfterRestore.end(),
        "pre-restore data is visible in the complete backup list");
    if (rollbackBackup != backupsAfterRestore.end())
    {
        const auto queuedRollback =
            fullBackupManager.QueueRestore(*rollbackBackup);
        const auto appliedRollback =
            snowdesktop::migration::ApplyPending(fullBackupState);
        Expect(queuedRollback.ok && appliedRollback.ok &&
            appliedRollback.applied &&
            Read(fullBackupData / L"SnowDesktop.layout.json") ==
                modifiedLayout,
            "a pre-migration backup can restore the previous data");
    }

    const auto importedBackupState =
        root / L"imported-complete-data-backup";
    const auto importedBackupData =
        importedBackupState / L"data";
    Write(importedBackupData / L"SnowDesktop.layout.json",
        "{ \"source\": \"before-archive-import\" }\n");
    snowdesktop::backup::FullDataBackupManager importBackupManager(
        importedBackupState, importedBackupData,
        "1.0.1.0", "installed");
    const auto importResult =
        importBackupManager.ImportAndQueue(exportedBackup);
    if (!importResult.ok)
    {
        std::cerr << "complete backup import error: "
            << importResult.error << '\n';
    }
    Expect(importResult.ok,
        "a snowbackup archive is verified and queued for restore");
    const auto appliedImport =
        snowdesktop::migration::ApplyPending(importedBackupState);
    Expect(appliedImport.ok && appliedImport.applied &&
        Read(importedBackupData / L"SnowDesktop.layout.json") ==
            originalLayout,
        "a snowbackup archive restores complete data after restart");

    const auto corruptedBackup =
        root / L"exports" / L"corrupted.snowbackup";
    std::filesystem::copy_file(exportedBackup, corruptedBackup,
        std::filesystem::copy_options::overwrite_existing, ec);
    Expect(CorruptArchivePayload(
            corruptedBackup, originalLayout),
        "complete backup corruption test modifies an archive payload");
    const auto corruptedImport =
        importBackupManager.ImportAndQueue(corruptedBackup);
    Expect(!corruptedImport.ok &&
        Read(importedBackupData / L"SnowDesktop.layout.json") ==
            originalLayout,
        "corrupted backup is rejected without replacing active data");

    const auto unsafeBackup =
        root / L"exports" / L"unsafe.snowbackup";
    MakeUnsafeArchive(unsafeBackup, { "../escape.txt" });
    const auto unsafeImport =
        importBackupManager.ImportAndQueue(unsafeBackup);
    Expect(!unsafeImport.ok &&
        !std::filesystem::exists(
            importedBackupState / L"escape.txt"),
        "backup archive path traversal is rejected");

    const auto deletedBackup =
        fullBackupManager.Delete(createdFullBackup.backup);
    Expect(deletedBackup.ok &&
        !std::filesystem::exists(createdFullBackup.backup.root),
        "complete backup can be deleted from managed storage");

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
