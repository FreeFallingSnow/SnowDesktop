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
constexpr wchar_t kRuntimeManifestFilename[] =
    L".snowdesktop-runtime-manifest.json";
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
    std::string contents;
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

bool IsAsciiAlphaNumeric(unsigned char value) noexcept
{
    return (value >= '0' && value <= '9') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z');
}

bool IsReservedDeviceName(std::wstring_view value) noexcept
{
    const std::size_t dot = value.find(L'.');
    std::wstring base(value.substr(0, dot));
    std::transform(base.begin(), base.end(), base.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(towupper(character));
        });
    if (base == L"CON" || base == L"PRN" || base == L"AUX" ||
        base == L"NUL")
    {
        return true;
    }
    return base.size() == 4 &&
        (base.starts_with(L"COM") || base.starts_with(L"LPT")) &&
        base[3] >= L'1' && base[3] <= L'9';
}

bool IsSafeIdentifier(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 96 ||
        !IsAsciiAlphaNumeric(static_cast<unsigned char>(value.front())) ||
        value.back() == '.')
    {
        return false;
    }
    for (const unsigned char character : value)
    {
        if (!IsAsciiAlphaNumeric(character) && character != '-' &&
            character != '_' && character != '.')
        {
            return false;
        }
    }
    return !IsReservedDeviceName(std::wstring(value.begin(), value.end()));
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
    // Keep the hashing buffer off the launcher's default 1 MiB thread stack.
    std::vector<char> buffer(1024 * 1024);
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

std::optional<std::string> Sha256Contents(std::string_view contents,
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
    if (contents.size() > std::numeric_limits<ULONG>::max() ||
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
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
            nullptr, 0, 0) < 0 ||
        (!contents.empty() && BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(contents.data())),
            static_cast<ULONG>(contents.size()), 0) < 0) ||
        BCryptFinishHash(hash, digest.data(), hashSize, 0) < 0)
    {
        cleanup();
        error = "cannot hash distribution manifest contents";
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
        const std::wstring component = part.wstring();
        if (component.empty() || component == L"." || component == L".." ||
            component.size() > 255 || component.back() == L'.' ||
            component.back() == L' ' || IsReservedDeviceName(component))
        {
            return false;
        }
        for (const wchar_t character : component)
        {
            if (character < 32 || character == L'<' || character == L'>' ||
                character == L':' || character == L'"' ||
                character == L'|' || character == L'?' ||
                character == L'*')
            {
                return false;
            }
        }
    }
    return true;
}

