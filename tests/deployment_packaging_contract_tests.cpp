#include <algorithm>
#include "build_tool_test_support.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    std::string text = output.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

void TestExecutableVersionResources(const std::filesystem::path& root)
{
    namespace fs = std::filesystem;
    const auto fixture = build_test::temporary("executable-version-resources");
    build_test::copy(root, fixture, {
        "cmake/SnowDesktop.VersionResources.cmake",
        "src/resources/version.rc.in"});
    build_test::write(fixture / "version.json", R"({"version":"1.2.3.0"})");
    build_test::write(fixture / "main.cpp", "int main() { return 0; }\n");
    build_test::write(fixture / "third_party/vendor/CMakeLists.txt",
        "add_executable(VendorTool ../../main.cpp)\n");
    const std::string project = R"cmake(
cmake_minimum_required(VERSION 3.24)
project(ExecutableVersionFixture LANGUAGES CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SnowDesktop.VersionResources.cmake")
add_executable(VersionFixture main.cpp)
set_property(TARGET VersionFixture PROPERTY OUTPUT_NAME VersionFixtureOutput)
snowdesktop_add_executable_version_resource(VersionFixture "SnowDesktop version resource fixture")
add_executable(TestOnly main.cpp)
set_property(TARGET TestOnly PROPERTY RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_subdirectory(third_party/vendor)
)cmake";
    build_test::write(fixture / "CMakeLists.txt", project);
    const auto configure = [&]() {
        return build_test::run({L"cmake", L"-S", fixture.wstring(),
            L"-B", (fixture / "build").wstring()}, fixture, 45);
    };
    const auto generated = fixture / "build/generated/version/VersionFixture.version.rc";
    const auto initial = configure();
    build_test::require(initial.code == 0,
        "version resource fixture configures with third-party and test exemptions: " +
        initial.out + initial.err);
    const auto resource = ReadText(generated);
    for (const char* value : {
             "FILEVERSION 1,2,3,0", "PRODUCTVERSION 1,2,3,0",
             "\"CompanyName\", \"FreeFallingSnow", "\"ProductName\", \"SnowDesktop",
             "\"FileDescription\", \"SnowDesktop version resource fixture",
             "\"FileVersion\", \"1.2.3.0", "\"ProductVersion\", \"1.2.3.0",
             "\"OriginalFilename\", \"VersionFixtureOutput.exe",
             "\"InternalName\", \"VersionFixtureOutput.exe"})
    {
        Check(resource.find(value) != std::string::npos,
            "generated EXE metadata contains the shared version, publisher and target identity");
    }
    Check(resource.find("@SNOWDESKTOP_") == std::string::npos,
        "generated EXE metadata has no unresolved template placeholders");

    build_test::write(fixture / "version.json", R"({"version":"9.8.7.0"})");
    const auto updated = configure();
    build_test::require(updated.code == 0,
        "version resource fixture reconfigures after a version update: " + updated.out + updated.err);
    Check(ReadText(generated).find("FILEVERSION 9,8,7,0") != std::string::npos,
        "standalone EXE metadata follows version.json instead of a copied version");

    auto legacyResource = ReadText(generated);
    build_test::write(fixture / "legacy.rc", legacyResource);
    build_test::write(fixture / "CMakeLists.txt", project +
        "add_executable(LegacyVersion main.cpp legacy.rc)\n");
    const auto legacy = configure();
    Check(legacy.code == 0, "complete existing version resources remain supported");
    build_test::replace(legacyResource,
        "\"FileDescription\", \"SnowDesktop version resource fixture\\0\"",
        "\"FileDescription\", \"\"");
    build_test::write(fixture / "legacy.rc", legacyResource);
    const auto incomplete = configure();
    Check(incomplete.code != 0 &&
            (incomplete.out + incomplete.err).find("Shipping executable LegacyVersion") != std::string::npos,
        "empty file descriptions do not count as complete EXE metadata");

    // The deferred check must see targets added after the module is included,
    // and an unrelated .rc file must not qualify as a version resource.
    build_test::write(fixture / "icon-only.rc", "// No VERSIONINFO here.\n");
    build_test::write(fixture / "CMakeLists.txt", project +
        "add_executable(MissingVersion main.cpp icon-only.rc)\n");
    const auto missing = configure();
    Check(missing.code != 0 &&
            (missing.out + missing.err).find("Shipping executable MissingVersion") != std::string::npos,
        "a future shipping EXE with icon-only resources fails configuration");

    build_test::write(fixture / "CMakeLists.txt", project + "add_subdirectory(owned)\n");
    build_test::write(fixture / "owned/CMakeLists.txt",
        "add_executable(NestedMissing ../main.cpp)\n");
    const auto nested = configure();
    Check(nested.code != 0 &&
            (nested.out + nested.err).find("Shipping executable NestedMissing") != std::string::npos,
        "future shipping EXEs in first-party subdirectories are also checked");

    build_test::write(fixture / "CMakeLists.txt", project);
    build_test::write(fixture / "version.json", R"({"version":"1.2.3.4"})");
    const auto invalid = configure();
    Check(invalid.code != 0 &&
            (invalid.out + invalid.err).find("Store-compatible A.B.C.0") != std::string::npos,
        "standalone helper metadata rejects versions outside the shared Store contract");
    fs::remove_all(fixture);
}

void TestPinnedToolchain(const std::string& cmake,
    const std::string& shellViewProps)
{
    Check(cmake.find(
              "SNOWDESKTOP_WINDOWSAPPSDK_PACKAGE_VERSION \"2.4.0\"") !=
            std::string::npos,
        "Windows App SDK is pinned to 2.4.0");
    Check(cmake.find(
              "SNOWDESKTOP_CPPWINRT_PACKAGE_VERSION \"3.0.260818.1\"") !=
            std::string::npos,
        "C++/WinRT is pinned to 3.0.260818.1");
    Check(cmake.find("requires a Visual Studio/MSBuild generator") !=
            std::string::npos &&
            cmake.find("VS_GLOBAL_WindowsAppSDKSelfContained \"true\"") !=
                std::string::npos,
        "WinUI target requires MSBuild and self-contained deployment");
    Check(cmake.find("VS_GLOBAL_SnowDesktopCppWinRtPackageVersion") !=
                std::string::npos &&
            shellViewProps.find("$(NuGetPackageRoot)") !=
                std::string::npos &&
            shellViewProps.find("$(RestorePackagesPath)") !=
                std::string::npos &&
            shellViewProps.find("$(NUGET_PACKAGES)") !=
                std::string::npos &&
            shellViewProps.find("$(UserProfile)\\.nuget\\packages") !=
                std::string::npos &&
            shellViewProps.find("SnowDesktopCppWinRTExecutable") !=
                std::string::npos &&
            shellViewProps.find("$(CppWinRTPackageDir)") ==
                std::string::npos,
        "clean builds resolve the pinned ShellView C++/WinRT generator from explicit and default NuGet roots");
}

void TestBuildManifest(const std::string& props,
    const std::string& writer)
{
    Check(props.find("$(NuGetPackageRoot)") != std::string::npos &&
            props.find("$(RestorePackagesPath)") != std::string::npos &&
            props.find("$(NUGET_PACKAGES)") != std::string::npos &&
            props.find("$(UserProfile)\\.nuget\\packages") !=
                std::string::npos &&
            props.find("$(SnowDesktopNuGetPackageRoot)\\microsoft.windowsappsdk") !=
                std::string::npos &&
            props.find("$(SnowDesktopNuGetPackageRoot)\\microsoft.windows.cppwinrt") !=
                std::string::npos,
        "deployment licenses resolve from explicit and default NuGet roots");
    for (const char* item : {
             "@(MicrosoftWindowsAppSDKFiles)",
             "@(MicrosoftWindowsAppSDKFilesRes)",
             "@(MicrosoftWindowsAppSDKAppxFragmentFiles)",
             "App.xbf", "SettingsShell.xbf", "$(TargetName).pri",
             "SnowDesktop.winmd", "FluentSystemIcons-Regular.ttf",
             "fa-solid-900.ttf", "assets\\icon\\icon_small.png",
             "Assets\\App\\SnowDesktop.png",
             "Microsoft.Windows.AI.MachineLearning.dll",
             "onnxruntime.dll", "DirectML.dll",
             "Microsoft.Web.WebView2.Core.dll",
             "Microsoft.Web.WebView2.Core.winmd"})
    {
        Check(props.find(item) != std::string::npos,
            "MSBuild deployment target captures every required item class");
    }
    Check(props.find("AfterTargets=\"Build\"") != std::string::npos &&
            props.find("write_deployment_manifest.ps1") !=
                std::string::npos,
        "MSBuild generates the deployment manifest after the application build");
    Check(props.find("$(TargetDir)Assets\\App") != std::string::npos &&
            props.find("$(TargetDir)Assets\\Fonts") != std::string::npos &&
            props.find("assets\\settings\\icons\\*.svg") !=
                std::string::npos &&
            props.find(
              "@(_SnowDesktopSettingsIcon-&gt;'Assets\\Settings\\Icons\\%(Filename)%(Extension)") !=
                std::string::npos &&
            props.find("<DeploymentKind>Asset</DeploymentKind>") !=
                std::string::npos &&
            writer.find(
                "\"NuGet\", \"WindowsAppSDK\", \"Generated\", \"Asset\"") !=
                std::string::npos,
        "the WinUI title-bar icon, icon fonts, and colored settings icons are copied and listed as explicit deployment assets");
    Check(props.find("-TargetDirectory &quot;$(TargetDir).&quot;") !=
                std::string::npos &&
            props.find("-TargetDirectory &quot;$(TargetDir)&quot;") ==
                std::string::npos,
        "MSBuild command quoting cannot consume arguments after a trailing backslash");
    Check(writer.find("sha256") != std::string::npos &&
            writer.find("Sort-Object path") != std::string::npos &&
            writer.find("No official Windows App SDK package.appxfragment") !=
                std::string::npos,
        "deployment manifest is deterministic, hashed, and requires official fragments");
    Check(writer.find("WindowsAppSDK-LICENSE.txt") != std::string::npos &&
            writer.find("WindowsAppSDK-NOTICE.txt") != std::string::npos &&
            writer.find("CppWinRT-LICENSE.txt") != std::string::npos &&
            writer.find("WindowsML-LICENSE.txt") != std::string::npos &&
            writer.find("WindowsML-NOTICE.txt") != std::string::npos &&
            writer.find("WebView2-LICENSE.txt") != std::string::npos &&
            writer.find("WebView2-NOTICE.txt") != std::string::npos,
        "deployment manifest carries the pinned NuGet license and notice files, including Windows ML and WebView2");
    Check(writer.find("Get-ChildItem -LiteralPath $TargetDirectory") ==
            std::string::npos,
        "deployment manifest generation does not glob the build output");
}

void TestPackagers(const std::string& module,
    const std::string& release,
    const std::string& steam)
{
    Check(module.find("Resolve-SnowDesktopDeploymentPath") !=
            std::string::npos &&
            module.find("IsPathRooted") != std::string::npos &&
            module.find("buildPath") != std::string::npos &&
            module.find("hash mismatch") != std::string::npos &&
            module.find("WindowsML-LICENSE.txt") != std::string::npos &&
            module.find("WindowsML-NOTICE.txt") != std::string::npos &&
            module.find("WebView2-LICENSE.txt") != std::string::npos &&
            module.find("WebView2-NOTICE.txt") != std::string::npos,
        "shared payload reader rejects unsafe or stale entries and requires dependency notices");
    Check(release.find("Copy-SnowDesktopDeploymentPayload") !=
            std::string::npos &&
            steam.find("Copy-SnowDesktopDeploymentPayload") !=
                std::string::npos,
        "portable, MSIX, and Steam payloads use the same deployment manifest");
    Check(release.find("Get-SnowDesktopPackagingContext") != std::string::npos &&
            steam.find("Get-SnowDesktopPackagingContext") != std::string::npos &&
            release.find("buildProvenance") != std::string::npos && steam.find("buildProvenance") != std::string::npos &&
            release.find("Join-Path $buildOutput \"widgets\"") != std::string::npos &&
            steam.find("Join-Path $buildOutput \"widgets\"") != std::string::npos,
        "both packagers bind saved build provenance and built widget inputs");
    Check(module.find("Copy-SnowDesktopRepositoryLicenses") !=
                std::string::npos &&
            module.find("Everything-SDK-LICENSE.txt") !=
                std::string::npos &&
            module.find("FontAwesome-LICENSE.txt") !=
                std::string::npos &&
            module.find("DearImGui-LICENSE.txt") !=
                std::string::npos &&
            module.find("Lua-LICENSE.txt") !=
                std::string::npos &&
            module.find("PinyinData-LICENSE.txt") !=
                std::string::npos &&
            release.find("Copy-SnowDesktopRepositoryLicenses") !=
                std::string::npos &&
            steam.find("Copy-SnowDesktopRepositoryLicenses") !=
                std::string::npos,
        "portable, MSIX, and Steam payloads include bundled third-party license texts");
    Check(module.find("Enable-SnowDesktopPrivateRuntimeAssembly") !=
            std::string::npos &&
            module.find("Test-SnowDesktopExecutableRootResource") !=
                std::string::npos &&
            module.find("Microsoft.UI.Xaml/Assets/") !=
                std::string::npos &&
            module.find("$rootTarget") != std::string::npos &&
            module.find("SnowDesktop.Runtime") != std::string::npos &&
            release.find("-RuntimeDirectory $runtimeDirectory") !=
                std::string::npos &&
            steam.find("-RuntimeDirectory $runtimeDirectory") !=
                std::string::npos,
        "release payloads isolate third-party runtime files in one private assembly directory");
    Check(release.find("SnowDesktopWorkshopManager.exe") ==
            std::string::npos &&
            release.find("SnowDesktopSteamBridge.exe") ==
                std::string::npos &&
            release.find("steam_api64.dll") == std::string::npos &&
            release.find("widgets\\snowdesktop-lua-widget\\bin\\snowwidget.exe") !=
                std::string::npos &&
            steam.find("SnowDesktopWorkshopManager.exe") !=
                std::string::npos &&
            steam.find("SnowDesktopSteamBridge.exe") !=
                std::string::npos &&
            steam.find("$bundledSkillPublisherFiles") !=
                std::string::npos &&
            steam.find("$payloadSkillPublisher") !=
                std::string::npos,
        "only the Steam payload includes the Steam bridge, Steam API, Workshop manager, and complete publishing CLI");
    Check(release.find("AllowOwnershipBridge") == std::string::npos &&
            release.find("Portable and MSIX payloads must not contain Steam bridge files") !=
                std::string::npos,
        "portable and MSIX packaging reject leaked Steam files without a bridge opt-in");
    Check(release.find("SnowDesktopLauncher.exe") == std::string::npos &&
            release.find("SnowDesktop.runtime-context.json") ==
                std::string::npos &&
            release.find("SnowDesktop.steam.json") == std::string::npos &&
            release.find(".snowdesktop/runtime") == std::string::npos &&
            release.find("Join-Path $payload \"distribution\"") ==
                std::string::npos,
        "traditional portable and MSIX packaging contain no Steam launcher, context, manifest, distribution, or managed runtime state");
    Check(steam.find("schemaVersion = 3") != std::string::npos &&
            steam.find("fileMetadata = $payloadFileMetadata") !=
                std::string::npos &&
            steam.find("sha256 = Get-Sha256") != std::string::npos,
        "Steam packages retain compatible file paths plus size and SHA-256 metadata");
    Check(steam.find("SnowDesktopLauncher.exe") != std::string::npos &&
            steam.find("$distribution = Join-Path $payload \"distribution\"") !=
                std::string::npos &&
            steam.find("SnowDesktop.steam.json") != std::string::npos &&
            steam.find("kind = \"steam-managed\"") !=
                std::string::npos &&
            steam.find("runtimeDirectory = \".snowdesktop/runtime\"") !=
                std::string::npos &&
            steam.find("dataDirectory = \"data\"") !=
                std::string::npos,
        "Steam packages separate the tracked distribution from the stable launcher, runtime copies, and data");
    Check(release.find("$runtimeDestination") != std::string::npos &&
            release.find("SnowDesktopWallpaperInjector32.exe") !=
                std::string::npos &&
            steam.find("$runtimeFiles = @(") != std::string::npos &&
            steam.find(
                "\"distribution/$runtimeDirectory/steam_api64.dll\"") !=
                std::string::npos,
        "first-party runtime helpers and the Steam redistributable are routed into the runtime directory");
    Check(module.find("AdditionalRuntimeDlls") != std::string::npos &&
            module.find("AdditionalExecutables") != std::string::npos &&
            module.find("existingPrivateManifest") != std::string::npos &&
            release.find("AdditionalRuntimeDlls") ==
                std::string::npos &&
            release.find("AdditionalExecutables") ==
                std::string::npos &&
            steam.find("-AdditionalRuntimeDlls @(") !=
                std::string::npos &&
            steam.find("$packagedConfigurationText") !=
                std::string::npos,
        "Steam tools declare and execute against the private Steamworks runtime");
    Check(release.find("Merge-SnowDesktopAppxFragments") !=
            std::string::npos &&
            module.find("windows.activatableClass.") != std::string::npos &&
            module.find("$pathNode.InnerText = $relativePath") !=
                std::string::npos,
        "MSIX merges official activatable-class package fragments with private runtime paths");
    Check(release.find("New-SnowDesktopPackageResourceIndex") !=
                std::string::npos &&
            release.find(
                "$relativeInputs = @(\"SnowDesktop.pri\", \"Microsoft.UI.Xaml\")") !=
                std::string::npos &&
            release.find("Move-Item -LiteralPath $source") !=
                std::string::npos &&
            release.find("Microsoft.UI.Xaml\",") != std::string::npos &&
            release.find("Microsoft.WindowsAppRuntime\"") !=
                std::string::npos &&
            release.find("Disable-InputPriMerging") == std::string::npos,
        "MSIX temporarily hides duplicate app inputs while merging and validating Windows App SDK component PRI maps");
    Check(module.find("Get-ChildItem") == std::string::npos &&
            release.find("$buildOutput + \"*\"") == std::string::npos &&
            steam.find("$buildOutput + \"*\"") == std::string::npos,
        "shared deployment copying never globs the build directory");
}

void TestBuildOutputLayout(const std::string& cmake,
    const std::string& arranger)
{
    Check(cmake.find(
              "${CMAKE_BINARY_DIR}/$<CONFIG>/tests") !=
                std::string::npos &&
            cmake.find("COMPILE_PDB_OUTPUT_DIRECTORY") !=
                std::string::npos &&
            cmake.find("scripts/arrange_build_output.ps1") !=
                std::string::npos &&
            cmake.find("-AllowMissingFirstPartyRuntime") !=
                std::string::npos &&
            cmake.find("SnowDesktopFastTests") !=
                std::string::npos,
        "CTest executables and symbols use the dedicated configuration tests directory");
    Check(arranger.find("SnowDesktop.Runtime") != std::string::npos &&
            arranger.find("Enable-SnowDesktopPrivateRuntimeAssembly") !=
                std::string::npos &&
            arranger.find("Test-SnowDesktopExecutableRootResource") !=
                std::string::npos &&
            arranger.find("Microsoft.WindowsAppRuntime.Bootstrap.dll") !=
                std::string::npos &&
            arranger.find("AllowMissingFirstPartyRuntime") !=
                std::string::npos &&
            arranger.find("buildPath") != std::string::npos &&
            arranger.find("empty runtime payload directories") !=
                std::string::npos &&
            arranger.find("Build output still contains root-level DLLs") !=
                std::string::npos,
        "standard builds become directly runnable private-runtime layouts without root DLLs");
    // Selection, cold target resolution and output-isolation failures execute
    // the actual runner functions in test_selection_tests.ps1. Source spelling
    // here cannot establish those behaviors and must not duplicate that suite.
}

void TestReleaseManagerShellReload(const std::string& manager,
    const std::string& documentation)
{
    Check(manager.find("Invoke-SnowDesktopPackagingBuild $repositoryRoot") != std::string::npos &&
            manager.find("Get-SnowDesktopPackagingContext $repositoryRoot $ArtifactRun") != std::string::npos &&
            manager.find("@('-ArtifactRun', $ArtifactRun)") != std::string::npos,
        "release manager packages the producing saved Run instead of rebuilding active output");
    Check(manager.find("Get-BuildOccupancy") != std::string::npos &&
            manager.find("SnowDesktopTaskbarHook.dll") !=
                std::string::npos &&
            manager.find("$buildHook") != std::string::npos &&
            manager.find("-ReloadShellBeforeBuild:$ReloadShell") !=
                std::string::npos,
        "release CLI and TUI only treat the build-directory hook as build occupancy");
    // Prevent retired binary-repository writes from returning to package/publish.
    Check(manager.find("$releaseRepository") == std::string::npos &&
            manager.find("$binaryRemote") == std::string::npos &&
            manager.find("Sync-ReleaseRepository") == std::string::npos &&
            manager.find("SnowDesktop_Release") == std::string::npos &&
            manager.find("sync-release") == std::string::npos,
        "release automation cannot inspect, synchronize, or publish the retired repository");
    Check(documentation.find(
              "scripts\\release.bat package -ReloadShell") !=
            std::string::npos &&
            documentation.find(
              "scripts\\release.bat prepare -ReloadShell") !=
                std::string::npos,
        "release documentation describes shell reload for package and prepare");
}

void TestGitHubReleasePublication(const std::filesystem::path& root)
{
    // Exercise the production publication plan so local Store packages and
    // their checksums cannot leak into the public GitHub attachment set.
    try { build_test::publication_tests(root); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        Check(false, "GitHub publication uses a version-only title and portable-only assets");
    }
}

void TestDockHookPackaging(const std::filesystem::path& repository)
{
    // Execute the real packagers' required-file and copy loops. SDK assembly,
    // signing and compression are outside this isolated runtime-file boundary.
    // The independent expected files prevent a valid build with omitted hooks
    // from silently producing a package that always uses system animations.
    using namespace build_test;
    const auto fixture = temporary("dock-hook-packaging");
    const auto driver = fixture / L"copy-runtime.ps1";
    write(driver, R"PS(param($Packager,$FixtureRoot,$Missing)
$ErrorActionPreference = 'Stop'
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Packager,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count) { throw 'Packager syntax error' }
$buildOutput = Join-Path $FixtureRoot 'build'
$runtimeDirectory = 'SnowDesktop.Runtime'
$distribution = Join-Path $FixtureRoot 'output'
$Destination = $distribution
$runtimeRoot = Join-Path $distribution $runtimeDirectory
$null = New-Item -ItemType Directory -Path $runtimeRoot -Force
function Invoke-Statement($Statement) { . ([scriptblock]::Create($Statement.Extent.Text)) }
if ([IO.Path]::GetFileName($Packager) -eq 'package_steam.ps1') {
    foreach ($variable in @('$runtimeFiles','$required')) {
        $assignment = @($ast.EndBlock.Statements | Where-Object {
            $_ -is [System.Management.Automation.Language.AssignmentStatementAst] -and $_.Left.Extent.Text -ceq $variable
        })
        if ($assignment.Count -ne 1) { throw 'Runtime inventory assignment not found' }
        . ([scriptblock]::Create($assignment[0].Extent.Text))
    }
    foreach ($name in $required) {
        $source = if ($runtimeFiles -contains $name) { Join-Path (Join-Path $buildOutput $runtimeDirectory) $name } else { Join-Path $buildOutput $name }
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($source)) -Force
        [IO.File]::WriteAllText($source, 'fixture:' + $name)
    }
    $loops = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [System.Management.Automation.Language.ForEachStatementAst] -and $_.Condition.Extent.Text -ceq '$required'
    })
    if ($loops.Count -ne 2) { throw 'Required-file validation and copy loops not found' }
} else {
    $copyPayload = $ast.Find({ param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Copy-Payload'
    }, $true)
    $loops = @($copyPayload.Body.EndBlock.Statements | Where-Object {
        $_ -is [System.Management.Automation.Language.ForEachStatementAst] -and $_.Variable.VariablePath.UserPath -eq 'name'
    })
    if ($loops.Count -ne 1) { throw 'Release runtime copy loop not found' }
    $runtimeDestination = $runtimeRoot
    foreach ($name in (. ([scriptblock]::Create($loops[0].Condition.Extent.Text)))) {
        $source = Join-Path (Join-Path $buildOutput $runtimeDirectory) $name
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($source)) -Force
        [IO.File]::WriteAllText($source, 'fixture:' + $name)
    }
}
if ($Missing) {
    $missingPath = Join-Path (Join-Path $buildOutput $runtimeDirectory) $Missing
    if (Test-Path -LiteralPath $missingPath) { Remove-Item -LiteralPath $missingPath }
}
foreach ($loop in $loops) { Invoke-Statement $loop }
)PS", true);
    Bridge bridge(repository, fixture);
    for (const auto* packager : {"package_steam.ps1", "package_release.ps1"})
    {
        const auto script = repository / "scripts" / packager;
        const auto output = fixture / packager;
        auto result = bridge.invoke(driver, {utf8(script.wstring()), utf8(output.wstring())});
        require(result.code == 0, result.out + result.err);
        for (const auto* hook : {"SnowDesktopHookEntry.dll", "SnowDesktopHookEntry32.dll", "SnowDesktopDockMinimizeHook.dll",
                 "SnowDesktopDockMinimizeHook32.dll", "SnowDesktopDockMinimizeHost32.exe"})
        {
            const auto packaged = output / "output" / "SnowDesktop.Runtime" / hook;
            require(fs::is_regular_file(packaged) && read(packaged) == "fixture:" + std::string(hook),
                std::string(packager) + " omitted or changed " + hook);
            require(!fs::exists(output / "output" / hook), "Dock hook escaped the private runtime");
            const auto missing = fixture / (std::string(packager) + "-missing-" + hook);
            result = bridge.invoke(driver, {utf8(script.wstring()), utf8(missing.wstring()), hook});
            require(result.code != 0 && result.err.find(hook) != std::string::npos,
                std::string(packager) + " accepted a build without " + hook);
        }
    }
}

