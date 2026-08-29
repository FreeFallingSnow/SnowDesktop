#include "steam_runtime_manager.h"

#include "json_value.h"
#include "steam_runtime_context.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using snowdesktop::steam_runtime::ApplyResult;

constexpr wchar_t kDistributionDirectory[] = L"distribution";
constexpr wchar_t kStateDirectory[] = L".snowdesktop";
constexpr wchar_t kRuntimeDirectory[] = L"runtime";
constexpr wchar_t kCurrentRuntimeFilename[] = L"current-runtime.txt";
constexpr wchar_t kCompleteFilename[] = L".snowdesktop-runtime-complete";
constexpr wchar_t kUpdateLockFilename[] = L"update.lock";
constexpr std::uint64_t kMaximumManifestBytes = 16ULL * 1024ULL * 1024ULL;

struct DistributionFile
{
    std::filesystem::path relativePath;
    std::uint64_t size = 0;
    std::string sha256;
};

struct DistributionManifest
{
    std::string version;
    std::string buildId;
    std::vector<DistributionFile> files;
    std::string digest;
};

class ExclusiveFile final
{
public:
    ExclusiveFile() = default;
    explicit ExclusiveFile(HANDLE value) noexcept : value_(value) {}
    ~ExclusiveFile() noexcept
    {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    ExclusiveFile(const ExclusiveFile&) = delete;
    ExclusiveFile& operator=(const ExclusiveFile&) = delete;
    ExclusiveFile(ExclusiveFile&& other) noexcept : value_(other.value_)
    {
        other.value_ = INVALID_HANDLE_VALUE;
    }
    ExclusiveFile& operator=(ExclusiveFile&& other) noexcept
    {
        if (this == &other)
            return *this;
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
        value_ = other.value_;
        other.value_ = INVALID_HANDLE_VALUE;
        return *this;
    }
    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

bool IsSafeIdentifier(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 96)
        return false;
    for (const unsigned char character : value)
    {
        if (!std::isalnum(character) && character != '-' &&
            character != '_' && character != '.')
        {
            return false;
        }
    }
    return true;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty())
        return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), result.data(),
            required) != required)
    {
        return {};
    }
    return result;
}

std::string ReadFile(const std::filesystem::path& path,
    std::uint64_t maximumBytes, std::string& error)
{
    std::error_code fileError;
    const std::uint64_t size = std::filesystem::file_size(path, fileError);
    if (fileError || size > maximumBytes)
    {
        error = "file is missing or exceeds its size limit: " +
            path.string();
        return {};
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "cannot open file: " + path.string();
        return {};
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (size != 0 && !stream.read(contents.data(),
            static_cast<std::streamsize>(contents.size())))
    {
        error = "cannot read file: " + path.string();
        return {};
    }
    return contents;
}

std::optional<std::string> Sha256(const std::filesystem::path& path,
    std::string& error)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD hashSize = 0;
    DWORD written = 0;
    std::vector<UCHAR> object;
    std::vector<UCHAR> digest;
    auto cleanup = [&] {
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &written, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
            &written, 0) < 0)
    {
        cleanup();
        error = "cannot initialize SHA-256";
        return std::nullopt;
    }
    object.resize(objectSize);
    digest.resize(hashSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
            nullptr, 0, 0) < 0)
    {
        cleanup();
        error = "cannot create SHA-256 state";
        return std::nullopt;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        cleanup();
        error = "cannot hash file: " + path.string();
        return std::nullopt;
    }
    std::array<char, 1024 * 1024> buffer{};
    while (stream)
    {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        if (count > 0 && BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count), 0) < 0)
        {
            cleanup();
            error = "cannot update SHA-256 for: " + path.string();
            return std::nullopt;
        }
    }
    if (!stream.eof() || BCryptFinishHash(
            hash, digest.data(), hashSize, 0) < 0)
    {
        cleanup();
        error = "cannot finish SHA-256 for: " + path.string();
        return std::nullopt;
    }
    cleanup();

    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (const UCHAR byte : digest)
        text << std::setw(2) << static_cast<unsigned>(byte);
    return text.str();
}