bool IsReservedRuntimePath(std::wstring_view path) noexcept
{
    return path == L".snowdesktop-runtime-complete" ||
        path == L".snowdesktop-runtime-manifest.json" ||
        path == L"snowdesktop.runtime-context.json";
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
        if (IsReservedRuntimePath(key))
        {
            error = "distribution manifest contains a reserved runtime path";
            return std::nullopt;
        }
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
    const auto digest = Sha256Contents(contents, error);
    if (!digest)
        return std::nullopt;
    manifest.digest = *digest;
    manifest.contents = contents;
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

bool SameManifest(const DistributionManifest& left,
    const DistributionManifest& right) noexcept
{
    if (left.version != right.version || left.buildId != right.buildId ||
        left.digest != right.digest || left.contents != right.contents ||
        left.files.size() != right.files.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left.files.size(); ++index)
    {
        const DistributionFile& leftFile = left.files[index];
        const DistributionFile& rightFile = right.files[index];
        if (leftFile.relativePath != rightFile.relativePath ||
            leftFile.size != rightFile.size ||
            leftFile.sha256 != rightFile.sha256)
        {
            return false;
        }
    }
    return true;
}

std::optional<DistributionManifest> ValidatePublishedRuntime(
    const std::filesystem::path& runtime,
    const DistributionManifest* expectedManifest, std::string& error)
{
    std::error_code fileError;
    if (!std::filesystem::is_directory(runtime, fileError) || fileError)
    {
        error = "published Steam runtime is missing or is not a directory";
        return std::nullopt;
    }

    auto manifest = ReadManifest(runtime / kRuntimeManifestFilename, error);
    if (!manifest)
    {
        if (error.empty())
            error = "published Steam runtime manifest is invalid";
        return std::nullopt;
    }
    if (expectedManifest && !SameManifest(*manifest, *expectedManifest))
    {
        error = "published Steam runtime manifest does not match the distribution";
        return std::nullopt;
    }

    std::string markerError;
    const std::string marker = ReadFile(
        runtime / kCompleteFilename, 256, markerError);
    if (!markerError.empty() || marker != manifest->digest + "\n")
    {
        error = "published Steam runtime completion marker is invalid";
        return std::nullopt;
    }

    std::string sidecarError;
    const std::string sidecar = ReadFile(runtime /
            snowdesktop::deployment::kSteamRuntimeContextFilename,
        4096, sidecarError);
    if (!sidecarError.empty() || sidecar != RuntimeContextJson())
    {
        error = "published Steam runtime context is invalid";
        return std::nullopt;
    }

    for (const DistributionFile& file : manifest->files)
    {
        if (!ValidateFile(runtime / file.relativePath, file, error))
        {
            if (error.empty())
                error = "published Steam runtime file is invalid";
            return std::nullopt;
        }
    }
    return manifest;
}

struct RuntimeDestination
{
    std::filesystem::path path;
    std::string directoryId;
    bool alreadyPublished = false;
};

std::optional<RuntimeDestination> SelectRecoveryDestination(
    const std::filesystem::path& runtimeRoot,
    const DistributionManifest& manifest, std::string& error)
{
    const std::string base = "recovery-" + manifest.digest;
    for (std::uint64_t attempt = 0;; ++attempt)
    {
        const std::string directoryId = attempt == 0 ? base :
            base + "-" + std::to_string(attempt);
        const std::filesystem::path candidate = runtimeRoot /
            Utf8ToWide(directoryId);
        std::error_code fileError;
        const bool exists = std::filesystem::exists(candidate, fileError);
        if (fileError)
        {
            error = "cannot inspect a Steam runtime recovery directory";
            return std::nullopt;
        }
        if (!exists)
            return RuntimeDestination{candidate, directoryId, false};

        std::string validationError;
        if (ValidatePublishedRuntime(candidate, &manifest, validationError))
            return RuntimeDestination{candidate, directoryId, true};

        if (attempt == std::numeric_limits<std::uint64_t>::max())
        {
            error = "cannot allocate a unique Steam runtime recovery directory";
            return std::nullopt;
        }
    }
}

std::optional<std::filesystem::path> CreateUniqueStagingDirectory(
    const std::filesystem::path& runtimeRoot, std::string& error)
{
    const std::wstring prefix = L".staging." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    for (std::uint32_t attempt = 0;; ++attempt)
    {
        const std::filesystem::path candidate = runtimeRoot /
            (attempt == 0 ? prefix : prefix + L"." +
                std::to_wstring(attempt));
        std::error_code fileError;
        if (std::filesystem::create_directory(candidate, fileError))
            return candidate;
        if (fileError)
        {
            error = "cannot create a staged Steam runtime";
            return std::nullopt;
        }
        if (attempt == std::numeric_limits<std::uint32_t>::max())
        {
            error = "cannot allocate a unique staged Steam runtime";
            return std::nullopt;
        }
    }
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
    const std::filesystem::path& runtimeRoot, std::string& validationError)
{
    std::string error;
    std::string directoryId = ReadFile(
        stateRoot / kCurrentRuntimeFilename, 256, error);
    while (!directoryId.empty() &&
        (directoryId.back() == '\r' || directoryId.back() == '\n'))
    {
        directoryId.pop_back();
    }
    if (!error.empty() || !IsSafeIdentifier(directoryId))
    {
        validationError = "the active Steam runtime selection is invalid";
        return std::nullopt;
    }
    const std::filesystem::path runtime = runtimeRoot /
        Utf8ToWide(directoryId);
    auto manifest = ValidatePublishedRuntime(
        runtime, nullptr, validationError);
    if (!manifest)
        return std::nullopt;
    return std::pair(runtime / L"SnowDesktop.exe", manifest->buildId);
}

ApplyResult FailureOrFallback(const std::filesystem::path& stateRoot,
    const std::filesystem::path& runtimeRoot, std::string error)
{
    ApplyResult result;
    result.error = std::move(error);
    std::string fallbackError;
    if (const auto fallback = ReadFallback(
            stateRoot, runtimeRoot, fallbackError))
    {
        result.ok = true;
        result.usedFallback = true;
        result.executable = fallback->first;
        result.buildId = fallback->second;
    }
    else if (!fallbackError.empty())
    {
        result.error += "; fallback rejected: " + fallbackError;
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

    auto activate = [&](const RuntimeDestination& selected) {
        if (!WriteTextAtomically(stateRoot / kCurrentRuntimeFilename,
                selected.directoryId + "\n", error))
        {
            return FailureOrFallback(stateRoot, runtimeRoot, error);
        }
        ApplyResult result;
        result.ok = true;
        result.executable = selected.path / L"SnowDesktop.exe";
        result.buildId = manifest->buildId;
        return result;
    };

    RuntimeDestination destination{
        runtimeRoot / Utf8ToWide(manifest->buildId), manifest->buildId, false};
    const bool primaryExists =
        std::filesystem::exists(destination.path, fileError);
    if (fileError)
        return FailureOrFallback(stateRoot, runtimeRoot,
            "cannot inspect the Steam runtime target");
    if (primaryExists)
    {
        std::string validationError;
        if (ValidatePublishedRuntime(
                destination.path, &*manifest, validationError))
        {
            destination.alreadyPublished = true;
            return activate(destination);
        }

        // Published runtimes are immutable. A missing marker, damaged file,
        // or reused build ID must never turn repair into an in-place delete.
        // Instead, either reuse an already verified recovery publication or
        // reserve a new content-addressed recovery directory.
        auto recovery = SelectRecoveryDestination(
            runtimeRoot, *manifest, error);
        if (!recovery)
            return FailureOrFallback(stateRoot, runtimeRoot, error);
        destination = std::move(*recovery);
        if (destination.alreadyPublished)
            return activate(destination);
    }

    const std::filesystem::path distribution =
        installRoot / kDistributionDirectory;
    for (const DistributionFile& file : manifest->files)
    {
        if (!ValidateFile(distribution / file.relativePath, file, error))
            return FailureOrFallback(stateRoot, runtimeRoot, error);
    }

    const auto stagingValue = CreateUniqueStagingDirectory(
        runtimeRoot, error);
    if (!stagingValue)
        return FailureOrFallback(stateRoot, runtimeRoot, error);
    const std::filesystem::path staging = *stagingValue;

    bool staged = true;
    for (const DistributionFile& file : manifest->files)
    {
        const std::filesystem::path source =
            distribution / file.relativePath;
        const std::filesystem::path fileDestination =
            staging / file.relativePath;
        std::filesystem::create_directories(
            fileDestination.parent_path(), fileError);
        if (fileError || !CopyFileW(
                source.c_str(), fileDestination.c_str(), FALSE) ||
            !ValidateFile(fileDestination, file, error))
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
        staged = WriteTextAtomically(staging / kRuntimeManifestFilename,
            manifest->contents, error);
    }
    if (staged)
    {
        staged = WriteTextAtomically(staging /
                snowdesktop::deployment::kSteamRuntimeContextFilename,
            RuntimeContextJson(), error) &&
            WriteTextAtomically(staging / kCompleteFilename,
                manifest->digest + "\n", error);
    }
    if (staged)
    {
        std::string validationError;
        if (!ValidatePublishedRuntime(staging, &*manifest, validationError))
        {
            error = "staged Steam runtime validation failed: " +
                validationError;
            staged = false;
        }
    }
    if (!staged)
    {
        std::filesystem::remove_all(staging, fileError);
        return FailureOrFallback(stateRoot, runtimeRoot, error);
    }

    if (!MoveFileExW(staging.c_str(), destination.path.c_str(),
            MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove_all(staging, fileError);
        return FailureOrFallback(stateRoot, runtimeRoot,
            "cannot publish the staged Steam runtime");
    }

    std::string publishedError;
    if (!ValidatePublishedRuntime(
            destination.path, &*manifest, publishedError))
    {
        return FailureOrFallback(stateRoot, runtimeRoot,
            "published Steam runtime validation failed: " + publishedError);
    }
    return activate(destination);
}
}