void TestSteamPackagingPaths(const std::filesystem::path& repository)
{
    // Run the same path resolver used by package_steam.ps1. Native assertions
    // define the output boundary; the PowerShell adapter only serializes it.
    using namespace build_test;
    const auto fixture = temporary("steam-packaging-paths");
    const auto original = fixture / L"repository";
    const auto source = original / L".build/candidate-slots/fixture/source";
    const auto runtime = original / L".build/candidate-artifacts/fixture/runtime";
    for (const auto& directory : {original, source})
    {
        write(directory / L"version.json", R"({"version":"1.2.3.0"})");
        write(directory / L"packaging/steam-identity.json", R"({"appId":5080330,"windowsDepotId":5080331})");
    }
    fs::create_directories(runtime);
    const auto adapter = fixture / L"resolve-steam-paths.ps1";
    write(adapter, R"PS(param($Module,$Source,$Build,$Artifacts,$Output,[switch]$Skip,[switch]$Reload)
$ErrorActionPreference='Stop'
Import-Module $Module -Force
Resolve-SnowDesktopSteamPackagingPaths -SourceRepositoryRoot $Source -Version '1.2.3.0' `
    -BuildOutputDirectory $Build -ArtifactRepositoryRoot $Artifacts -OutputDirectory $Output `
    -SkipBuild:$Skip -ReloadShell:$Reload | ConvertTo-Json -Compress
)PS", true);
    Bridge bridge(repository, fixture);
    const auto module = repository / L"scripts/steam_packaging_paths.psm1";
    auto arguments = [&](const fs::path& src, const fs::path& input,
                         const fs::path& artifacts, const fs::path& output) {
        return std::vector<std::string>{"-Module", utf8(module.wstring()),
            "-Source", utf8(src.wstring()), "-Build", utf8(input.wstring()),
            "-Artifacts", utf8(artifacts.wstring()), "-Output", utf8(output.wstring())};
    };
    auto defaults = arguments(original, {}, {}, {});
    const auto normal = bridge.call(adapter, defaults);
    require(fs::path(wide(normal.at("buildOutput").str())) == original / L".build/Release" &&
            fs::path(wide(normal.at("outputDirectory").str())) == original / L"artifacts/v1.2.3.0/steam" &&
            fs::path(wide(normal.at("zip").str())) == original / L"artifacts/v1.2.3.0/SnowDesktop-Steam-x64-1.2.3.0.zip",
        "ordinary Steam packaging changed its build, payload or ZIP location");
    auto frozen = arguments(source, runtime, original,
        original / L"artifacts/v1.2.3.0/steam-frozen");
    frozen.push_back("-Skip");
    const auto resolved = bridge.call(adapter, frozen);
    require(fs::path(wide(resolved.at("sourceRoot").str())) == source &&
            fs::path(wide(resolved.at("buildOutput").str())) == runtime &&
            fs::path(wide(resolved.at("outputDirectory").str())) == original / L"artifacts/v1.2.3.0/steam-frozen" &&
            fs::path(wide(resolved.at("zip").str())) == original / L"artifacts/v1.2.3.0/SnowDesktop-Steam-x64-1.2.3.0.zip" &&
            !fs::exists(source / L"artifacts"),
        "frozen Steam input placed its payload or ZIP inside captured source");
    auto reject = [&](std::vector<std::string> args, const std::string& expected) {
        const auto result = bridge.invoke(adapter, args);
        require(result.code != 0 && result.err.find(expected) != std::string::npos &&
                !fs::exists(source / L"artifacts"),
            "Steam packaging accepted an unsafe path or failed for the wrong reason: " + result.out + result.err);
    };
    auto noRoot = arguments(source, runtime, {}, {}); noRoot.push_back("-Skip");
    reject(noRoot, "-ArtifactRepositoryRoot");
    auto self = arguments(source, runtime, source, {}); self.push_back("-Skip");
    reject(self, "frozen candidate source tree");
    auto escape = arguments(source, runtime, original, original / L"artifacts/outside-version"); escape.push_back("-Skip");
    reject(escape, "artifacts\\v1.2.3.0");
    auto overlap = arguments(source, original / L"artifacts/v1.2.3.0", original, {}); overlap.push_back("-Skip");
    reject(overlap, "input and output directories must not overlap");
    reject(arguments(source, runtime, original, {}), "requires -SkipBuild");
    auto reload = frozen; reload.push_back("-Reload");
    reject(reload, "cannot be combined with -SkipBuild");
    const auto outside = fixture / L"outside";
    fs::create_directories(outside);
    const auto linked = original / L"artifacts/v1.2.3.0/linked";
    fs::create_directories(linked.parent_path());
    const auto junction = run({powershell().wstring(), L"-NoProfile", L"-Command",
        wide("New-Item -ItemType Junction -Path " + ps_literal(linked) +
            " -Target " + ps_literal(outside) + " | Out-Null")}, fixture, 10);
    require(junction.code == 0, "Steam path fixture could not create its output junction: " + junction.err);
    auto linkedOutput = arguments(source, runtime, original, linked); linkedOutput.push_back("-Skip");
    reject(linkedOutput, "cannot traverse a reparse point");
    // Verify the public packager itself rejects the original failing call
    // before it can read runtime EXEs or create source/artifacts.
    copy(repository, source, {"scripts/package_steam.ps1", "scripts/steam_packaging_paths.psm1",
        "scripts/deployment_payload.psm1"});
    const auto denied = bridge.invoke(source / L"scripts/package_steam.ps1", {"-SkipBuild"});
    require(denied.code != 0 && denied.err.find("-ArtifactRepositoryRoot") != std::string::npos &&
            !fs::exists(source / L"artifacts"),
        "real Steam packager did not reject implicit output inside a frozen source tree");
}