bool IsLowerHexSha256(std::string_view value) noexcept
{
    if (value.size() != 64)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char value) {
        return (value >= '0' && value <= '9') ||
            (value >= 'a' && value <= 'f');
    });
}

bool IsSafeRelativePath(const std::filesystem::path& path) noexcept
{
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory())
    {
        return false;
    }
    for (const auto& part : path)
    {
        if (part.empty() || part == L"." || part == L"..")
            return false;
    }
    return true;
}

const JsonValue* RequiredField(const JsonValue& object,
    std::string_view name, JsonValue::Type type) noexcept
{
    const JsonValue* value = object.Find(name);
    return value && value->type == type ? value : nullptr;
}

std::optional<DistributionManifest> ReadManifest(
    const std::filesystem::path& manifestPath, std::string& error)
{
    const std::string contents = ReadFile(
        manifestPath, kMaximumManifestBytes, error);
    if (!error.empty())
        return std::nullopt;

    JsonValue root;
    std::string parseError;
    if (!ParseJson(contents, root, &parseError) || !root.IsObject())
    {
        error = "distribution manifest is invalid JSON: " + parseError;
        return std::nullopt;
    }
    const JsonValue* schema = RequiredField(
        root, "schemaVersion", JsonValue::Type::Number);
    const JsonValue* kind = RequiredField(
        root, "kind", JsonValue::Type::String);
    const JsonValue* version = RequiredField(
        root, "version", JsonValue::Type::String);
    const JsonValue* buildId = RequiredField(
        root, "buildId", JsonValue::Type::String);
    const JsonValue* distributionDirectory = RequiredField(
        root, "distributionDirectory", JsonValue::Type::String);
    const JsonValue* runtimeDirectory = RequiredField(
        root, "runtimeDirectory", JsonValue::Type::String);
    const JsonValue* dataDirectory = RequiredField(
        root, "dataDirectory", JsonValue::Type::String);
    const JsonValue* files = RequiredField(
        root, "files", JsonValue::Type::Array);
    if (!schema || schema->number != 1.0 || !kind ||
        kind->string != "steam-managed" || !version || !buildId ||
        !IsSafeIdentifier(buildId->string) || !distributionDirectory ||
        distributionDirectory->string != "distribution" ||
        !runtimeDirectory || runtimeDirectory->string != ".snowdesktop/runtime" ||
        !dataDirectory || dataDirectory->string != "data" ||
        !files || files->array.empty())
    {
        error = "distribution manifest has missing or unsupported fields";
        return std::nullopt;
    }

    DistributionManifest manifest;
    manifest.version = version->string;
    manifest.buildId = buildId->string;
    std::set<std::wstring> uniquePaths;
    bool hasHost = false;
    for (const JsonValue& item : files->array)
    {
        if (!item.IsObject())
        {
            error = "distribution manifest contains a non-object file entry";
            return std::nullopt;
        }
        const JsonValue* pathValue = RequiredField(
            item, "path", JsonValue::Type::String);
        const JsonValue* sizeValue = RequiredField(
            item, "size", JsonValue::Type::Number);
        const JsonValue* hashValue = RequiredField(
            item, "sha256", JsonValue::Type::String);
        if (!pathValue || !sizeValue || !hashValue ||
            sizeValue->number < 0 || !std::isfinite(sizeValue->number) ||
            std::floor(sizeValue->number) != sizeValue->number ||
            sizeValue->number > static_cast<double>(
                std::numeric_limits<std::uint64_t>::max()) ||
            !IsLowerHexSha256(hashValue->string))
        {
            error = "distribution manifest contains an invalid file entry";
            return std::nullopt;
        }
        const std::wstring widePath = Utf8ToWide(pathValue->string);
        const std::filesystem::path relative(widePath);
        if ((pathValue->string.size() != 0 && widePath.empty()) ||
            !IsSafeRelativePath(relative))
        {
            error = "distribution manifest contains an unsafe file path";
            return std::nullopt;
        }
        std::wstring key = relative.generic_wstring();
        std::transform(key.begin(), key.end(), key.begin(),
            [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
        if (!uniquePaths.insert(key).second)
        {
            error = "distribution manifest contains a duplicate file path";
            return std::nullopt;
        }
        if (relative == L"SnowDesktop.exe")
            hasHost = true;
        manifest.files.push_back({relative,
            static_cast<std::uint64_t>(sizeValue->number),
            hashValue->string});
    }
    if (!hasHost)
    {
        error = "distribution manifest does not contain SnowDesktop.exe";
        return std::nullopt;
    }
    const auto digest = Sha256(manifestPath, error);
    if (!digest)
        return std::nullopt;
    manifest.digest = *digest;
    return manifest;
}

bool ValidateFile(const std::filesystem::path& path,
    const DistributionFile& expected, std::string& error)
{
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(path, fileError) || fileError ||
        std::filesystem::file_size(path, fileError) != expected.size ||
        fileError)
    {
        error = "distribution file is missing or has the wrong size: " +
            expected.relativePath.string();
        return false;
    }
    const auto hash = Sha256(path, error);
    if (!hash || *hash != expected.sha256)
    {
        if (error.empty())
            error = "distribution file hash mismatch: " +
                expected.relativePath.string();
        return false;
    }
    return true;
}

bool WriteTextAtomically(const std::filesystem::path& path,
    std::string_view value, std::string& error)
{
    const std::filesystem::path temporary = path.wstring() + L".tmp." +
        std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream || !stream.write(value.data(),
                static_cast<std::streamsize>(value.size())) || !stream.flush())
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporary, cleanupError);
            error = "cannot write state file: " + path.string();
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        error = "cannot publish state file: " + path.string();
        return false;
    }
    return true;
}

