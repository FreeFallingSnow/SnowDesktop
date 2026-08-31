#include "auto_start_rules.h"
#include "data_path_policy.h"
#include "steam_runtime_context.h"
#include "steam_runtime_manager.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
int failures = 0;
int skips = 0;
constexpr wchar_t kRuntimeCompleteFilename[] =
    L".snowdesktop-runtime-complete";
constexpr wchar_t kCurrentRuntimeFilename[] = L"current-runtime.txt";
constexpr wchar_t kRuntimeManifestFilename[] =
    L".snowdesktop-runtime-manifest.json";

class OccupiedFile final
{
public:
    explicit OccupiedFile(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }

    ~OccupiedFile()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }

    OccupiedFile(const OccupiedFile&) = delete;
    OccupiedFile& operator=(const OccupiedFile&) = delete;

    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void Skip(const char* message, DWORD error = ERROR_SUCCESS)
{
    ++skips;
    std::cout << "SKIP: " << message;
    if (error != ERROR_SUCCESS)
        std::cout << " (Windows error " << error << ')';
    std::cout << '\n';
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

using DirectorySnapshot = std::vector<
    std::pair<std::filesystem::path, std::string>>;

DirectorySnapshot SnapshotDirectory(const std::filesystem::path& root)
{
    DirectorySnapshot result;
    for (const auto& entry :
        std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;
        result.emplace_back(
            entry.path().lexically_relative(root), ReadText(entry.path()));
    }
    std::sort(result.begin(), result.end(), [](const auto& left,
                  const auto& right) {
        return left.first.generic_wstring() < right.first.generic_wstring();
    });
    return result;
}

bool IsReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool CreateDirectoryJunction(const std::filesystem::path& link,
    const std::filesystem::path& target)
{
    std::filesystem::create_directories(link.parent_path());
    const std::wstring command = L"cmd.exe /d /c mklink /J \"" +
        std::filesystem::absolute(link).wstring() + L"\" \"" +
        std::filesystem::absolute(target).wstring() +
        L"\" >nul 2>&1";
    return _wsystem(command.c_str()) == 0 && IsReparsePoint(link);
}

bool CreateFileSymlink(const std::filesystem::path& link,
    const std::filesystem::path& target, DWORD& error)
{
    error = ERROR_SUCCESS;
    constexpr DWORD allowUnprivilegedCreate = 0x2;
    if (CreateSymbolicLinkW(link.c_str(),
            std::filesystem::absolute(target).c_str(),
            allowUnprivilegedCreate) ||
        (GetLastError() == ERROR_INVALID_PARAMETER &&
            CreateSymbolicLinkW(link.c_str(),
                std::filesystem::absolute(target).c_str(), 0)))
    {
        return IsReparsePoint(link);
    }
    error = GetLastError();
    return false;
}

bool CreateFileHardLink(const std::filesystem::path& link,
    const std::filesystem::path& target)
{
    std::filesystem::create_directories(link.parent_path());
    return CreateHardLinkW(link.c_str(), target.c_str(), nullptr) != FALSE;
}

std::filesystem::path FindRepositoryFile(
    const std::filesystem::path& relativePath)
{
    std::filesystem::path testSource(__FILE__);
    if (testSource.is_absolute())
    {
        const auto candidate = testSource.parent_path().parent_path() /
            relativePath;
        if (std::filesystem::is_regular_file(candidate))
            return candidate;
    }

    std::filesystem::path current = std::filesystem::current_path();
    while (!current.empty())
    {
        const auto candidate = current / relativePath;
        if (std::filesystem::is_regular_file(candidate))
            return candidate;
        if (current == current.parent_path())
            break;
        current = current.parent_path();
    }
    return {};
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

snowdesktop::steam_runtime::ApplyResult PrepareRuntime(
    const std::filesystem::path& root, std::string_view buildId,
    std::string_view hostContents = "host clean",
    std::string_view libraryContents = "dll clean")
{
    WriteText(root / snowdesktop::deployment::kSteamLauncherFilename,
        "launcher");
    WriteDistribution(root, "1.0.5.0", buildId,
        hostContents, libraryContents);
    auto result = snowdesktop::steam_runtime::ApplyDistribution(root);
    Check(result.ok && !result.usedFallback &&
            std::string_view(result.buildId) == buildId,
        "the runtime test fixture is published before corruption");
    return result;
}

void TestInactiveRuntimePruning(const std::filesystem::path& root)
{
    const auto oldRuntime = PrepareRuntime(
        root, "1.0.5.0-1111111111111111", "host old", "dll old");
    const auto currentRuntime = PrepareRuntime(
        root, "1.0.5.0-2222222222222222", "host current", "dll current");
    if (!oldRuntime.ok || !currentRuntime.ok)
        return;

    const auto runtimeRoot = root / L".snowdesktop" / L"runtime";
    WriteText(oldRuntime.executable.parent_path() / L"imgui.ini",
        "unexpected runtime residue");
    WriteText(oldRuntime.executable.parent_path() / L"data" /
        L"snowwidget" / L"staging" / L"residue.txt",
        "unexpected runtime data");

    const auto unknownDirectory = runtimeRoot / L"user-unknown";
    WriteText(unknownDirectory / L"keep.txt", "not a published runtime");
    const auto similarUnknownDirectory =
        runtimeRoot / L"1.0.5.0-not-a-runtime-digest";
    WriteText(similarUnknownDirectory / L"keep.txt",
        "not a launcher-owned runtime");
    {
        OccupiedFile occupied(oldRuntime.executable);
        Check(occupied.valid(),
            "the inactive runtime can be held open during retirement");
        const auto retained =
            snowdesktop::steam_runtime::PruneInactiveRuntimes(
                root, currentRuntime.executable);
        Check(retained.ok && retained.removed == 0 &&
                retained.retained == 1 &&
                std::filesystem::exists(oldRuntime.executable) &&
                std::filesystem::exists(currentRuntime.executable),
            "runtime pruning retains an occupied old runtime without touching the current runtime");
    }

    const auto pruned = snowdesktop::steam_runtime::PruneInactiveRuntimes(
        root, currentRuntime.executable);
    Check(pruned.ok && pruned.removed == 1 && pruned.retained == 0 &&
            !std::filesystem::exists(oldRuntime.executable) &&
            std::filesystem::exists(currentRuntime.executable),
        "runtime pruning removes a polluted old runtime after its final handle closes");
    Check(std::filesystem::exists(unknownDirectory / L"keep.txt"),
        "runtime pruning preserves an unrelated directory in the managed root");
    Check(std::filesystem::exists(similarUnknownDirectory / L"keep.txt"),
        "runtime pruning preserves a directory outside launcher naming rules");
}

void CorruptDistributionLibrary(const std::filesystem::path& root)
{
    WriteText(root / L"distribution" / L"SnowDesktop.Runtime" /
        L"runtime.dll", "dll evil!");
}

void CorruptDistributionManifest(const std::filesystem::path& root)
{
    WriteText(root /
            snowdesktop::steam_runtime::kDistributionManifestFilename,
        "{not-json");
}

void CheckFallbackRejected(
    const snowdesktop::steam_runtime::ApplyResult& result,
    const char* message)
{
    Check(!result.ok && !result.usedFallback &&
            result.executable.empty() && !result.error.empty(),
        message);
}

void TestOccupiedSameBuildRecovery(const std::filesystem::path& root)
{
    constexpr std::string_view buildId = "occupied-build";
    const auto initial = PrepareRuntime(
        root, buildId, "host stable", "dll stable");
    if (!initial.ok)
        return;

    const auto oldRuntime = initial.executable.parent_path();
    const auto oldLibrary = oldRuntime / L"SnowDesktop.Runtime" /
        L"runtime.dll";
    WriteText(oldRuntime / kRuntimeCompleteFilename,
        "damaged completion marker\n");
    const DirectorySnapshot oldSnapshot = SnapshotDirectory(oldRuntime);

    OccupiedFile occupiedExecutable(initial.executable);
    OccupiedFile occupiedLibrary(oldLibrary);
    Check(occupiedExecutable.valid() && occupiedLibrary.valid(),
        "the prior same-build executable and DLL are held without delete sharing");
    if (!occupiedExecutable.valid() || !occupiedLibrary.valid())
        return;

    const auto recovered =
        snowdesktop::steam_runtime::ApplyDistribution(root);
    if (!recovered.ok)
        std::cerr << "occupied recovery error: " << recovered.error << '\n';
    Check(recovered.ok && !recovered.usedFallback &&
            std::string_view(recovered.buildId) == buildId,
        "a damaged same-build marker publishes a fresh runtime while the old runtime is occupied");
    if (!recovered.ok)
        return;

    const auto recoveredRuntime = recovered.executable.parent_path();
    Check(recoveredRuntime != oldRuntime &&
            recoveredRuntime.parent_path() == oldRuntime.parent_path(),
        "same-build recovery publishes to a distinct sibling runtime directory");
    Check(std::filesystem::is_directory(oldRuntime) &&
            SnapshotDirectory(oldRuntime) == oldSnapshot,
        "same-build recovery never deletes or modifies the occupied old runtime");
    Check(ReadText(recovered.executable) == "host stable" &&
            ReadText(recoveredRuntime / L"SnowDesktop.Runtime" /
                L"runtime.dll") == "dll stable",
        "the recovery runtime contains the complete validated distribution");
    Check(ReadText(root / L".snowdesktop" / kCurrentRuntimeFilename) ==
            recoveredRuntime.filename().string() + "\n",
        "the active runtime pointer records the physical recovery directory ID");
    const auto context =
        snowdesktop::deployment::ResolveRuntimeDeploymentContext(
            recovered.executable, false);
    Check(context.kind ==
            snowdesktop::deployment::RuntimeDeploymentKind::SteamManaged,
        "the recovery runtime publishes a valid managed Steam sidecar");
}

void TestTamperedActiveRuntimeBypassesFastPath(
    const std::filesystem::path& root)
{
    constexpr std::string_view buildId = "tampered-active";
    const auto initial = PrepareRuntime(root, buildId);
    if (!initial.ok)
        return;

    WriteText(initial.executable, "host evil!");
    Check(ReadText(initial.executable).size() ==
            std::string_view("host clean").size(),
        "the active-runtime tamper preserves file size to require hash validation");
    const auto repaired =
        snowdesktop::steam_runtime::ApplyDistribution(root);
    if (!repaired.ok)
        std::cerr << "tampered active recovery error: " << repaired.error << '\n';
    Check(repaired.ok && !repaired.usedFallback &&
            std::string_view(repaired.buildId) == buildId &&
            ReadText(repaired.executable) == "host clean",
        "a valid completion marker cannot fast-path a hash-tampered active runtime");
}

void TestInvalidRuntimeFallbacks(const std::filesystem::path& root)
{
    const auto verifyRejected = [&](const std::filesystem::path& caseRoot,
                                    std::string_view buildId,
                                    const auto& corruptRuntime,
                                    const char* message) {
        const auto initial = PrepareRuntime(caseRoot, buildId);
        if (!initial.ok)
            return;
        const auto runtime = initial.executable.parent_path();
        corruptRuntime(runtime, initial.executable);
        CorruptDistributionLibrary(caseRoot);
        const auto fallback =
            snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
        Check(!fallback.ok && !fallback.usedFallback &&
                fallback.executable.empty() && !fallback.error.empty(),
            message);
    };

    verifyRejected(root / L"marker", "fallback-marker",
        [](const std::filesystem::path& runtime,
            const std::filesystem::path&) {
            WriteText(runtime / kRuntimeCompleteFilename,
                "damaged completion marker\n");
        },
        "fallback rejects a runtime with an invalid completion marker");

    verifyRejected(root / L"hash", "fallback-hash",
        [](const std::filesystem::path&,
            const std::filesystem::path& executable) {
            WriteText(executable, "host evil!");
        },
        "fallback rejects a runtime whose payload hash is invalid");

    verifyRejected(root / L"sidecar", "fallback-sidecar",
        [](const std::filesystem::path& runtime,
            const std::filesystem::path&) {
            WriteText(runtime /
                    snowdesktop::deployment::kSteamRuntimeContextFilename,
                "{not-json");
        },
        "fallback rejects a runtime whose managed sidecar is invalid");
}

void TestRuntimeDirectoryIdValidation(const std::filesystem::path& root)
{
    constexpr std::string_view buildId = "directory-id";
    const auto initial = PrepareRuntime(root, buildId);
    if (!initial.ok)
        return;

    CorruptDistributionManifest(root);
    const auto logicalFallback =
        snowdesktop::steam_runtime::ApplyDistribution(root);
    Check(logicalFallback.ok && logicalFallback.usedFallback &&
            logicalFallback.executable == initial.executable &&
            std::string_view(logicalFallback.buildId) == buildId,
        "fallback accepts a runtime directory whose physical ID is the logical build ID");

    WriteDistribution(root, "1.0.5.0", buildId,
        "host clean", "dll clean");
    WriteText(initial.executable.parent_path() / kRuntimeCompleteFilename,
        "damaged completion marker\n");
    const auto recovered =
        snowdesktop::steam_runtime::ApplyDistribution(root);
    Check(recovered.ok && !recovered.usedFallback &&
            recovered.executable.parent_path() !=
                initial.executable.parent_path() &&
            recovered.executable.parent_path().filename().wstring().starts_with(
                L"recovery-") &&
            std::string_view(recovered.buildId) == buildId,
        "same-build recovery publishes an allowed recovery-digest directory ID");
    if (!recovered.ok)
        return;

    CorruptDistributionManifest(root);
    const auto recoveryFallback =
        snowdesktop::steam_runtime::ApplyDistribution(root);
    Check(recoveryFallback.ok && recoveryFallback.usedFallback &&
            recoveryFallback.executable == recovered.executable &&
            std::string_view(recoveryFallback.buildId) == buildId,
        "fallback accepts a validated recovery-digest directory ID");

    const auto runtimeRoot = recovered.executable.parent_path().parent_path();
    std::filesystem::path currentRuntime = recovered.executable.parent_path();
    const std::wstring recoveryBase = currentRuntime.filename().wstring();
    const auto verifyRecoverySuffix = [&](std::wstring_view suffix,
                                          bool accepted,
                                          const char* message) {
        const std::filesystem::path renamed = runtimeRoot /
            (recoveryBase + L"-" + std::wstring(suffix));
        std::error_code renameError;
        std::filesystem::rename(currentRuntime, renamed, renameError);
        Check(!renameError,
            "the recovery runtime can be renamed for numeric suffix validation");
        if (renameError)
            return;
        currentRuntime = renamed;
        WriteText(root / L".snowdesktop" / kCurrentRuntimeFilename,
            currentRuntime.filename().string() + "\n");
        const auto fallback =
            snowdesktop::steam_runtime::ApplyDistribution(root);
        if (accepted)
        {
            Check(fallback.ok && fallback.usedFallback &&
                    fallback.executable.parent_path() == currentRuntime,
                message);
        }
        else
        {
            CheckFallbackRejected(fallback, message);
        }
    };

    verifyRecoverySuffix(L"18446744073709551615", true,
        "fallback accepts the maximum uint64 recovery attempt suffix");
    verifyRecoverySuffix(L"18446744073709551616", false,
        "fallback rejects a recovery attempt suffix above uint64");
    verifyRecoverySuffix(L"01", false,
        "fallback rejects a recovery attempt suffix with a leading zero");

    const auto arbitraryRuntime = runtimeRoot / L"arbitrary-runtime-copy";
    std::error_code renameError;
    std::filesystem::rename(currentRuntime, arbitraryRuntime, renameError);
    Check(!renameError,
        "the valid recovery runtime can be renamed for the directory-ID rejection fixture");
    if (renameError)
        return;
    WriteText(root / L".snowdesktop" / kCurrentRuntimeFilename,
        "arbitrary-runtime-copy\n");
    const auto arbitraryFallback =
        snowdesktop::steam_runtime::ApplyDistribution(root);
    CheckFallbackRejected(arbitraryFallback,
        "fallback rejects an otherwise valid runtime under an arbitrary renamed directory ID");
}

void TestRuntimeReparseFallbacks(const std::filesystem::path& root)
{
    const auto verifyRejected = [](const std::filesystem::path& caseRoot,
                                    const char* message) {
        CorruptDistributionManifest(caseRoot);
        CheckFallbackRejected(
            snowdesktop::steam_runtime::ApplyDistribution(caseRoot),
            message);
    };

    {
        const auto caseRoot = root / L"runtime-root";
        const auto initial = PrepareRuntime(caseRoot, "reparse-root");
        if (initial.ok)
        {
            const auto runtime = initial.executable.parent_path();
            const auto backing = caseRoot / L"runtime-root-backing";
            std::error_code error;
            std::filesystem::rename(runtime, backing, error);
            Check(!error,
                "the runtime-root reparse fixture can move its backing directory");
            if (!error)
            {
                if (CreateDirectoryJunction(runtime, backing))
                {
                    verifyRejected(caseRoot,
                        "fallback rejects a runtime whose root is a directory junction");
                    RemoveDirectoryW(runtime.c_str());
                }
                else
                {
                    Skip("runtime-root reparse test could not create a directory junction",
                        GetLastError());
                    std::filesystem::rename(backing, runtime, error);
                }
            }
        }
    }

    {
        const auto caseRoot = root / L"payload-parent";
        const auto initial = PrepareRuntime(caseRoot, "reparse-parent");
        if (initial.ok)
        {
            const auto runtime = initial.executable.parent_path();
            const auto parent = runtime / L"SnowDesktop.Runtime";
            const auto backing = caseRoot / L"payload-parent-backing";
            std::error_code error;
            std::filesystem::rename(parent, backing, error);
            Check(!error,
                "the payload-parent reparse fixture can move its backing directory");
            if (!error)
            {
                if (CreateDirectoryJunction(parent, backing))
                {
                    verifyRejected(caseRoot,
                        "fallback rejects a runtime whose payload parent is a directory junction");
                    RemoveDirectoryW(parent.c_str());
                }
                else
                {
                    Skip("payload-parent reparse test could not create a directory junction",
                        GetLastError());
                    std::filesystem::rename(backing, parent, error);
                }
            }
        }
    }

    {
        const auto caseRoot = root / L"internal-metadata";
        const auto initial = PrepareRuntime(caseRoot, "reparse-metadata");
        if (initial.ok)
        {
            const auto runtime = initial.executable.parent_path();
            const auto metadata = runtime / kRuntimeManifestFilename;
            const auto contents = ReadText(metadata);
            const auto external = caseRoot / L"external-runtime-manifest.json";
            WriteText(external, contents);
            std::filesystem::remove(metadata);
            DWORD error = ERROR_SUCCESS;
            if (CreateFileSymlink(metadata, external, error))
            {
                verifyRejected(caseRoot,
                    "fallback rejects symlinked internal runtime metadata");
                DeleteFileW(metadata.c_str());
            }
            else
            {
                Skip("internal-metadata reparse test could not create a file symlink",
                    error);
                WriteText(metadata, contents);
            }
        }
    }

    {
        const auto caseRoot = root / L"payload-file";
        const auto initial = PrepareRuntime(caseRoot, "reparse-file");
        if (initial.ok)
        {
            const auto runtime = initial.executable.parent_path();
            const auto payload = runtime / L"SnowDesktop.Runtime" /
                L"runtime.dll";
            const auto contents = ReadText(payload);
            const auto external = caseRoot / L"external-runtime.dll";
            WriteText(external, contents);
            std::filesystem::remove(payload);
            DWORD error = ERROR_SUCCESS;
            if (CreateFileSymlink(payload, external, error))
            {
                verifyRejected(caseRoot,
                    "fallback rejects a symlinked runtime payload file");
                DeleteFileW(payload.c_str());
            }
            else
            {
                Skip("payload-file reparse test could not create a file symlink",
                    error);
                WriteText(payload, contents);
            }
        }
    }
}

void TestManagedRootReparseBoundaries(const std::filesystem::path& root)
{
    const auto verifyRejected = [&](std::wstring_view caseName,
                                    const std::filesystem::path& relativeLink,
                                    bool moveExisting,
                                    const char* message) {
        const auto caseRoot = root / caseName;
        WriteText(caseRoot /
                snowdesktop::deployment::kSteamLauncherFilename,
            "launcher");
        WriteDistribution(caseRoot, "1.0.5.0", "ancestor-boundary",
            "host boundary", "dll boundary");

        const auto link = caseRoot / relativeLink;
        const auto backing = root / L"external-backing" /
            std::filesystem::path(caseName);
        std::error_code fileError;
        if (moveExisting)
        {
            std::filesystem::create_directories(backing.parent_path());
            std::filesystem::rename(link, backing, fileError);
            Check(!fileError,
                "the managed-root boundary fixture can move its backing directory");
            if (fileError)
                return;
        }
        else
        {
            std::filesystem::create_directories(backing);
        }
        WriteText(backing / L"outside-sentinel.txt", "outside unchanged");
        const DirectorySnapshot before = SnapshotDirectory(backing);
        const bool created = CreateDirectoryJunction(link, backing);
        Check(created,
            "managed-root ancestor junction fixtures must be available");
        if (!created)
            return;

        const auto result =
            snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
        CheckFallbackRejected(result, message);
        Check(SnapshotDirectory(backing) == before,
            "a rejected managed-root junction leaves its outside backing tree unchanged");
        Check(RemoveDirectoryW(link.c_str()) != FALSE,
            "the managed-root junction fixture is detached without traversing it");
    };

    verifyRejected(L"state-root", L".snowdesktop", false,
        "Steam apply rejects a .snowdesktop ancestor junction");
    verifyRejected(L"runtime-root", L".snowdesktop/runtime", false,
        "Steam apply rejects a .snowdesktop/runtime ancestor junction");
    verifyRejected(L"data-root", L"data", false,
        "Steam apply rejects a data directory junction");
    verifyRejected(L"distribution-root", L"distribution", true,
        "Steam apply rejects a distribution directory junction");

    {
        const auto caseRoot = root / L"data-root-with-fallback";
        const auto initial = PrepareRuntime(
            caseRoot, "data-junction-fallback");
        if (initial.ok)
        {
            const auto data = caseRoot / L"data";
            WriteText(data / L"user-data.txt", "user data unchanged");
            const auto backing = root / L"external-backing" /
                L"data-root-with-fallback";
            std::error_code fileError;
            std::filesystem::create_directories(backing.parent_path());
            std::filesystem::rename(data, backing, fileError);
            Check(!fileError,
                "the established data-root junction fixture can move its backing directory");
            if (!fileError)
            {
                const DirectorySnapshot before = SnapshotDirectory(backing);
                const bool created = CreateDirectoryJunction(data, backing);
                Check(created,
                    "the established data-root junction fixture must be available");
                if (created)
                {
                    const auto result =
                        snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
                    CheckFallbackRejected(result,
                        "a data junction fails closed even when a valid runtime fallback exists");
                    Check(SnapshotDirectory(backing) == before,
                        "a rejected data junction with fallback leaves outside user data unchanged");
                    Check(RemoveDirectoryW(data.c_str()) != FALSE,
                        "the established data-root junction is detached without traversing it");
                }
            }
        }
    }
}

void TestAtomicStateTemporaryIsolation(const std::filesystem::path& root)
{
    const auto prepare = [](const std::filesystem::path& caseRoot,
                            std::string_view buildId) {
        WriteText(caseRoot /
                snowdesktop::deployment::kSteamLauncherFilename,
            "launcher");
        WriteDistribution(caseRoot, "1.0.5.0", buildId,
            "host atomic", "dll atomic");
        std::filesystem::create_directories(
            caseRoot / L".snowdesktop" / L"runtime");
    };
    const auto legacyTemporary = [](const std::filesystem::path& caseRoot) {
        return caseRoot / L".snowdesktop" /
            (std::wstring(kCurrentRuntimeFilename) + L".tmp." +
                std::to_wstring(GetCurrentProcessId()));
    };

    {
        const auto caseRoot = root / L"ordinary-precreated";
        prepare(caseRoot, "atomic-ordinary");
        const auto precreated = legacyTemporary(caseRoot);
        WriteText(precreated, "precreated temporary must not be reused");
        const auto result =
            snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
        Check(result.ok && !result.usedFallback,
            "a precreated legacy temporary filename cannot block Steam activation");
        Check(ReadText(precreated) ==
                "precreated temporary must not be reused",
            "atomic state publication never truncates or reuses a precreated temporary file");
    }

    {
        const auto caseRoot = root / L"hardlink-precreated";
        prepare(caseRoot, "atomic-hardlink");
        const auto sentinel = root / L"outside-hardlink-sentinel.txt";
        WriteText(sentinel, "outside hardlink sentinel unchanged");
        const auto precreated = legacyTemporary(caseRoot);
        const bool linked = CreateFileHardLink(precreated, sentinel);
        Check(linked,
            "the hardlink temporary-file regression fixture must be available");
        if (linked)
        {
            const auto result =
                snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
            Check(result.ok && !result.usedFallback,
                "a precreated hardlink at the legacy temporary name cannot block activation");
            Check(ReadText(sentinel) ==
                    "outside hardlink sentinel unchanged" &&
                    ReadText(precreated) ==
                    "outside hardlink sentinel unchanged",
                "atomic state publication never follows or truncates a precreated hardlink");
            const auto pointer = caseRoot / L".snowdesktop" /
                kCurrentRuntimeFilename;
            Check(!IsReparsePoint(pointer) &&
                    ReadText(pointer) == "atomic-hardlink\n",
                "the final active-runtime pointer is a new plain state file");
        }
    }

    {
        const auto caseRoot = root / L"symlink-precreated";
        prepare(caseRoot, "atomic-symlink");
        const auto sentinel = root / L"outside-symlink-sentinel.txt";
        WriteText(sentinel, "outside symlink sentinel unchanged");
        const auto precreated = legacyTemporary(caseRoot);
        DWORD linkError = ERROR_SUCCESS;
        if (CreateFileSymlink(precreated, sentinel, linkError))
        {
            const auto result =
                snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
            Check(result.ok && !result.usedFallback &&
                    ReadText(sentinel) ==
                    "outside symlink sentinel unchanged" &&
                    IsReparsePoint(precreated),
                "atomic state publication never follows or replaces a precreated symlink");
        }
        else
        {
            Skip("temporary-file symlink regression fixture could not be created",
                linkError);
        }
    }
}

void TestReadOnlyPayloadAndStagingCleanup(
    const std::filesystem::path& root)
{
    {
        const auto caseRoot = root / L"readonly-payload";
        WriteText(caseRoot /
                snowdesktop::deployment::kSteamLauncherFilename,
            "launcher");
        WriteDistribution(caseRoot, "1.0.5.0", "readonly-payload",
            "host readonly", "dll readonly");
        const auto source = caseRoot / L"distribution" /
            L"SnowDesktop.Runtime" / L"runtime.dll";
        const DWORD sourceAttributes = GetFileAttributesW(source.c_str());
        Check(sourceAttributes != INVALID_FILE_ATTRIBUTES &&
                SetFileAttributesW(source.c_str(),
                    sourceAttributes | FILE_ATTRIBUTE_READONLY) != FALSE,
            "the read-only depot payload fixture can mark its source file read-only");

        const auto result =
            snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
        Check(result.ok && !result.usedFallback,
            "a read-only Steam depot payload is copied, flushed, and activated");
        if (result.ok)
        {
            const auto published = result.executable.parent_path() /
                L"SnowDesktop.Runtime" / L"runtime.dll";
            const DWORD publishedAttributes =
                GetFileAttributesW(published.c_str());
            Check(publishedAttributes != INVALID_FILE_ATTRIBUTES &&
                    (publishedAttributes & FILE_ATTRIBUTE_READONLY) == 0,
                "the immutable runtime copy is normalized to a flushable plain file");
        }

        const auto runtimeRoot = caseRoot / L".snowdesktop" / L"runtime";
        bool hasStaging = false;
        for (const auto& entry : std::filesystem::directory_iterator(runtimeRoot))
        {
            if (entry.path().filename().wstring().starts_with(L".staging."))
                hasStaging = true;
        }
        Check(!hasStaging,
            "a read-only payload activation leaves no staged runtime residue");

        if (sourceAttributes != INVALID_FILE_ATTRIBUTES)
            SetFileAttributesW(source.c_str(), sourceAttributes);
    }

    {
        const auto caseRoot = root / L"abandoned-staging";
        const auto initial = PrepareRuntime(caseRoot, "cleanup-staging");
        if (!initial.ok)
            return;
        const auto runtimeRoot =
            initial.executable.parent_path().parent_path();
        const auto abandoned = runtimeRoot / L".staging.123.456";
        const auto readOnly = abandoned / L"read-only-partial.dll";
        WriteText(readOnly, "partial staged payload");
        const DWORD attributes = GetFileAttributesW(readOnly.c_str());
        Check(attributes != INVALID_FILE_ATTRIBUTES &&
                SetFileAttributesW(readOnly.c_str(),
                    attributes | FILE_ATTRIBUTE_READONLY) != FALSE,
            "the abandoned staging fixture can contain a read-only partial file");

        const auto relaunched =
            snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
        Check(relaunched.ok && !relaunched.usedFallback &&
                !std::filesystem::exists(abandoned),
            "the next locked launcher run safely removes abandoned read-only staging data");
    }

    {
        const auto caseRoot = root / L"hardlinked-staging";
        const auto initial = PrepareRuntime(caseRoot, "hardlink-staging");
        if (!initial.ok)
            return;
        const auto runtimeRoot =
            initial.executable.parent_path().parent_path();
        const auto abandoned = runtimeRoot / L".staging.123.789";
        const auto sentinel = root / L"outside-read-only-sentinel.dll";
        WriteText(sentinel, "outside read-only sentinel unchanged");
        const DWORD originalAttributes =
            GetFileAttributesW(sentinel.c_str());
        const bool markedReadOnly =
            originalAttributes != INVALID_FILE_ATTRIBUTES &&
            SetFileAttributesW(sentinel.c_str(),
                originalAttributes | FILE_ATTRIBUTE_READONLY) != FALSE;
        Check(markedReadOnly,
            "the staging hardlink fixture can mark its outside sentinel read-only");
        const auto stagedHardlink = abandoned / L"linked-partial.dll";
        const bool linked = markedReadOnly &&
            CreateFileHardLink(stagedHardlink, sentinel);
        Check(linked,
            "the staging hardlink fixture can link a partial file outside the runtime root");
        if (linked)
        {
            const auto relaunched =
                snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
            const DWORD finalAttributes =
                GetFileAttributesW(sentinel.c_str());
            Check(relaunched.ok && relaunched.usedFallback &&
                    relaunched.executable == initial.executable &&
                    relaunched.error.find(
                        "cannot clean abandoned Steam staging data") !=
                        std::string::npos,
                "a read-only staged hardlink is rejected with the last valid runtime as fallback");
            Check(std::filesystem::exists(abandoned) &&
                    ReadText(sentinel) ==
                        "outside read-only sentinel unchanged" &&
                    finalAttributes != INVALID_FILE_ATTRIBUTES &&
                    (finalAttributes & FILE_ATTRIBUTE_READONLY) != 0,
                "rejected staging cleanup leaves the outside hardlink target content and attributes unchanged");
        }
        if (originalAttributes != INVALID_FILE_ATTRIBUTES)
            SetFileAttributesW(sentinel.c_str(), originalAttributes);
    }

    {
        const auto caseRoot = root / L"noncanonical-staging";
        const auto initial = PrepareRuntime(caseRoot, "canonical-staging");
        if (!initial.ok)
            return;
        const auto runtimeRoot =
            initial.executable.parent_path().parent_path();
        const auto leadingZero = runtimeRoot / L".staging.01.2";
        const auto overflow = runtimeRoot /
            L".staging.4294967296.18446744073709551616";
        WriteText(leadingZero / L"unowned.txt", "leading zero");
        WriteText(overflow / L"unowned.txt", "overflow");

        const auto relaunched =
            snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
        Check(relaunched.ok && !relaunched.usedFallback,
            "noncanonical staging-like names do not block a normal launcher run");
        Check(ReadText(leadingZero / L"unowned.txt") == "leading zero" &&
                ReadText(overflow / L"unowned.txt") == "overflow",
            "cleanup retains staging-like directories that the launcher generator cannot produce");
    }
}

void TestCaseExactRuntimeTree(const std::filesystem::path& root)
{
    const auto initial = PrepareRuntime(root, "case-exact-tree");
    if (!initial.ok)
        return;
    const auto directory = initial.executable.parent_path() /
        L"SnowDesktop.Runtime";
    const auto original = directory / L"runtime.dll";
    const auto temporary = directory / L"case-rename.tmp";
    const auto upper = directory / L"RUNTIME.DLL";
    std::error_code renameError;
    std::filesystem::rename(original, temporary, renameError);
    if (!renameError)
        std::filesystem::rename(temporary, upper, renameError);
    Check(!renameError,
        "the exact-tree fixture can change a payload filename only by case");
    if (renameError)
        return;

    bool observedUpperCase = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        if (entry.path().filename() == L"RUNTIME.DLL")
            observedUpperCase = true;
    }
    Check(observedUpperCase,
        "the exact-tree fixture preserves the case-only renamed directory entry");
    CorruptDistributionManifest(root);
    CheckFallbackRejected(
        snowdesktop::steam_runtime::ApplyDistribution(root),
        "fallback rejects a hash-identical payload whose filename casing differs from the manifest");
}

void TestManifestSizeBoundaries(const std::filesystem::path& root)
{
    const auto verifyRejected = [&](std::wstring_view caseName,
                                    std::string_view sizeToken,
                                    const char* message) {
        const auto caseRoot = root / caseName;
        WriteText(caseRoot /
                snowdesktop::deployment::kSteamLauncherFilename,
            "launcher");
        WriteDistribution(caseRoot, "1.0.5.0", "manifest-size",
            "host size", "dll size");
        const auto manifestPath = caseRoot /
            snowdesktop::steam_runtime::kDistributionManifestFilename;
        std::string manifest = ReadText(manifestPath);
        const std::size_t begin = manifest.find("\"size\":");
        const std::size_t valueBegin = begin == std::string::npos ?
            begin : begin + std::string_view("\"size\":").size();
        const std::size_t end = valueBegin == std::string::npos ?
            valueBegin : manifest.find(',', valueBegin);
        Check(begin != std::string::npos && end != std::string::npos,
            "the manifest-size fixture can locate its first size token");
        if (begin == std::string::npos || end == std::string::npos)
            return;
        manifest.replace(valueBegin, end - valueBegin, sizeToken);
        WriteText(manifestPath, manifest);
        const auto result =
            snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
        CheckFallbackRejected(result, message);
        Check(result.error.find(
                "distribution manifest contains an invalid file entry") !=
                std::string::npos,
            "out-of-range manifest sizes are rejected by parsing before payload validation");
    };

    verifyRejected(L"beyond-exact-json", "9007199254740992",
        "manifest file sizes beyond exact JSON integer range are rejected");
    verifyRejected(L"uint64-overflow", "18446744073709551616",
        "manifest file sizes at 2^64 are rejected without conversion overflow");
}

void TestCompletionMarkerIsFinalFence()
{
    const auto sourcePath =
        FindRepositoryFile(L"src/steam_runtime_manager.cpp");
    Check(!sourcePath.empty(),
        "the runtime manager source is available for the publish-order contract");
    if (sourcePath.empty())
        return;

    const std::string source = ReadText(sourcePath);
    const auto payloadValidation =
        source.find("!ValidateFile(fileDestination, file, error)");
    const auto internalManifest = source.find(
        "WriteTextAtomically(staging / kRuntimeManifestFilename",
        payloadValidation);
    const auto sidecar = source.find(
        "snowdesktop::deployment::kSteamRuntimeContextFilename",
        internalManifest);
    const auto completionMarker = source.find(
        "WriteTextAtomically(staging / kCompleteFilename", sidecar);
    const auto stagedValidation = source.find(
        "ValidatePublishedRuntime(staging, &*manifest", completionMarker);
    const auto publish = source.find(
        "MoveFileExW(staging.c_str(), destination.path.c_str()",
        stagedValidation);
    Check(payloadValidation != std::string::npos &&
            internalManifest != std::string::npos &&
            sidecar != std::string::npos &&
            completionMarker != std::string::npos &&
            stagedValidation != std::string::npos &&
            publish != std::string::npos &&
            payloadValidation < internalManifest &&
            internalManifest < sidecar && sidecar < completionMarker &&
            completionMarker < stagedValidation &&
            stagedValidation < publish,
        "the completion marker is the final staged write after payload validation and before validation and publish");

    const auto publishedValidation = source.find(
        "std::optional<DistributionManifest> ValidatePublishedRuntime");
    const auto publishedSidecar = source.find(
        "const std::string sidecar = ReadFile", publishedValidation);
    const auto publishedPayload = source.find(
        "for (const DistributionFile& file : manifest->files)",
        publishedValidation);
    const auto publishedMarker = source.find(
        "const std::string marker = ReadFile", publishedValidation);
    const auto publishedValidationEnd = source.find(
        "struct RuntimeDestination", publishedValidation);
    Check(publishedValidation != std::string::npos &&
            publishedSidecar != std::string::npos &&
            publishedPayload != std::string::npos &&
            publishedMarker != std::string::npos &&
            publishedValidationEnd != std::string::npos &&
            publishedValidation < publishedSidecar &&
            publishedSidecar < publishedPayload &&
            publishedPayload < publishedMarker &&
            publishedMarker < publishedValidationEnd,
        "published runtime validation reads the completion marker only after sidecar and payload hashes pass");
}

void TestUnexpectedRuntimeEntries(const std::filesystem::path& root)
{
    const auto verifyRejected = [&](const std::filesystem::path& caseRoot,
                                    std::string_view buildId,
                                    const std::filesystem::path& relativePath,
                                    bool directory,
                                    const char* fastPathMessage,
                                    const char* fallbackMessage) {
        const auto addUnexpected = [&](const std::filesystem::path& runtime) {
            if (directory)
                std::filesystem::create_directories(runtime / relativePath);
            else
                WriteText(runtime / relativePath, "unexpected");
        };

        const auto initial = PrepareRuntime(caseRoot, buildId);
        if (!initial.ok)
            return;
        addUnexpected(initial.executable.parent_path());
        const auto repaired =
            snowdesktop::steam_runtime::ApplyDistribution(caseRoot);
        Check(repaired.ok && !repaired.usedFallback &&
                repaired.executable.parent_path() !=
                    initial.executable.parent_path() &&
                !std::filesystem::exists(
                    repaired.executable.parent_path() / relativePath),
            fastPathMessage);
        if (!repaired.ok)
            return;

        addUnexpected(repaired.executable.parent_path());
        CorruptDistributionManifest(caseRoot);
        CheckFallbackRejected(
            snowdesktop::steam_runtime::ApplyDistribution(caseRoot),
            fallbackMessage);
    };

    verifyRejected(root / L"ordinary-file", "unexpected-file",
        L"unexpected.txt", false,
        "fast path rejects a runtime containing an unexpected ordinary file",
        "fallback rejects a runtime containing an unexpected ordinary file");
    verifyRejected(root / L"library", "unexpected-library",
        L"SnowDesktop.Runtime/unexpected.dll", false,
        "fast path rejects a runtime containing an unexpected DLL",
        "fallback rejects a runtime containing an unexpected DLL");
    verifyRejected(root / L"directory", "unexpected-directory",
        L"unexpected-directory", true,
        "fast path rejects a runtime containing an unexpected directory",
        "fallback rejects a runtime containing an unexpected directory");
    verifyRejected(root / L"legacy-imgui", "legacy-imgui-runtime",
        L"imgui.ini", false,
        "fast path still rejects a runtime containing legacy ImGui settings",
        "fallback still rejects a runtime containing legacy ImGui settings");
    verifyRejected(root / L"legacy-data", "legacy-data-runtime",
        L"data", true,
        "fast path still rejects a runtime containing a legacy data directory",
        "fallback still rejects a runtime containing a legacy data directory");
}

void TestLegacyDistributionWrites(const std::filesystem::path& root)
{
    WriteDistribution(root, "1.0.5.0", "legacy-distribution-writes",
        "host clean", "dll clean");
    WriteText(root / L"distribution" / L"data" /
        L"SteamWorkshopManager" / L"projects.json", "legacy user data");
    WriteText(root / L"distribution" / L"imgui.ini", "legacy settings");
    WriteText(root / L"distribution" / L"unknown.dll", "unlisted library");
    WriteText(root / L"distribution" / L"unexplained" /
        L"nested.bin", "unlisted nested file");

    const auto result =
        snowdesktop::steam_runtime::ApplyDistribution(root);
    Check(result.ok && !result.usedFallback &&
            !std::filesystem::exists(
                result.executable.parent_path() / L"data") &&
            !std::filesystem::exists(
                result.executable.parent_path() / L"imgui.ini") &&
            !std::filesystem::exists(
                result.executable.parent_path() / L"unknown.dll") &&
            !std::filesystem::exists(
                result.executable.parent_path() / L"unexplained"),
        "unlisted distribution entries do not block or enter a clean immutable runtime");
    Check(ReadText(root / L"distribution" / L"data" /
                L"SteamWorkshopManager" / L"projects.json") ==
                "legacy user data" &&
            ReadText(root / L"distribution" / L"imgui.ini") ==
                "legacy settings" &&
            ReadText(root / L"distribution" / L"unknown.dll") ==
                "unlisted library" &&
            ReadText(root / L"distribution" / L"unexplained" /
                L"nested.bin") == "unlisted nested file",
        "unlisted distribution entries remain untouched while the runtime is repaired");
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
        TestInactiveRuntimePruning(root / L"pruning");
        TestOccupiedSameBuildRecovery(root / L"occupied-recovery");
        TestTamperedActiveRuntimeBypassesFastPath(root / L"tampered-active");
        TestInvalidRuntimeFallbacks(root / L"invalid-fallbacks");
        TestRuntimeDirectoryIdValidation(root / L"directory-id-validation");
        TestRuntimeReparseFallbacks(root / L"reparse-fallbacks");
        TestManagedRootReparseBoundaries(root / L"managed-root-boundaries");
        TestAtomicStateTemporaryIsolation(root / L"atomic-state");
        TestReadOnlyPayloadAndStagingCleanup(
            root / L"readonly-and-staging-cleanup");
        TestCaseExactRuntimeTree(root / L"case-exact-tree");
        TestManifestSizeBoundaries(root / L"manifest-size-boundaries");
        TestCompletionMarkerIsFinalFence();
        TestUnexpectedRuntimeEntries(root / L"unexpected-runtime-entries");
        TestLegacyDistributionWrites(root / L"legacy-distribution-writes");
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
    if (skips != 0)
        std::cout << skips << " Steam runtime reparse check(s) skipped\n";
    std::cout << "Steam runtime update checks passed\n";
    return 0;
}