void TestSteamPipeAutomation(const std::string& steamPipe,
    const std::string& steamPipeConfiguration,
    const std::string& manager,
    const std::string& scriptDocumentation,
    const std::string& packagingDocumentation)
{
    Check(steamPipe.find(
              "ValidateSet(\"Preview\", \"UploadDev\", \"UploadPublic\")") !=
                std::string::npos &&
            steamPipe.find("\"Preview\" \"1\"") != std::string::npos &&
            steamPipe.find("SetLive") != std::string::npos &&
            steamPipe.find("Assert-PublicReleaseBranch") !=
                std::string::npos,
        "SteamPipe exposes separate preview, private-development, and public upload modes");
    Check(steamPipe.find("SNOWDESKTOP_STEAMCMD_PATH") !=
                std::string::npos &&
            steamPipe.find("SNOWDESKTOP_STEAM_BUILD_ACCOUNT") !=
                std::string::npos &&
            steamPipe.find("+@NoPromptForPassword") !=
                std::string::npos &&
            steamPipe.find("+run_app_build") != std::string::npos &&
            steamPipe.find("[string]$Password") == std::string::npos &&
            steamPipe.find("-betapassword") == std::string::npos,
        "SteamPipe uses a pre-authenticated build account without accepting password arguments");
    Check(steamPipe.find("Tee-Object -FilePath $logPath -Append") !=
                std::string::npos &&
            steamPipe.find("Tee-Object -LiteralPath $logPath -Append") ==
                std::string::npos,
        "SteamPipe appends SteamCMD output with a valid Tee-Object parameter set");
    Check(steamPipe.find("-not $Yes") != std::string::npos &&
            steamPipe.find("$ConfirmVersion -ne $version") !=
                std::string::npos &&
            steamPipe.find("$ConfirmPrivateBranch -cne $privateBranch") !=
                std::string::npos &&
            steamPipe.find("$ConfirmPublicBranch -cne $publicBranch") !=
                std::string::npos &&
            steamPipe.find("Assert-SteamPayloadManifest") !=
                std::string::npos &&
            steamPipe.find("Compare-Object -ReferenceObject $listed") !=
                std::string::npos &&
            steamPipe.find("Steam payload metadata mismatch") !=
                std::string::npos,
        "Steam development and public uploads require explicit version and branch confirmation over a current payload manifest");
    Check(steamPipe.find("FileExclusion\" \"steam_appid.txt") !=
                std::string::npos &&
            steamPipe.find("FileExclusion\" \"data\\*") !=
                std::string::npos &&
            steamPipe.find("FileExclusion\" \"runtime\\*") !=
                std::string::npos &&
            steamPipe.find("artifacts\\v$version") !=
                std::string::npos &&
            steamPipe.find("\"steampipe\"") != std::string::npos,
        "SteamPipe keeps generated state out of the depot and stores upload state beside version artifacts");
    Check(manager.find("\"steam-preview\"") != std::string::npos &&
            manager.find("\"steam-upload-dev\"") != std::string::npos &&
            manager.find("\"steam-upload-public\"") !=
                std::string::npos &&
            manager.find("Invoke-SteamPipeAction") != std::string::npos &&
            manager.find("ConfirmPrivateBranch") != std::string::npos &&
            manager.find("ConfirmPublicBranch") != std::string::npos,
        "the shared release manager exposes preview and separately confirmed private and public upload commands");
    Check(steamPipeConfiguration.find("\"privateDevelopmentBranch\"") !=
                std::string::npos &&
            steamPipeConfiguration.find("internal-dev") !=
                std::string::npos &&
            steamPipeConfiguration.find("\"publicReleaseBranch\"") !=
                std::string::npos &&
            steamPipeConfiguration.find("\"public\"") !=
                std::string::npos &&
            steamPipeConfiguration.find("password") == std::string::npos &&
            steamPipeConfiguration.find("account") == std::string::npos &&
            steamPipeConfiguration.find("token") == std::string::npos,
        "committed SteamPipe configuration contains explicit private and public branches but no credentials");
    Check(scriptDocumentation.find("steam-preview") != std::string::npos &&
            scriptDocumentation.find("steam-upload-dev") !=
                std::string::npos &&
            scriptDocumentation.find("steam-upload-public") !=
                std::string::npos &&
            packagingDocumentation.find("@NoPromptForPassword") !=
                std::string::npos &&
            packagingDocumentation.find("Preview \"1\"") !=
                std::string::npos &&
            packagingDocumentation.find("不会上传内容") !=
                std::string::npos,
        "SteamPipe documentation distinguishes preview safety from private branch upload");
}