std::string RuntimeContextJson()
{
    return "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"kind\": \"steam-managed\",\n"
        "  \"installRootRelative\": \"../../..\",\n"
        "  \"dataRootRelative\": \"data\",\n"
        "  \"launcherRelative\": \"SnowDesktopLauncher.exe\"\n"
        "}\n";
}

ExclusiveFile AcquireUpdateLock(const std::filesystem::path& lockPath)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(30);
    do
    {
        HANDLE value = CreateFileW(lockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (value != INVALID_HANDLE_VALUE)
            return ExclusiveFile(value);
        if (GetLastError() != ERROR_SHARING_VIOLATION &&
            GetLastError() != ERROR_LOCK_VIOLATION)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    return {};
}

std::optional<std::pair<std::filesystem::path, std::string>>
ReadFallback(const std::filesystem::path& stateRoot,
    const std::filesystem::path& runtimeRoot)
{
    std::string error;
    std::string buildId = ReadFile(
        stateRoot / kCurrentRuntimeFilename, 256, error);
    while (!buildId.empty() &&
        (buildId.back() == '\r' || buildId.back() == '\n'))
    {
        buildId.pop_back();
    }
    if (!error.empty() || !IsSafeIdentifier(buildId))
        return std::nullopt;
    const std::filesystem::path runtime = runtimeRoot /
        Utf8ToWide(buildId);
    const std::filesystem::path executable = runtime / L"SnowDesktop.exe";
    const std::filesystem::path sidecar = runtime /
        snowdesktop::deployment::kSteamRuntimeContextFilename;
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(executable, fileError) ||
        !std::filesystem::is_regular_file(sidecar, fileError))
    {
        return std::nullopt;
    }
    return std::pair(executable, buildId);
}

ApplyResult FailureOrFallback(const std::filesystem::path& stateRoot,
    const std::filesystem::path& runtimeRoot, std::string error)
{
    ApplyResult result;
    result.error = std::move(error);
    if (const auto fallback = ReadFallback(stateRoot, runtimeRoot))
    {
        result.ok = true;
        result.usedFallback = true;
        result.executable = fallback->first;
        result.buildId = fallback->second;
    }
    return result;
}
}

