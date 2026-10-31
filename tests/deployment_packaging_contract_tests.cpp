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

// Reads generated data such as .rc output, not production source contracts.
std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    std::string text = output.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

void TestPerformanceDeployment(const std::filesystem::path& root)
{
    const auto fixture = build_test::temporary("perfetto-deployment-contract");
    const auto script = fixture / "contract.ps1";
    build_test::write(script, R"ps(
param([string]$Repository,[string]$Root)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $Repository 'scripts/deployment_payload.psm1') -Force
function Write-Fixture([string]$Relative) {
    $path=Join-Path $Root $Relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))|Out-Null
    [IO.File]::WriteAllText($path,'fixture')
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}
$names=@('CppWinRT-LICENSE.txt','WindowsAppSDK-LICENSE.txt','WindowsAppSDK-NOTICE.txt',
    'WindowsML-LICENSE.txt','WindowsML-NOTICE.txt','WebView2-LICENSE.txt','WebView2-NOTICE.txt',
    'YASB-LICENSE.txt','MiSans-LICENSE.pdf','HarmonyOS-Sans-LICENSE.txt')
$notices=@(foreach($name in $names) {
    $path='.deployment/licenses/'+$name
    [ordered]@{path=$path;destination=('licenses/'+$name);sha256=(Write-Fixture $path)}
})
$manifest=[ordered]@{schemaVersion=1;architecture='x64';windowsAppSdkPackageVersion='2.4.0';cppWinRtPackageVersion='3.0.260818.1'
    files=@([ordered]@{path='fixture.bin';source='Generated';sha256=(Write-Fixture 'fixture.bin')})
    appxFragments=@([ordered]@{path='.deployment/appxfragments/fixture.appxfragment';source='fixture';sha256=(Write-Fixture '.deployment/appxfragments/fixture.appxfragment')})
    notices=$notices}
function Save { [IO.File]::WriteAllText((Join-Path $Root 'SnowDesktop.deployment.json'),($manifest|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false)) }
Save
$legacy=Read-SnowDesktopDeploymentManifest -BuildOutput $Root
if(@($legacy.notices).Count -ne 10){throw 'legacy ten-notice manifest must remain readable'}
$tool='performance/scripts/profile.ps1'
$manifest.files+=([ordered]@{path=$tool;source='SnowDesktop';sha256=(Write-Fixture $tool)})
Save
$rejected=$false
try { Read-SnowDesktopDeploymentManifest -BuildOutput $Root|Out-Null } catch { $rejected=$true }
if(-not $rejected){throw 'performance tools without Perfetto license must be rejected'}
$perfetto=[ordered]@{path='.deployment/licenses/Perfetto-LICENSE.txt';destination='licenses/Perfetto-LICENSE.txt';sha256=(Write-Fixture '.deployment/licenses/Perfetto-LICENSE.txt')}
$manifest.notices+= $perfetto
Save
$current=Read-SnowDesktopDeploymentManifest -BuildOutput $Root
if(@($current.notices).Count -ne 11){throw 'current eleven-notice manifest must be accepted'}
$delivery=Join-Path $Root 'delivery'
Copy-SnowDesktopDeploymentPayload -BuildOutput $Root -Destination $delivery -RuntimeDirectory 'SnowDesktop.Runtime'|Out-Null
if(-not (Test-Path -LiteralPath (Join-Path $delivery $tool)) -or (Test-Path -LiteralPath (Join-Path $delivery ('SnowDesktop.Runtime/'+$tool)))){throw 'host analysis tools must stay beside the executable'}
if(-not (Test-Path -LiteralPath (Join-Path $delivery 'licenses/Perfetto-LICENSE.txt'))){throw 'Perfetto notice must be distributed'}
$perfetto.destination='licenses/Unexpected.txt'
Save
$rejected=$false
try { Read-SnowDesktopDeploymentManifest -BuildOutput $Root|Out-Null } catch { $rejected=$true }
if(-not $rejected){throw 'extending one known notice must not accept arbitrary destinations'}
)ps");
    const auto result = build_test::run({build_test::powershell(true).wstring(), L"-NoProfile", L"-NonInteractive",
        L"-ExecutionPolicy", L"Bypass", L"-File", script.wstring(), L"-Repository", root.wstring(), L"-Root", fixture.wstring()}, fixture, 25);
    build_test::require(result.code == 0, "production deployment reader/copy handles legacy and Perfetto notice contracts: " + result.out + result.err);
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
        $assignment = @($ast.FindAll({ param($node)
            $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq $variable
        }, $true))
        if ($assignment.Count -ne 1) { throw 'Runtime inventory assignment not found' }
        . ([scriptblock]::Create($assignment[0].Extent.Text))
    }
    foreach ($name in $required) {
        $source = if ($runtimeFiles -contains $name) { Join-Path (Join-Path $buildOutput $runtimeDirectory) $name } else { Join-Path $buildOutput $name }
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($source)) -Force
        [IO.File]::WriteAllText($source, 'fixture:' + $name)
    }
    $loops = @($ast.FindAll({ param($node)
        $node -is [System.Management.Automation.Language.ForEachStatementAst] -and $node.Condition.Extent.Text -ceq '$required'
    }, $true))
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
                 "SnowDesktopDockMinimizeHook32.dll", "SnowDesktopDockMinimizeHost32.exe", "SnowDesktopWindowHelper.exe"})
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

}

int main(int argc, char** argv)
{
    Check(argc == 2, "repository root is provided");
    if (argc == 2)
    {
        const std::filesystem::path root(argv[1]);
        try
        {
            TestPerformanceDeployment(root);
            TestExecutableVersionResources(root);
            TestGitHubReleasePublication(root);
            TestDockHookPackaging(root);
            TestSteamPackagingPaths(root);
        }
        catch (const std::exception& error)
        {
            std::cerr << error.what() << '\n';
            Check(false, "production packaging behavior failed");
        }
    }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