void TestRuntimeResolution(const std::string& deploymentHeader,
    const std::string& deploymentSource,
    const std::string& mainSource,
    const std::string& wallpaperCapture,
    const std::string&,
    const std::string&)
{
    Check(deploymentHeader.find("GetRuntimeFilePath") !=
            std::string::npos &&
            deploymentSource.find("SnowDesktop.Runtime") !=
                std::string::npos,
        "application runtime lookup prefers the packaged runtime directory");
    Check(deploymentHeader.find("GetInjectableRuntimeFilePath") !=
            std::string::npos &&
            deploymentSource.find("DeployInjectableRuntimeCopy") !=
            std::string::npos &&
            deploymentSource.find("GetDataDirectoryPath") !=
                std::string::npos &&
            deploymentSource.find("ShellHook") != std::string::npos &&
            deploymentSource.find(".owner.lock") != std::string::npos &&
            deploymentSource.find("RuntimeDirectoryHasLiveOwner") !=
                std::string::npos &&
            deploymentSource.find("CleanupLegacyRuntimeRootsOnce") !=
                std::string::npos &&
            deploymentSource.find("CleanupLegacyLocalAppDataRoot") !=
                std::string::npos &&
            deploymentSource.find("MigrateLegacyCreatorProjects") !=
                std::string::npos &&
            deploymentSource.find("legacy-runtime-roots-v2.done") !=
                std::string::npos &&
            deploymentSource.find("~InjectableRuntimeDirectory") !=
                std::string::npos,
        "cross-process hooks use a leased data directory with legacy, startup, and exit cleanup");
    Check(deploymentSource.find("cache.Deploy(source, excludeImplementation)") != std::string::npos &&
            deploymentSource.find("GetStableHookEntryRoot()") != std::string::npos &&
            deploymentSource.find("hook_entry::Prepare") != std::string::npos &&
            deploymentSource.find("shared.empty() ? source.wstring()") == std::string::npos &&
            deploymentSource.find("DeployInjectableRuntimeCopy(kTaskbarHookFilename, previous)") != std::string::npos,
        "ordinary and isolated hook deployment share the bounded pool without unbounded fallback paths");
    Check(wallpaperCapture.find("GetInjectableRuntimeFilePath") !=
            std::string::npos &&
            wallpaperCapture.find("GetRuntimeFilePath") !=
                std::string::npos &&
            wallpaperCapture.find("injector.parent_path().c_str()") !=
                std::string::npos,
        "wallpaper hooks use injectable copies while the 32-bit injector resolves from the runtime directory");
    Check(deploymentSource.find("try_get_activation_factory") !=
            std::string::npos &&
            deploymentSource.find("GetStartupTaskOnCurrentApartment") !=
                std::string::npos &&
            deploymentSource.find("StartupTask::GetAsync(") ==
                std::string::npos,
        "packaged StartupTask calls avoid caching a proxy beyond the short-lived MTA apartment");
    const std::size_t startupQueryHandler = mainSource.find(
        "TryHandlePackagedAutoStartQueryCommand()");
    const std::size_t previewHost = mainSource.find(
        "TryRunWidgetAuthorPreviewHostCommand(");
    const std::size_t singleInstance = mainSource.find(
        "snowdesktop::single_instance::Guard singleInstance;");
    Check(deploymentSource.find(
              "RunInstalledPackagedAutoStartOperationThroughActivation(") !=
                std::string::npos &&
            deploymentSource.find("ActivateApplication(") !=
                std::string::npos &&
            deploymentSource.find(
              "SetInstalledPackagedAutoStartEnabled(") !=
                std::string::npos &&
            deploymentSource.find("SystemAppData") == std::string::npos &&
            deploymentSource.find("UserEnabledStartupOnce") ==
                std::string::npos &&
            startupQueryHandler != std::string::npos &&
            previewHost != std::string::npos &&
            singleInstance != std::string::npos &&
            startupQueryHandler < previewHost &&
            previewHost < singleInstance,
        "portable builds query the installed StartupTask through the packaged public API before normal app startup");
    // Hook ownership and occupied-output preflight are executed by the isolated
    // build_workflow tests. Source placement does not prove those behaviors.
}

