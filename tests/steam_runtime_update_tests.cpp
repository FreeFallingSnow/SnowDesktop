#include "auto_start_rules.h"
#include "data_path_policy.h"
#include "steam_runtime_context.h"
#include "steam_runtime_manager.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

void WriteText(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream)
        throw std::runtime_error("cannot write test file");
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

std::string Sha256(const std::filesystem::path& path)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD hashSize = 0;
    DWORD written = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &written, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
            &written, 0) < 0)
    {
        throw std::runtime_error("cannot initialize test SHA-256");
    }
    std::vector<UCHAR> object(objectSize);
    std::vector<UCHAR> digest(hashSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
            nullptr, 0, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("cannot create test SHA-256");
    }
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 4096> buffer{};
    while (stream)
    {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (stream.gcount() > 0 && BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(stream.gcount()), 0) < 0)
        {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error("cannot update test SHA-256");
        }
    }
    if (BCryptFinishHash(hash, digest.data(), hashSize, 0) < 0)
    {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("cannot finish test SHA-256");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (const UCHAR byte : digest)
        text << std::setw(2) << static_cast<unsigned>(byte);
    return text.str();
}

std::string JsonEscape(std::string_view value)
{
    std::string result;
    for (const char character : value)
    {
        if (character == '\\' || character == '"')
            result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

void WriteDistribution(const std::filesystem::path& root,
    std::string_view version, std::string_view buildId,
    std::string_view hostContents, std::string_view libraryContents)
{
    const auto distribution = root / L"distribution";
    const auto host = distribution / L"SnowDesktop.exe";
    const auto library = distribution / L"SnowDesktop.Runtime" /
        L"runtime.dll";
    WriteText(host, hostContents);
    WriteText(library, libraryContents);

    std::ostringstream manifest;
    manifest << "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"kind\": \"steam-managed\",\n"
        "  \"version\": \"" << JsonEscape(version) << "\",\n"
        "  \"buildId\": \"" << JsonEscape(buildId) << "\",\n"
        "  \"distributionDirectory\": \"distribution\",\n"
        "  \"runtimeDirectory\": \".snowdesktop/runtime\",\n"
        "  \"dataDirectory\": \"data\",\n"
        "  \"files\": [\n"
        "    {\"path\":\"SnowDesktop.exe\",\"size\":"
        << std::filesystem::file_size(host) << ",\"sha256\":\""
        << Sha256(host) << "\"},\n"
        "    {\"path\":\"SnowDesktop.Runtime/runtime.dll\",\"size\":"
        << std::filesystem::file_size(library) << ",\"sha256\":\""
        << Sha256(library) << "\"}\n"
        "  ]\n"
        "}\n";
    WriteText(root / snowdesktop::steam_runtime::
        kDistributionManifestFilename, manifest.str());
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

void TestContextResolution(const std::filesystem::path& root)
{
    using namespace snowdesktop::deployment;
    const auto portable = root / L"portable" / L"SnowDesktop.exe";
    WriteText(portable, "portable");
    auto context = ResolveRuntimeDeploymentContext(portable, false);
    Check(context.kind == RuntimeDeploymentKind::Portable &&
            !context.explicitContext,
        "an executable without a sidecar remains exactly portable");

    WriteText(portable.parent_path() / kSteamRuntimeContextFilename,
        "{not-json");
    context = ResolveRuntimeDeploymentContext(portable, true);
    Check(context.kind == RuntimeDeploymentKind::Packaged &&
            !context.explicitContext,
        "MSIX package identity wins even beside a malformed Steam sidecar");
    context = ResolveRuntimeDeploymentContext(portable, false);
    Check(context.kind == RuntimeDeploymentKind::Invalid &&
            context.explicitContext,
        "an explicit malformed Steam context fails closed");

    const auto nonDirectoryParent = root / L"sidecar-parent-is-a-file";
    WriteText(nonDirectoryParent, "not a directory");
    context = ResolveRuntimeDeploymentContext(
        nonDirectoryParent / L"SnowDesktop.exe", false);
    Check(context.kind == RuntimeDeploymentKind::Invalid &&
            context.explicitContext && !context.error.empty(),
        "a sidecar path-type probe error fails closed instead of becoming portable");

    const auto overlongParent = root / std::wstring(300, L'x');
    context = ResolveRuntimeDeploymentContext(
        overlongParent / L"SnowDesktop.exe", false);
    Check(context.kind == RuntimeDeploymentKind::Invalid &&
            context.explicitContext && !context.error.empty(),
        "a sidecar I/O probe error fails closed instead of becoming portable");

    const auto install = root / L"managed";
    const auto runtime = install / L".snowdesktop" / L"runtime" /
        L"build-1";
    const auto executable = runtime / L"SnowDesktop.exe";
    WriteText(install / kSteamLauncherFilename, "launcher");
    WriteText(executable, "host");
    WriteManagedSidecar(runtime);
    context = ResolveRuntimeDeploymentContext(executable, false);
    Check(context.kind == RuntimeDeploymentKind::SteamManaged &&
            context.installRoot == std::filesystem::weakly_canonical(install) &&
            context.dataRoot == std::filesystem::weakly_canonical(
                install / L"data") &&
            context.launcher == std::filesystem::weakly_canonical(
                install / kSteamLauncherFilename),
        "managed Steam runtime resolves stable launcher and unique data root");

    const auto devInstall = root / L"dev-install";
    const auto devRuntime = devInstall / L".snowdesktop" / L"dev" /
        L"abc123";
    const auto devExecutable = devRuntime / L"SnowDesktop.exe";
    WriteText(devExecutable, "dev host");
    WriteText(devRuntime / kSteamRuntimeContextFilename,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"kind\": \"steam-local-dev\",\n"
        "  \"installRootRelative\": \"../../..\",\n"
        "  \"dataRootRelative\": \".snowdesktop/dev-data/debug\",\n"
        "  \"launcherRelative\": \".snowdesktop/dev/abc123/SnowDesktop.exe\",\n"
        "  \"profileId\": \"debug\"\n"
        "}\n");
    context = ResolveRuntimeDeploymentContext(devExecutable, false);
    Check(context.kind == RuntimeDeploymentKind::SteamLocalDevelopment &&
            context.dataRoot == std::filesystem::weakly_canonical(
                devInstall / L".snowdesktop" / L"dev-data" / L"debug") &&
            !CanOwnProductionAutoStart(context.kind),
        "local Steam development is isolated and cannot own production startup");
}

void TestRuntimeDataPathPolicy(const std::filesystem::path& root)
{
    using snowdesktop::data_paths::EnsureDirectoryTree;
    using snowdesktop::data_paths::ResolveRuntimeDataPathPolicy;
    using snowdesktop::deployment::RuntimeDeploymentContext;
    using snowdesktop::deployment::RuntimeDeploymentKind;

    const auto executableDirectory = root / L"portable";
    RuntimeDeploymentContext context;
    auto policy = ResolveRuntimeDataPathPolicy(
        context, {}, executableDirectory);
    Check(policy.dataRoot == executableDirectory / L"data" &&
            policy.legacyRoot == executableDirectory &&
            policy.pendingMigrationStateRoot == executableDirectory,
        "portable data and migration roots retain their legacy layout");

    const auto localState = root / L"LocalState";
    context.kind = RuntimeDeploymentKind::Packaged;
    policy = ResolveRuntimeDataPathPolicy(
        context, localState, root / L"package-runtime");
    Check(policy.dataRoot == localState / L"data" &&
            policy.legacyRoot == localState &&
            policy.pendingMigrationStateRoot == localState,
        "packaged data and migration roots retain their LocalState layout");

    const auto installRoot = root / L"steam";
    context.kind = RuntimeDeploymentKind::SteamManaged;
    context.installRoot = installRoot;
    context.dataRoot = installRoot / L"data";
    policy = ResolveRuntimeDataPathPolicy(
        context, {}, root / L"managed-runtime");
    Check(policy.dataRoot == installRoot / L"data" &&
            policy.legacyRoot == installRoot &&
            policy.pendingMigrationStateRoot == installRoot,
        "managed Steam keeps its stable production data and migration roots");

    const auto productionData = installRoot / L"data";
    const auto productionLegacy = installRoot / L"SnowDesktop.general.json";
    WriteText(productionData / L"production.txt", "production-data");
    WriteText(productionLegacy, "production-legacy");

    context.kind = RuntimeDeploymentKind::SteamLocalDevelopment;
    context.dataRoot = installRoot / L".snowdesktop" / L"dev-data" /
        L"debug";
    policy = ResolveRuntimeDataPathPolicy(
        context, {}, root / L"dev-runtime");
    Check(policy.dataRoot == context.dataRoot &&
            !policy.legacyRoot && !policy.pendingMigrationStateRoot,
        "local Steam development exposes no production migration or legacy root");
    Check(EnsureDirectoryTree(policy.dataRoot) &&
            std::filesystem::is_directory(policy.dataRoot),
        "local Steam development recursively creates its profile data root");
    Check(ReadText(productionData / L"production.txt") ==
                "production-data" &&
            ReadText(productionLegacy) == "production-legacy",
        "creating a local profile leaves production data and legacy files untouched");
}

void TestRuntimeUpdate(const std::filesystem::path& root)
{
    WriteText(root / snowdesktop::deployment::kSteamLauncherFilename,
        "launcher");
    WriteDistribution(root, "1.0.5.0", "build-one", "host one", "dll one");
    auto first = snowdesktop::steam_runtime::ApplyDistribution(root);
    Check(first.ok && !first.usedFallback && first.buildId == "build-one",
        "the first valid distribution becomes the active runtime");
    Check(ReadText(first.executable) == "host one",
        "the first runtime is copied from distribution");

    const auto userData = root / L"data" / L"unique-user-data.txt";
    WriteText(userData, "keep me");
    HANDLE occupied = CreateFileW(first.executable.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
        nullptr);
    Check(occupied != INVALID_HANDLE_VALUE,
        "the old runtime can be held open during an update test");

    WriteDistribution(root, "1.0.5.0", "build-two", "host two", "dll two");
    auto second = snowdesktop::steam_runtime::ApplyDistribution(root);
    Check(second.ok && !second.usedFallback && second.buildId == "build-two",
        "a new distribution is materialized while the prior runtime is occupied");
    Check(ReadText(first.executable) == "host one" &&
            ReadText(second.executable) == "host two",
        "an update never overwrites the occupied immutable runtime");
    Check(ReadText(userData) == "keep me",
        "runtime switching preserves the single stable data directory");

    WriteDistribution(root, "1.0.5.0", "build-three",
        "host three", "dll three");
    WriteText(root / L"distribution" / L"SnowDesktop.exe", "corrupt");
    auto fallback = snowdesktop::steam_runtime::ApplyDistribution(root);
    Check(fallback.ok && fallback.usedFallback &&
            fallback.buildId == "build-two" &&
            fallback.executable == second.executable,
        "an incomplete Steam update falls back to the last completed runtime");
    Check(ReadText(userData) == "keep me",
        "a failed update cannot replace or delete user data");

    if (occupied != INVALID_HANDLE_VALUE)
        CloseHandle(occupied);
}

void TestSteamAutoStartRules()
{
    using namespace snowdesktop;
    const auto clean = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Steam,
        LegacyAutoStartState::Missing,
        LegacyAutoStartState::Missing);
    Check(clean.canMigrate && !clean.enableUnifiedTask &&
            clean.owner == UnifiedAutoStartOwner::Steam,
        "a managed Steam deployment can own a newly created disabled task");
    const auto portableLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Steam,
        LegacyAutoStartState::Enabled,
        LegacyAutoStartState::Missing);
    Check(portableLegacy.canMigrate && portableLegacy.enableUnifiedTask &&
            portableLegacy.owner == UnifiedAutoStartOwner::Portable,
        "Steam migration preserves a sole active portable legacy owner");
}
}

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        (L"SnowDesktop-steam-runtime-tests-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    try
    {
        TestContextResolution(root / L"contexts");
        TestRuntimeDataPathPolicy(root / L"data-paths");
        TestRuntimeUpdate(root / L"update");
        TestSteamAutoStartRules();
    }
    catch (const std::exception& exception)
    {
        ++failures;
        std::cerr << "FAIL: unexpected exception: " << exception.what()
                  << '\n';
    }
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    if (failures != 0)
    {
        std::cerr << failures << " Steam runtime update check(s) failed\n";
        return 1;
    }
    std::cout << "Steam runtime update checks passed\n";
    return 0;
}
