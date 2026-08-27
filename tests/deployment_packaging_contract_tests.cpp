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
    return output.str();
}

void TestPinnedToolchain(const std::string& cmake)
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
}

void TestBuildManifest(const std::string& props,
    const std::string& writer)
{
    for (const char* item : {
             "@(MicrosoftWindowsAppSDKFiles)",
             "@(MicrosoftWindowsAppSDKFilesRes)",
             "@(MicrosoftWindowsAppSDKAppxFragmentFiles)",
             "App.xbf", "SettingsShell.xbf", "SnowDesktop.pri",
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
    Check(module.find("Enable-SnowDesktopPrivateRuntimeAssembly") !=
            std::string::npos &&
            module.find("SnowDesktop.Runtime") != std::string::npos &&
            release.find("-RuntimeDirectory $runtimeDirectory") !=
                std::string::npos &&
            steam.find("-RuntimeDirectory $runtimeDirectory") !=
                std::string::npos,
        "release payloads isolate third-party runtime files in one private assembly directory");
    Check(release.find("SnowDesktopWorkshopManager.exe") ==
            std::string::npos &&
            release.find("widgets\\snowdesktop-lua-widget\\bin\\snowwidget.exe") !=
                std::string::npos &&
            steam.find("SnowDesktopWorkshopManager.exe") !=
                std::string::npos,
        "only the Steam payload includes the Workshop manager while every release keeps the Agent Skill tool");
    Check(release.find("$runtimeDestination") != std::string::npos &&
            release.find("SnowDesktopWallpaperInjector32.exe") !=
                std::string::npos &&
            steam.find("$runtimeFiles = @(") != std::string::npos &&
            steam.find(
                "steamworksRedistributable = \"$runtimeDirectory/steam_api64.dll\"") !=
                std::string::npos,
        "first-party runtime helpers and the Steam redistributable are routed into the runtime directory");
    Check(module.find("AdditionalRuntimeDlls") != std::string::npos &&
            module.find("AdditionalExecutables") != std::string::npos &&
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
    Check(release.find("Disable-InputPriMerging") != std::string::npos &&
            release.find("Where-Object { $_.type -eq \"PRI\" }") !=
                std::string::npos &&
            release.find("Disable-InputPriMerging -Path $priConfig") !=
                std::string::npos,
        "MSIX package resources do not remerge self-contained component PRI files");
    Check(module.find("Get-ChildItem") == std::string::npos &&
            release.find("$buildOutput + \"*\"") == std::string::npos &&
            steam.find("$buildOutput + \"*\"") == std::string::npos,
        "shared deployment copying never globs the build directory");
}

void TestReleaseManagerShellReload(const std::string& manager,
    const std::string& documentation)
{
    Check(manager.find("[switch]$ReloadShell") != std::string::npos &&
            manager.find("\"--reload-shell\"") != std::string::npos &&
            manager.find("-BatchArguments $buildArguments") !=
                std::string::npos,
        "release manager forwards the explicit shell reload option to the standard build");
    Check(manager.find("Get-BuildOccupancy") != std::string::npos &&
            manager.find("SnowDesktopTaskbarHook.dll") !=
                std::string::npos &&
            manager.find("$buildHook") != std::string::npos &&
            manager.find("-ReloadShellBeforeBuild:$ReloadShell") !=
                std::string::npos,
        "release CLI and TUI only treat the build-directory hook as build occupancy");
    Check(manager.find("SnowDesktop.Runtime") != std::string::npos &&
            manager.find("SnowDesktopWorkshopManager.exe") !=
                std::string::npos &&
            manager.find("Remove-Item -LiteralPath $legacyPath") !=
                std::string::npos,
        "release repository synchronization mirrors the runtime directory and removes obsolete root helpers");
    Check(documentation.find(
              "scripts\\release.bat package -ReloadShell") !=
            std::string::npos &&
            documentation.find(
              "scripts\\release.bat prepare -ReloadShell") !=
                std::string::npos,
        "release documentation describes shell reload for package and prepare");
}

void TestRuntimeResolution(const std::string& deploymentHeader,
    const std::string& deploymentSource,
    const std::string& wallpaperCapture,
    const std::string& releaseBuild,
    const std::string& debugBuild)
{
    Check(deploymentHeader.find("GetRuntimeFilePath") !=
            std::string::npos &&
            deploymentSource.find("SnowDesktop.Runtime") !=
                std::string::npos,
        "application runtime lookup prefers the packaged runtime directory");
    Check(deploymentSource.find("DeployTaskbarHookCopy") !=
            std::string::npos &&
            deploymentSource.find("GetTemporaryDirectory") !=
                std::string::npos &&
            deploymentSource.find(
                "static const std::wstring deployedPath") !=
                std::string::npos,
        "taskbar injection uses one process-specific temporary hook copy");
    Check(wallpaperCapture.find("RuntimeFilePath") !=
            std::string::npos &&
            wallpaperCapture.find("SnowDesktop.Runtime") !=
                std::string::npos &&
            wallpaperCapture.find("injector.parent_path().c_str()") !=
                std::string::npos,
        "wallpaper hooks and the 32-bit injector resolve from the runtime directory");
    Check(releaseBuild.find(
              ".build\\Release\\SnowDesktopTaskbarHook.dll") !=
            std::string::npos &&
            debugBuild.find(
              ".build_debug\\Debug\\SnowDesktopTaskbarHook.dll") !=
                std::string::npos &&
            releaseBuild.find("Get-Process -Name explorer -ErrorAction Stop") !=
                std::string::npos,
        "build preflight distinguishes build hooks from disposable temporary copies");
}
}

int main(int argc, char** argv)
{
    Check(argc == 2, "repository root is provided");
    if (argc == 2)
    {
        const std::filesystem::path root(argv[1]);
        TestPinnedToolchain(ReadText(root / "CMakeLists.txt"));
        TestBuildManifest(
            ReadText(root / "cmake/SnowDesktop.WinUI.props"),
            ReadText(root / "scripts/write_deployment_manifest.ps1"));
        TestPackagers(
            ReadText(root / "scripts/deployment_payload.psm1"),
            ReadText(root / "scripts/package_release.ps1"),
            ReadText(root / "scripts/package_steam.ps1"));
        TestReleaseManagerShellReload(
            ReadText(root / "scripts/release_manager.ps1"),
            ReadText(root / "packaging/README.md"));
        TestRuntimeResolution(
            ReadText(root / "src/deployment_context.h"),
            ReadText(root / "src/deployment_context.cpp"),
            ReadText(root / "src/app/wallpaper_engine_capture.cpp"),
            ReadText(root / "scripts/build.bat"),
            ReadText(root / "scripts/build_debug.bat"));
    }

    if (failures != 0)
    {
        std::cerr << failures << " deployment packaging contract check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Deployment packaging contract checks passed\n";
    return EXIT_SUCCESS;
}