void TestAutoStartTransitionManifest(const std::string& manifest)
{
    Check(manifest.find(
              "Category=\"windows.appExecutionAlias\"") !=
                std::string::npos &&
            manifest.find(
              "Alias=\"SnowDesktopStore.exe\"") !=
                std::string::npos,
        "the installed deployment exposes a stable execution alias for the unified logon task");
    Check(manifest.find("Category=\"windows.startupTask\"") !=
                std::string::npos &&
            manifest.find("1.0.4.0 transition") !=
                std::string::npos,
        "the transition package retains the legacy StartupTask long enough to migrate its user state");
}

void TestOwnedRuntimeStaging(const std::string& dropExtraction,
    const std::string& widgetPackages, const std::string& snowwidget,
    const std::string& packageTool, const std::string& workshopCore,
    const std::string& workshopManager, const std::string& workshopSource,
    const std::string& workshopProjects)
{
    Check(dropExtraction.find("GetDataSubdirectoryPath(L\"DropContent\")") == std::string::npos,
        "transient non-file drop payloads must never be persisted below the user data root");
    Check(widgetPackages.find("temp_directory_path") == std::string::npos &&
            widgetPackages.find("CreateStagingPath(\"validation\")") !=
                std::string::npos,
        "component archive validation uses the configured data staging root");
    Check(snowwidget.find("GetTempPathW") == std::string::npos &&
            snowwidget.find("GetTempFileNameW") == std::string::npos &&
            snowwidget.find("L\"data\"") != std::string::npos &&
            snowwidget.find("L\"snowwidget\"") != std::string::npos &&
            snowwidget.find("preview-results") != std::string::npos &&
            snowwidget.find("publish-staging") != std::string::npos,
        "snowwidget scratch files stay under its software data directory");
    Check(packageTool.find("temp_directory_path") == std::string::npos &&
            packageTool.find("stagingRoot_") != std::string::npos &&
            packageTool.find("SteamWorkshopManager") != std::string::npos,
        "Workshop packaging uses a manager data staging directory");
    Check(workshopCore.find("temp_directory_path") == std::string::npos &&
            workshopCore.find("stagingRoot_") != std::string::npos &&
            workshopCore.find("SteamWorkshop") != std::string::npos,
        "Steam uploads use a software data staging directory");
    Check(workshopManager.find("managerRoot_ / L\"staging\"") !=
                std::string::npos &&
            workshopProjects.find(
                ".legacy-localappdata-migrated-v1") != std::string::npos &&
            workshopProjects.find("MigrateWorkshopManagerDataOnce") !=
                std::string::npos,
        "the Workshop Manager shares the main data root and scans its former store only once");
    Check(workshopSource.find("PackagePaths{}") == std::string::npos &&
            workshopSource.find("PackagePaths::ForCurrentDeployment()") !=
                std::string::npos,
        "Workshop subscription validation uses the deployment data staging directory");
}
}

