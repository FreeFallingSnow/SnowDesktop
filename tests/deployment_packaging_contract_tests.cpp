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
             "Assets\\App\\SnowDesktop.png"})
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
            writer.find("CppWinRT-LICENSE.txt") != std::string::npos,
        "deployment manifest carries the pinned NuGet license and notice files");
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
            module.find("hash mismatch") != std::string::npos,
        "shared payload reader rejects unsafe and stale manifest entries");
    Check(release.find("Copy-SnowDesktopDeploymentPayload") !=
            std::string::npos &&
            steam.find("Copy-SnowDesktopDeploymentPayload") !=
                std::string::npos,
        "portable, MSIX, and Steam payloads use the same deployment manifest");
    Check(release.find("SnowDesktopWorkshopManager.exe") !=
            std::string::npos &&
            release.find("widgets\\snowdesktop-lua-widget\\bin\\snowwidget.exe") !=
                std::string::npos,
        "portable and MSIX payloads include component management and Agent Skill tools");
    Check(release.find("Merge-SnowDesktopAppxFragments") !=
            std::string::npos &&
            module.find("windows.activatableClass.") != std::string::npos,
        "MSIX merges official activatable-class package fragments");
    Check(module.find("Get-ChildItem") == std::string::npos &&
            release.find("$buildOutput + \"*\"") == std::string::npos &&
            steam.find("$buildOutput + \"*\"") == std::string::npos,
        "shared deployment copying never globs the build directory");
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
    }

    if (failures != 0)
    {
        std::cerr << failures << " deployment packaging contract check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Deployment packaging contract checks passed\n";
    return EXIT_SUCCESS;
}
