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
        "only the Steam payload includes the Workshop manager and complete Agent publishing CLI while every release keeps the authoring tool");
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
    const std::string& arranger,
    const std::string& testScript,
    const std::string& testManager)
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
    Check(testScript.find("test_manager.ps1") !=
                std::string::npos &&
            testScript.find("MODE=fast") !=
                std::string::npos &&
            testScript.find("MODE=core") !=
                std::string::npos &&
            testScript.find("MODE=label") !=
                std::string::npos &&
            testScript.find("MODE=name") !=
                std::string::npos &&
            testManager.find("--show-only=json-v1") !=
                std::string::npos &&
            testManager.find("^SnowDesktop.+Tests\\.exe$") !=
                std::string::npos &&
            testManager.find("-LE\", \"^integration$") !=
                std::string::npos &&
            testManager.find("BuildPreset \"fast-tests\"") !=
                std::string::npos &&
            testManager.find("rootTests.Count -ne 0") !=
                std::string::npos &&
            testManager.find("rootDlls.Count -ne 0") !=
                std::string::npos &&
            testManager.find("emptyRuntimeDirs.Count -ne 0") !=
                std::string::npos,
        "the standard test entry point supports layered CTest selection without duplicating target lists");
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
    Check(manager.find("$thirdPartyLicenses = Join-Path $temporary \"licenses\"") !=
                std::string::npos &&
            manager.find("-Source $thirdPartyLicenses") !=
                std::string::npos &&
            manager.find("-Destination (Join-Path $releaseRepository \"licenses\")") !=
                std::string::npos &&
            manager.find("-LogPath $syncLog") != std::string::npos,
        "release repository synchronization mirrors third-party licenses without nesting the licenses directory");
    Check(documentation.find(
              "scripts\\release.bat package -ReloadShell") !=
            std::string::npos &&
            documentation.find(
              "scripts\\release.bat prepare -ReloadShell") !=
                std::string::npos,
        "release documentation describes shell reload for package and prepare");
}

void TestSteamPipeAutomation(const std::string& steamPipe,
    const std::string& steamPipeConfiguration,
    const std::string& manager,
    const std::string& scriptDocumentation,
    const std::string& packagingDocumentation)
{
    Check(steamPipe.find("ValidateSet(\"Preview\", \"UploadDev\")") !=
                std::string::npos &&
            steamPipe.find("\"Preview\" \"1\"") != std::string::npos &&
            steamPipe.find("SetLive") != std::string::npos &&
            steamPipe.find("(\"default\", \"public\", \"release\", \"live\")") !=
                std::string::npos,
        "SteamPipe exposes preview and a denylisted private-development upload mode only");
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
            steamPipe.find("Assert-SteamPayloadManifest") !=
                std::string::npos &&
            steamPipe.find("Compare-Object -ReferenceObject $listed") !=
                std::string::npos &&
            steamPipe.find("Steam payload metadata mismatch") !=
                std::string::npos,
        "Steam development uploads require explicit version and branch confirmation over a current payload manifest");
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
            manager.find("Invoke-SteamPipeAction") != std::string::npos &&
            manager.find("ConfirmPrivateBranch") != std::string::npos,
        "the shared release manager exposes preview and explicitly confirmed private upload commands");
    Check(steamPipeConfiguration.find("\"privateDevelopmentBranch\"") !=
                std::string::npos &&
            steamPipeConfiguration.find("internal-dev") !=
                std::string::npos &&
            steamPipeConfiguration.find("password") == std::string::npos &&
            steamPipeConfiguration.find("account") == std::string::npos &&
            steamPipeConfiguration.find("token") == std::string::npos,
        "committed SteamPipe configuration contains a branch but no credentials");
    Check(scriptDocumentation.find("steam-preview") != std::string::npos &&
            scriptDocumentation.find("steam-upload-dev") !=
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
    const std::string& releaseBuild,
    const std::string& debugBuild)
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
    Check(releaseBuild.find(
              ".build\\Release\\SnowDesktop.Runtime\\SnowDesktopTaskbarHook.dll") !=
            std::string::npos &&
            debugBuild.find(
              ".build_debug\\Debug\\SnowDesktop.Runtime\\SnowDesktopTaskbarHook.dll") !=
                std::string::npos &&
            releaseBuild.find("arrange_build_output.ps1") !=
                std::string::npos &&
            releaseBuild.find("Get-Process -Name explorer -ErrorAction Stop") !=
                std::string::npos,
        "build preflight distinguishes build hooks from disposable temporary copies");
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
    Check(dropExtraction.find("GetTempPathW") == std::string::npos &&
            dropExtraction.find("GetDataSubdirectoryPath(L\"DropContent\")") !=
                std::string::npos,
        "non-file drop content is materialized only below the SnowDesktop data root");
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
            ReadText(root / "scripts/arrange_build_output.ps1"),
            ReadText(root / "scripts/test.bat"),
            ReadText(root / "scripts/test_manager.ps1"));
        TestReleaseManagerShellReload(
            ReadText(root / "scripts/release_manager.ps1"),
            ReadText(root / "packaging/README.md"));
        TestSteamPipeAutomation(
            ReadText(root / "scripts/steam_pipe.ps1"),
            ReadText(root / "packaging/steam-pipe.json"),
            ReadText(root / "scripts/release_manager.ps1"),
            ReadText(root / "scripts/README.md"),
            ReadText(root / "packaging/README.md"));
        TestRuntimeResolution(
            ReadText(root / "src/deployment_context.h"),
            ReadText(root / "src/deployment_context.cpp"),
            ReadText(root / "src/main.cpp"),
            ReadText(root / "src/app/wallpaper_engine_capture.cpp"),
            ReadText(root / "scripts/build.bat"),
            ReadText(root / "scripts/build_debug.bat"));
        TestAutoStartTransitionManifest(
            ReadText(root / "packaging/AppxManifest.xml.in"));
        TestOwnedRuntimeStaging(
            ReadText(root / "src/app/app_drop_data_extraction.cpp"),
            ReadText(root / "src/widget_package.cpp"),
            ReadText(root / "tools/snowwidget/main.cpp"),
            ReadText(root / "steam_bridge/src/package_tool.cpp"),
            ReadText(root / "steam_bridge/src/steam_workshop_core.cpp"),
            ReadText(root / "steam_bridge/src/manager_main.cpp"),
            ReadText(root / "src/steam_workshop_source.cpp"),
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