int main(int argc, char** argv)
{
    Check(argc == 2, "repository root is provided");
    if (argc == 2)
    {
        const std::filesystem::path root(argv[1]);
        try { TestExecutableVersionResources(root); }
        catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            Check(false, "EXE version metadata generation and future-target enforcement");
        }
        TestPinnedToolchain(
            ReadText(root / "CMakeLists.txt"),
            ReadText(root / "cmake/SnowDesktop.ShellView.props"));
        TestBuildManifest(
            ReadText(root / "cmake/SnowDesktop.WinUI.props"),
            ReadText(root / "scripts/write_deployment_manifest.ps1"));
        TestPackagers(
            ReadText(root / "scripts/deployment_payload.psm1"),
            ReadText(root / "scripts/package_release.ps1"),
            ReadText(root / "scripts/package_steam.ps1"));
        TestBuildOutputLayout(
            ReadText(root / "CMakeLists.txt"),
            ReadText(root / "scripts/arrange_build_output.ps1"));
        TestReleaseManagerShellReload(
            ReadText(root / "scripts/release_manager.ps1"),
            ReadText(root / "packaging/README.md"));
        TestGitHubReleasePublication(root);
        try { TestDockHookPackaging(root); }
        catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            Check(false, "Dock animation hooks are copied and required by every packager");
        }
        try { TestSteamPackagingPaths(root); }
        catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            Check(false, "Steam packaging separates frozen inputs from versioned artifact output");
        }
        TestSteamPipeAutomation(
            ReadText(root / "scripts/steam_pipe.ps1"),
            ReadText(root / "packaging/steam-pipe.json"),
            ReadText(root / "scripts/release_manager.ps1"),
            ReadText(root / "scripts/README.md"),
            ReadText(root / "packaging/README.md"));
        TestRuntimeResolution(
            ReadText(root / "src/platform/deployment_context.h"),
            ReadText(root / "src/platform/deployment_context.cpp"),
            ReadText(root / "src/main.cpp"),
            ReadText(root / "src/app/features/wallpaper_engine_capture.cpp"),
            ReadText(root / "scripts/build.bat"),
            ReadText(root / "scripts/build_debug.bat"));
        TestAutoStartTransitionManifest(
            ReadText(root / "packaging/AppxManifest.xml.in"));
        TestOwnedRuntimeStaging(
            ReadText(root / "src/app/drag_drop/app_drop_data_extraction.cpp"),
            ReadText(root / "src/widget/packages/widget_package.cpp"),
            ReadText(root / "tools/snowwidget/main.cpp"),
            ReadText(root / "steam_bridge/src/package_tool.cpp"),
            ReadText(root / "steam_bridge/src/steam_workshop_core.cpp"),
            ReadText(root / "steam_bridge/src/manager_main.cpp"),
            ReadText(root / "src/steam/steam_workshop_source.cpp"),
            ReadText(root / "steam_bridge/src/workshop_project.cpp"));
    }

    if (failures != 0)
    {
        std::cerr << failures << " deployment packaging contract check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Deployment packaging contract checks passed\n";
    return EXIT_SUCCESS;
}