namespace snowdesktop::steam_runtime
{
ApplyResult ApplyDistribution(const std::filesystem::path& installRoot)
{
    const std::filesystem::path stateRoot = installRoot / kStateDirectory;
    const std::filesystem::path runtimeRoot = stateRoot / kRuntimeDirectory;
    std::error_code fileError;
    std::filesystem::create_directories(runtimeRoot, fileError);
    if (fileError)
    {
        ApplyResult result;
        result.error = "cannot create the Steam runtime state directory";
        return result;
    }
    // User data is deliberately outside every versioned runtime, but remains
    // inside Steam's install directory as required by this distribution.
    std::filesystem::create_directories(installRoot / L"data", fileError);
    if (fileError)
        return FailureOrFallback(stateRoot, runtimeRoot,
            "cannot create the Steam data directory");

    ExclusiveFile lock = AcquireUpdateLock(stateRoot / kUpdateLockFilename);
    if (!lock.valid())
        return FailureOrFallback(stateRoot, runtimeRoot,
            "cannot acquire the Steam runtime update lock");

    std::string error;
    const std::filesystem::path manifestPath =
        installRoot / kDistributionManifestFilename;
    const auto manifest = ReadManifest(manifestPath, error);
    if (!manifest)
        return FailureOrFallback(stateRoot, runtimeRoot, error);

    const std::filesystem::path target = runtimeRoot /
        Utf8ToWide(manifest->buildId);
    const std::filesystem::path complete = target / kCompleteFilename;
    std::string completeError;
    const std::string completeValue = ReadFile(complete, 256, completeError);
    if (completeError.empty() && completeValue == manifest->digest + "\n")
    {
        if (!WriteTextAtomically(stateRoot / kCurrentRuntimeFilename,
                manifest->buildId + "\n", error))
        {
            return FailureOrFallback(stateRoot, runtimeRoot, error);
        }
        ApplyResult result;
        result.ok = true;
        result.executable = target / L"SnowDesktop.exe";
        result.buildId = manifest->buildId;
        return result;
    }

    const std::filesystem::path distribution =
        installRoot / kDistributionDirectory;
    for (const DistributionFile& file : manifest->files)
    {
        if (!ValidateFile(distribution / file.relativePath, file, error))
            return FailureOrFallback(stateRoot, runtimeRoot, error);
    }

    const std::filesystem::path staging = runtimeRoot /
        (Utf8ToWide(manifest->buildId) + L".staging." +
            std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(staging, fileError);
    fileError.clear();
    std::filesystem::create_directories(staging, fileError);
    if (fileError)
        return FailureOrFallback(stateRoot, runtimeRoot,
            "cannot create a staged Steam runtime");

    bool staged = true;
    for (const DistributionFile& file : manifest->files)
    {
        const std::filesystem::path source =
            distribution / file.relativePath;
        const std::filesystem::path destination =
            staging / file.relativePath;
        std::filesystem::create_directories(
            destination.parent_path(), fileError);
        if (fileError || !CopyFileW(
                source.c_str(), destination.c_str(), FALSE) ||
            !ValidateFile(destination, file, error))
        {
            if (error.empty())
                error = "cannot stage Steam runtime file: " +
                    file.relativePath.string();
            staged = false;
            break;
        }
    }
    if (staged)
    {
        staged = WriteTextAtomically(staging /
                snowdesktop::deployment::kSteamRuntimeContextFilename,
            RuntimeContextJson(), error) &&
            WriteTextAtomically(staging / kCompleteFilename,
                manifest->digest + "\n", error);
    }
    if (!staged)
    {
        std::filesystem::remove_all(staging, fileError);
        return FailureOrFallback(stateRoot, runtimeRoot, error);
    }

    if (std::filesystem::exists(target, fileError))
    {
        fileError.clear();
        std::filesystem::remove_all(target, fileError);
        if (fileError)
        {
            std::filesystem::remove_all(staging, fileError);
            return FailureOrFallback(stateRoot, runtimeRoot,
                "an incomplete target runtime is still in use");
        }
    }
    if (!MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove_all(staging, fileError);
        return FailureOrFallback(stateRoot, runtimeRoot,
            "cannot publish the staged Steam runtime");
    }
    if (!WriteTextAtomically(stateRoot / kCurrentRuntimeFilename,
            manifest->buildId + "\n", error))
    {
        return FailureOrFallback(stateRoot, runtimeRoot, error);
    }

    ApplyResult result;
    result.ok = true;
    result.executable = target / L"SnowDesktop.exe";
    result.buildId = manifest->buildId;
    return result;
}
}
