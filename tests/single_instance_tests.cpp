#include "single_instance.h"
#include "steam_runtime_context.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <shlobj.h>

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

void CheckDataDirectory(std::wstring_view actual,
    const std::filesystem::path& expected, const char* message)
{
    std::error_code canonicalError;
    const auto canonicalExpected =
        std::filesystem::weakly_canonical(expected, canonicalError);
    const auto& comparableExpected = canonicalError
        ? expected
        : canonicalExpected;
    if (snowdesktop::single_instance::DataDirectoriesMatch(
            actual, comparableExpected.wstring()))
    {
        return;
    }
    ++failures;
    std::wcerr << L"FAIL: " << message << L"\n  actual: " << actual
               << L"\n  expected: " << comparableExpected.wstring() << L'\n';
}

void WriteText(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream)
        throw std::runtime_error("cannot write test file");
}

std::filesystem::path PackagedDataRoot(std::wstring_view familyName)
{
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
    {
        return {};
    }
    std::filesystem::path result(localAppData);
    CoTaskMemFree(localAppData);
    return result / L"Packages" / familyName / L"LocalState" / L"data";
}

void WriteManagedSidecar(const std::filesystem::path& runtime)
{
    WriteText(runtime /
            snowdesktop::deployment::kSteamRuntimeContextFilename,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"kind\": \"steam-managed\",\n"
        "  \"installRootRelative\": \"../../..\",\n"
        "  \"dataRootRelative\": \"data\",\n"
        "  \"launcherRelative\": \"SnowDesktopLauncher.exe\"\n"
        "}\n");
}

void WriteLocalDevelopmentSidecar(const std::filesystem::path& runtime,
    std::string_view buildId, std::string_view profileId)
{
    WriteText(runtime /
            snowdesktop::deployment::kSteamRuntimeContextFilename,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"kind\": \"steam-local-dev\",\n"
        "  \"installRootRelative\": \"../../..\",\n"
        "  \"dataRootRelative\": \".snowdesktop/dev-data/" +
            std::string(profileId) + "\",\n"
        "  \"launcherRelative\": \".snowdesktop/dev/" +
            std::string(buildId) + "/SnowDesktop.exe\",\n"
        "  \"profileId\": \"" + std::string(profileId) + "\"\n"
        "}\n");
}

void TestDeploymentDataResolution(const std::filesystem::path& root)
{
    using snowdesktop::single_instance::DataDirectoriesMatch;
    using snowdesktop::single_instance::ResolveInstanceDataDirectory;

    const auto portable = root / L"portable" / L"SnowDesktop.exe";
    WriteText(portable, "portable");
    Check(DataDirectoriesMatch(
            ResolveInstanceDataDirectory(portable.wstring()),
            (portable.parent_path() / L"data").wstring()),
        "a sidecar-free portable instance retains executable-relative data");

    WriteText(portable.parent_path() /
            snowdesktop::deployment::kSteamRuntimeContextFilename,
        "{not-json");
    Check(ResolveInstanceDataDirectory(portable.wstring()).empty(),
        "an invalid explicit Steam sidecar does not fall back to portable data");

    constexpr std::wstring_view packageFamily =
        L"FreeFallingSnow.SnowDesktop.Test_123456789abcd";
    const auto packagedExpected = PackagedDataRoot(packageFamily);
    Check(!packagedExpected.empty() && DataDirectoriesMatch(
            ResolveInstanceDataDirectory(
                portable.wstring(), packageFamily),
            packagedExpected.wstring()),
        "MSIX package identity retains LocalState data beside a sidecar");

    const auto managedInstall = root / L"managed";
    const auto managedRuntime = managedInstall / L".snowdesktop" /
        L"runtime" / L"build-1";
    const auto managedExecutable = managedRuntime / L"SnowDesktop.exe";
    WriteText(managedInstall /
        snowdesktop::deployment::kSteamLauncherFilename, "launcher");
    WriteText(managedExecutable, "managed host");
    WriteManagedSidecar(managedRuntime);
    CheckDataDirectory(
        ResolveInstanceDataDirectory(managedExecutable.wstring()),
        managedInstall / L"data",
        "a managed Steam instance reports the install-root data directory");

    constexpr std::string_view buildId = "build-local";
    constexpr std::string_view profileId = "profile-local";
    const auto localInstall = root / L"local";
    const auto localRuntime = localInstall / L".snowdesktop" / L"dev" /
        std::filesystem::path(buildId);
    const auto localExecutable = localRuntime / L"SnowDesktop.exe";
    WriteText(localExecutable, "local host");
    WriteLocalDevelopmentSidecar(localRuntime, buildId, profileId);
    CheckDataDirectory(
        ResolveInstanceDataDirectory(localExecutable.wstring()),
        localInstall / L".snowdesktop" / L"dev-data" /
            std::filesystem::path(profileId),
        "a local Steam development instance reports its isolated profile data");
}

void TestManagedSteamRuntimeReplacement(const std::filesystem::path& root)
{
    using snowdesktop::single_instance::InstanceInfo;
    using snowdesktop::single_instance::IsManagedSteamRuntimeReplacement;

    const auto install = root / L"managed-replacement";
    WriteText(install /
        snowdesktop::deployment::kSteamLauncherFilename, "launcher");
    const auto oldRuntime = install / L".snowdesktop" /
        L"runtime" / L"build-old";
    const auto newRuntime = install / L".snowdesktop" /
        L"runtime" / L"build-new";
    const auto oldExecutable = oldRuntime / L"SnowDesktop.exe";
    const auto newExecutable = newRuntime / L"SnowDesktop.exe";
    WriteText(oldExecutable, "old host");
    WriteText(newExecutable, "new host");
    WriteManagedSidecar(oldRuntime);
    WriteManagedSidecar(newRuntime);

    InstanceInfo running;
    running.executablePath = oldExecutable.wstring();
    InstanceInfo requested;
    requested.executablePath = newExecutable.wstring();
    Check(IsManagedSteamRuntimeReplacement(running, requested),
        "different immutable runtimes in one managed Steam install require an automatic handoff");

    requested.executablePath = oldExecutable.wstring();
    Check(!IsManagedSteamRuntimeReplacement(running, requested),
        "the same managed Steam runtime remains an ordinary same-instance activation");

    const auto otherInstall = root / L"other-managed-replacement";
    WriteText(otherInstall /
        snowdesktop::deployment::kSteamLauncherFilename, "launcher");
    const auto otherRuntime = otherInstall / L".snowdesktop" /
        L"runtime" / L"build-new";
    const auto otherExecutable = otherRuntime / L"SnowDesktop.exe";
    WriteText(otherExecutable, "other host");
    WriteManagedSidecar(otherRuntime);
    requested.executablePath = otherExecutable.wstring();
    Check(!IsManagedSteamRuntimeReplacement(running, requested),
        "managed Steam runtimes from different installs retain the explicit version-conflict flow");
}
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktopSingleInstanceTests-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    std::error_code cleanupError;
    try
    {
        TestDeploymentDataResolution(root);
        TestManagedSteamRuntimeReplacement(root);
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        ++failures;
    }
    std::filesystem::remove_all(root, cleanupError);
    if (cleanupError)
    {
        std::cerr << "FAIL: temporary test cleanup failed: "
                  << cleanupError.message() << '\n';
        ++failures;
    }
    if (failures == 0)
        std::cout << "single_instance_tests: passed\n";
    return failures == 0 ? 0 : 1;
}
