#include "steam_runtime_manager.h"

#include "json_value.h"
#include "steam_runtime_context.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
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
using snowdesktop::steam_runtime::PruneResult;

constexpr wchar_t kDistributionDirectory[] = L"distribution";
constexpr wchar_t kStateDirectory[] = L".snowdesktop";
constexpr wchar_t kRuntimeDirectory[] = L"runtime";
constexpr wchar_t kCurrentRuntimeFilename[] = L"current-runtime.txt";
constexpr wchar_t kCompleteFilename[] = L".snowdesktop-runtime-complete";
constexpr wchar_t kRuntimeManifestFilename[] =
    L".snowdesktop-runtime-manifest.json";
constexpr wchar_t kUpdateLockFilename[] = L"update.lock";
constexpr std::uint64_t kMaximumManifestBytes = 16ULL * 1024ULL * 1024ULL;
constexpr double kMaximumExactJsonInteger = 9007199254740991.0;

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

bool ValidatePlainDirectoryNoReparse(const std::filesystem::path& path,
    std::string_view label, std::string& error)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes & FILE_ATTRIBUTE_DEVICE) != 0)
    {
        error = std::string(label) +
            " is missing, is not a directory, or is a reparse point";
        return false;
    }
    return true;
}

bool EnsurePlainDirectoryNoReparse(const std::filesystem::path& path,
    std::string_view label, std::string& error)
{
    if (!CreateDirectoryW(path.c_str(), nullptr))
    {
        const DWORD createError = GetLastError();
        if (createError != ERROR_ALREADY_EXISTS)
        {
            error = "cannot create " + std::string(label) +
                " (Win32 error " + std::to_string(createError) + ')';
            return false;
        }
    }
    return ValidatePlainDirectoryNoReparse(path, label, error);
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
            sizeValue->number > kMaximumExactJsonInteger ||
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

bool NormalizeStagedFileAttributes(const std::filesystem::path& path,
    std::string& error)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes & FILE_ATTRIBUTE_DEVICE) != 0)
    {
        error = "staged Steam runtime file is not a plain file";
        return false;
    }
    DWORD normalized = attributes & ~FILE_ATTRIBUTE_READONLY;
    if (normalized == 0)
        normalized = FILE_ATTRIBUTE_NORMAL;
    if (normalized != attributes &&
        !SetFileAttributesW(path.c_str(), normalized))
    {
        error = "cannot make a staged Steam runtime file writable";
        return false;
    }
    return true;
}

bool ValidatePlainFileNoReparse(const std::filesystem::path& path,
    std::string_view label, std::string& error);

bool FlushPlainFile(const std::filesystem::path& path,
    std::string_view label, std::string& error)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "cannot open " + std::string(label) +
            " for durable publication (Win32 error " +
            std::to_string(GetLastError()) + ')';
        return false;
    }

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    DWORD operationError = ERROR_SUCCESS;
    if (!GetFileInformationByHandleEx(file,
            FileAttributeTagInfo, &attributes, sizeof(attributes)))
    {
        operationError = GetLastError();
    }
    else if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DEVICE) != 0)
    {
        operationError = ERROR_INVALID_DATA;
    }
    else if (!FlushFileBuffers(file))
    {
        operationError = GetLastError();
    }
    const bool closed = CloseHandle(file) != FALSE;
    const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
    if (operationError != ERROR_SUCCESS || !closed)
    {
        error = "cannot durably flush " + std::string(label) +
            " (Win32 error " + std::to_string(
                operationError != ERROR_SUCCESS ?
                    operationError : closeError) + ')';
        return false;
    }
    return true;
}

bool WriteTextAtomically(const std::filesystem::path& path,
    std::string_view value, std::string& error)
{
    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 64; ++attempt)
    {
        std::array<UCHAR, 8> randomBytes{};
        if (BCryptGenRandom(nullptr, randomBytes.data(),
                static_cast<ULONG>(randomBytes.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        {
            error = "cannot create a random temporary state filename";
            return false;
        }

        std::wostringstream suffix;
        suffix << std::hex << std::setfill(L'0');
        for (const UCHAR byte : randomBytes)
            suffix << std::setw(2) << static_cast<unsigned>(byte);
        temporary = path.wstring() + L".tmp." + suffix.str();
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (file != INVALID_HANDLE_VALUE)
            break;
        const DWORD createError = GetLastError();
        if (createError != ERROR_FILE_EXISTS &&
            createError != ERROR_ALREADY_EXISTS)
        {
            error = "cannot create a temporary state file (Win32 error " +
                std::to_string(createError) + ')';
            return false;
        }
    }
    if (file == INVALID_HANDLE_VALUE)
    {
        error = "cannot allocate a unique temporary state file";
        return false;
    }

    std::size_t offset = 0;
    DWORD operationError = ERROR_SUCCESS;
    while (offset < value.size() && operationError == ERROR_SUCCESS)
    {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            value.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(file, value.data() + offset, request,
                &written, nullptr))
        {
            operationError = GetLastError();
        }
        else if (written != request)
        {
            operationError = ERROR_WRITE_FAULT;
        }
        offset += written;
    }
    if (operationError == ERROR_SUCCESS && !FlushFileBuffers(file))
        operationError = GetLastError();
    const bool closed = CloseHandle(file) != FALSE;
    const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
    if (operationError != ERROR_SUCCESS || !closed)
    {
        DeleteFileW(temporary.c_str());
        error = "cannot durably write state file: " + path.string() +
            " (Win32 error " + std::to_string(
                operationError != ERROR_SUCCESS ?
                    operationError : closeError) + ')';
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD moveError = GetLastError();
        DeleteFileW(temporary.c_str());
        error = "cannot publish state file: " + path.string() +
            " (Win32 error " + std::to_string(moveError) + ')';
        return false;
    }
    return ValidatePlainFileNoReparse(path,
        "published Steam state file", error);
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

std::wstring ExactPathKey(const std::filesystem::path& path)
{
    return path.generic_wstring();
}

bool ValidatePlainFileNoReparse(const std::filesystem::path& path,
    std::string_view label, std::string& error)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes & FILE_ATTRIBUTE_DEVICE) != 0)
    {
        error = std::string(label) +
            " is missing, is not a regular file, or is a reparse point";
        return false;
    }
    return true;
}

bool ValidateExactPayloadTree(const std::filesystem::path& root,
    const DistributionManifest& manifest, bool includeRuntimeMetadata,
    bool ignoreUnexpectedEntries, std::string& error)
{
    if (!ValidatePlainDirectoryNoReparse(
            root, "Steam payload root", error))
        return false;

    std::set<std::wstring> expectedFiles;
    std::set<std::wstring> expectedDirectories;
    for (const DistributionFile& file : manifest.files)
    {
        expectedFiles.insert(ExactPathKey(file.relativePath));
        for (std::filesystem::path parent = file.relativePath.parent_path();
             !parent.empty(); parent = parent.parent_path())
        {
            expectedDirectories.insert(ExactPathKey(parent));
        }
    }
    if (includeRuntimeMetadata)
    {
        expectedFiles.insert(ExactPathKey(kCompleteFilename));
        expectedFiles.insert(ExactPathKey(kRuntimeManifestFilename));
        expectedFiles.insert(ExactPathKey(
            snowdesktop::deployment::kSteamRuntimeContextFilename));
    }

    if (ignoreUnexpectedEntries)
    {
        // The Steam-controlled distribution is only a reconstruction source.
        // Validate every manifest path and later hash every listed file, but
        // do not inspect or copy unrelated local residue. This also keeps an
        // unknown inaccessible or reparse-point entry from blocking repair.
        for (const std::wstring& directory : expectedDirectories)
        {
            if (!ValidatePlainDirectoryNoReparse(root / directory,
                    "Steam distribution directory", error))
            {
                return false;
            }
        }
        for (const std::wstring& file : expectedFiles)
        {
            if (!ValidatePlainFileNoReparse(root / file,
                    "Steam distribution file", error))
            {
                return false;
            }
        }
        return true;
    }

    std::set<std::wstring> actualFiles;
    std::set<std::wstring> actualDirectories;
    std::error_code iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::none, iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    if (iteratorError)
    {
        error = "cannot enumerate the Steam payload tree";
        return false;
    }
    while (iterator != end)
    {
        const std::filesystem::path entryPath = iterator->path();
        const std::filesystem::path relative =
            entryPath.lexically_relative(root);
        const std::wstring key = ExactPathKey(relative);
        const DWORD attributes = GetFileAttributesW(entryPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (attributes & FILE_ATTRIBUTE_DEVICE) != 0)
        {
            error = "Steam payload contains an inaccessible or reparse-point entry: " +
                relative.string();
            return false;
        }

        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            if (!expectedDirectories.contains(key))
            {
                error = "Steam payload contains an unexpected directory: " +
                    relative.string();
                return false;
            }
            actualDirectories.insert(key);
        }
        else
        {
            if (!expectedFiles.contains(key))
            {
                error = "Steam payload contains an unexpected file: " +
                    relative.string();
                return false;
            }
            actualFiles.insert(key);
        }

        iterator.increment(iteratorError);
        if (iteratorError)
        {
            error = "cannot enumerate the complete Steam payload tree";
            return false;
        }
    }

    if (actualFiles != expectedFiles ||
        actualDirectories != expectedDirectories)
    {
        error = "Steam payload tree is incomplete";
        return false;
    }
    return true;
}

bool DirectoryIdMatchesManifest(std::string_view directoryId,
    const DistributionManifest& manifest) noexcept
{
    if (directoryId == manifest.buildId)
        return true;
    const std::string recoveryPrefix = "recovery-" + manifest.digest;
    if (directoryId == recoveryPrefix)
        return true;
    if (!directoryId.starts_with(recoveryPrefix) ||
        directoryId.size() <= recoveryPrefix.size() + 1 ||
        directoryId[recoveryPrefix.size()] != '-')
    {
        return false;
    }
    const std::string_view suffix =
        directoryId.substr(recoveryPrefix.size() + 1);
    if (suffix.front() < '1' || suffix.front() > '9')
        return false;
    std::uint64_t attempt = 0;
    const auto parsed = std::from_chars(
        suffix.data(), suffix.data() + suffix.size(), attempt, 10);
    return parsed.ec == std::errc{} &&
        parsed.ptr == suffix.data() + suffix.size() && attempt != 0 &&
        std::to_string(attempt) == suffix;
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
    const DWORD runtimeAttributes = GetFileAttributesW(runtime.c_str());
    if (runtimeAttributes == INVALID_FILE_ATTRIBUTES ||
        (runtimeAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (runtimeAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        error = "published Steam runtime is missing, is not a directory, or is a reparse point";
        return std::nullopt;
    }

    std::string boundaryError;
    if (!ValidatePlainFileNoReparse(runtime / kRuntimeManifestFilename,
            "published Steam runtime manifest", boundaryError))
    {
        error = std::move(boundaryError);
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

    if (!ValidateExactPayloadTree(
            runtime, *manifest, true, false, error))
    {
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

    // The completion marker is the final publication fence. Read it only
    // after the manifest, sidecar, exact tree, and every payload hash pass.
    std::string markerError;
    const std::string marker = ReadFile(
        runtime / kCompleteFilename, 256, markerError);
    if (!markerError.empty() || marker != manifest->digest + "\n")
    {
        error = "published Steam runtime completion marker is invalid";
        return std::nullopt;
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
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD inspectError = GetLastError();
            if (inspectError == ERROR_FILE_NOT_FOUND ||
                inspectError == ERROR_PATH_NOT_FOUND)
            {
                return RuntimeDestination{candidate, directoryId, false};
            }
            error = "cannot inspect a Steam runtime recovery directory";
            return std::nullopt;
        }

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

std::optional<std::filesystem::path> SelectUniqueStagingPath(
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
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD inspectError = GetLastError();
            if (inspectError == ERROR_FILE_NOT_FOUND ||
                inspectError == ERROR_PATH_NOT_FOUND)
            {
                return candidate;
            }
            error = "cannot inspect a retired Steam runtime staging path";
            return std::nullopt;
        }
        if (attempt == std::numeric_limits<std::uint32_t>::max())
        {
            error = "cannot allocate a unique retired Steam runtime staging path";
            return std::nullopt;
        }
    }
}

bool RemoveStagingDirectorySafely(const std::filesystem::path& staging,
    const std::filesystem::path& runtimeRoot, std::string& error) noexcept
{
    if (staging.parent_path() != runtimeRoot)
    {
        error = "refusing to clean a staged runtime outside the runtime root";
        return false;
    }
    std::string boundaryError;
    if (!ValidatePlainDirectoryNoReparse(
            runtimeRoot, "Steam runtime directory", boundaryError) ||
        !ValidatePlainDirectoryNoReparse(
            staging, "staged Steam runtime", boundaryError))
    {
        error = std::move(boundaryError);
        return false;
    }

    struct CleanupEntry
    {
        std::filesystem::path path;
        DWORD attributes = 0;
    };
    std::vector<CleanupEntry> entries;
    std::error_code iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        staging, std::filesystem::directory_options::none, iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    if (iteratorError)
    {
        error = "cannot enumerate a staged runtime for cleanup";
        return false;
    }
    while (iterator != end)
    {
        const std::filesystem::path entry = iterator->path();
        const DWORD attributes = GetFileAttributesW(entry.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (attributes & FILE_ATTRIBUTE_DEVICE) != 0)
        {
            error = "refusing to clean a staged runtime containing a reparse or device entry";
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_READONLY) != 0 &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            HANDLE file = CreateFileW(entry.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = "cannot inspect a read-only staged runtime entry";
                return false;
            }
            BY_HANDLE_FILE_INFORMATION information{};
            const bool inspected =
                GetFileInformationByHandle(file, &information) != FALSE;
            const DWORD inspectError =
                inspected ? ERROR_SUCCESS : GetLastError();
            const bool closed = CloseHandle(file) != FALSE;
            const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
            if (!inspected || !closed)
            {
                error = "cannot inspect a read-only staged runtime link (Win32 error " +
                    std::to_string(!inspected ? inspectError : closeError) + ')';
                return false;
            }
            if (information.nNumberOfLinks > 1)
            {
                error = "refusing to change attributes through a staged runtime hardlink";
                return false;
            }
        }
        entries.push_back({entry, attributes});
        iterator.increment(iteratorError);
        if (iteratorError)
        {
            error = "cannot enumerate the complete staged runtime for cleanup";
            return false;
        }
    }

    for (const CleanupEntry& entry : entries)
    {
        if ((entry.attributes & FILE_ATTRIBUTE_READONLY) == 0)
            continue;
        DWORD normalized = entry.attributes & ~FILE_ATTRIBUTE_READONLY;
        if (normalized == 0)
            normalized = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileAttributesW(entry.path.c_str(), normalized))
        {
            error = "cannot clear a read-only staged runtime entry";
            return false;
        }
    }

    const DWORD stagingAttributes = GetFileAttributesW(staging.c_str());
    if (stagingAttributes == INVALID_FILE_ATTRIBUTES)
    {
        error = "cannot inspect the staged runtime before cleanup";
        return false;
    }
    if ((stagingAttributes & FILE_ATTRIBUTE_READONLY) != 0)
    {
        DWORD normalized = stagingAttributes & ~FILE_ATTRIBUTE_READONLY;
        if (normalized == 0)
            normalized = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileAttributesW(staging.c_str(), normalized))
        {
            error = "cannot clear the read-only staged runtime root";
            return false;
        }
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(staging, cleanupError);
    if (cleanupError)
    {
        error = "cannot remove the staged Steam runtime: " +
            cleanupError.message();
        return false;
    }
    return true;
}

bool ParseCanonicalUnsigned(std::wstring_view text, std::uint64_t maximum,
    bool requireNonZero, std::uint64_t& value) noexcept
{
    if (text.empty() || (text.size() > 1 && text.front() == L'0'))
        return false;
    std::uint64_t parsed = 0;
    for (const wchar_t character : text)
    {
        if (character < L'0' || character > L'9')
            return false;
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - L'0');
        if (parsed > (maximum - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }
    if (requireNonZero && parsed == 0)
        return false;
    value = parsed;
    return true;
}

bool IsStagingDirectoryName(std::wstring_view name) noexcept
{
    constexpr std::wstring_view prefix = L".staging.";
    if (!name.starts_with(prefix))
        return false;
    name.remove_prefix(prefix.size());

    const std::size_t firstSeparator = name.find(L'.');
    if (firstSeparator == std::wstring_view::npos)
        return false;
    const std::wstring_view process = name.substr(0, firstSeparator);
    name.remove_prefix(firstSeparator + 1);
    const std::size_t secondSeparator = name.find(L'.');
    const std::wstring_view tick = name.substr(0, secondSeparator);
    const std::wstring_view attempt = secondSeparator ==
            std::wstring_view::npos ? std::wstring_view{} :
        name.substr(secondSeparator + 1);
    if (secondSeparator != std::wstring_view::npos &&
        attempt.find(L'.') != std::wstring_view::npos)
    {
        return false;
    }

    std::uint64_t parsed = 0;
    return ParseCanonicalUnsigned(process,
               std::numeric_limits<std::uint32_t>::max(), true, parsed) &&
        ParseCanonicalUnsigned(tick,
            std::numeric_limits<std::uint64_t>::max(), false, parsed) &&
        (secondSeparator == std::wstring_view::npos ||
            ParseCanonicalUnsigned(attempt,
                std::numeric_limits<std::uint32_t>::max(), true, parsed));
}

bool CleanupAbandonedStagingDirectories(
    const std::filesystem::path& runtimeRoot, std::string& error)
{
    if (!ValidatePlainDirectoryNoReparse(
            runtimeRoot, "Steam runtime directory", error))
    {
        return false;
    }

    std::vector<std::filesystem::path> abandoned;
    std::error_code iteratorError;
    std::filesystem::directory_iterator iterator(
        runtimeRoot, std::filesystem::directory_options::none, iteratorError);
    const std::filesystem::directory_iterator end;
    if (iteratorError)
    {
        error = "cannot enumerate the Steam runtime directory for abandoned staging data";
        return false;
    }
    while (iterator != end)
    {
        const std::filesystem::path entry = iterator->path();
        if (IsStagingDirectoryName(entry.filename().wstring()))
        {
            const DWORD attributes = GetFileAttributesW(entry.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                (attributes & FILE_ATTRIBUTE_DEVICE) != 0)
            {
                error = "an abandoned staging name is not a plain directory";
                return false;
            }
            abandoned.push_back(entry);
        }
        iterator.increment(iteratorError);
        if (iteratorError)
        {
            error = "cannot enumerate the complete Steam runtime directory for abandoned staging data";
            return false;
        }
    }

    for (const auto& entry : abandoned)
    {
        std::string cleanupError;
        if (!RemoveStagingDirectorySafely(
                entry, runtimeRoot, cleanupError))
        {
            error = "cannot clean abandoned Steam staging data: " +
                cleanupError;
            return false;
        }
    }
    return true;
}

ExclusiveFile AcquireUpdateLock(const std::filesystem::path& lockPath)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(30);
    do
    {
        HANDLE value = CreateFileW(lockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (value != INVALID_HANDLE_VALUE)
        {
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (GetFileInformationByHandleEx(value, FileAttributeTagInfo,
                    &attributes, sizeof(attributes)) &&
                (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
                (attributes.FileAttributes & FILE_ATTRIBUTE_DEVICE) == 0)
            {
                return ExclusiveFile(value);
            }
            CloseHandle(value);
            break;
        }
        const DWORD openError = GetLastError();
        if (openError != ERROR_SHARING_VIOLATION &&
            openError != ERROR_LOCK_VIOLATION)
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
    if (!ValidatePlainDirectoryNoReparse(
            stateRoot, "Steam runtime state directory", validationError) ||
        !ValidatePlainDirectoryNoReparse(
            runtimeRoot, "Steam runtime directory", validationError))
    {
        return std::nullopt;
    }

    std::string error;
    const std::filesystem::path pointer =
        stateRoot / kCurrentRuntimeFilename;
    if (!ValidatePlainFileNoReparse(
            pointer, "active Steam runtime selection", error))
    {
        validationError = std::move(error);
        return std::nullopt;
    }
    std::string directoryId = ReadFile(pointer, 256, error);
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
    if (!DirectoryIdMatchesManifest(directoryId, *manifest))
    {
        validationError =
            "the active Steam runtime directory does not match its manifest";
        return std::nullopt;
    }
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
    std::string rootError;
    if (!ValidatePlainDirectoryNoReparse(
            installRoot, "Steam install root", rootError) ||
        !EnsurePlainDirectoryNoReparse(
            stateRoot, "Steam runtime state directory", rootError) ||
        !EnsurePlainDirectoryNoReparse(
            runtimeRoot, "Steam runtime directory", rootError))
    {
        ApplyResult result;
        result.error = std::move(rootError);
        return result;
    }
    // User data is deliberately outside every versioned runtime, but remains
    // inside Steam's install directory as required by this distribution.
    if (!EnsurePlainDirectoryNoReparse(
            installRoot / L"data", "Steam data directory", rootError))
    {
        ApplyResult result;
        result.error = std::move(rootError);
        return result;
    }

    std::error_code fileError;

    ExclusiveFile lock = AcquireUpdateLock(stateRoot / kUpdateLockFilename);
    if (!lock.valid())
        return FailureOrFallback(stateRoot, runtimeRoot,
            "cannot acquire the Steam runtime update lock");

    std::string error;
    if (!CleanupAbandonedStagingDirectories(runtimeRoot, error))
        return FailureOrFallback(stateRoot, runtimeRoot, error);

    const std::filesystem::path manifestPath =
        installRoot / kDistributionManifestFilename;
    if (!ValidatePlainFileNoReparse(manifestPath,
            "Steam distribution manifest", error))
    {
        return FailureOrFallback(stateRoot, runtimeRoot, error);
    }
    const auto manifest = ReadManifest(manifestPath, error);
    if (!manifest)
        return FailureOrFallback(stateRoot, runtimeRoot, error);

    auto activate = [&](const RuntimeDestination& selected) {
        if (!ValidatePlainDirectoryNoReparse(
                stateRoot, "Steam runtime state directory", error) ||
            !ValidatePlainDirectoryNoReparse(
                runtimeRoot, "Steam runtime directory", error))
        {
            return FailureOrFallback(stateRoot, runtimeRoot, error);
        }
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
    const DWORD primaryAttributes = GetFileAttributesW(destination.path.c_str());
    const bool primaryExists = primaryAttributes != INVALID_FILE_ATTRIBUTES;
    if (!primaryExists)
    {
        const DWORD inspectError = GetLastError();
        if (inspectError != ERROR_FILE_NOT_FOUND &&
            inspectError != ERROR_PATH_NOT_FOUND)
        {
            return FailureOrFallback(stateRoot, runtimeRoot,
                "cannot inspect the Steam runtime target");
        }
    }
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
    if (!ValidateExactPayloadTree(
            distribution, *manifest, false, true, error))
    {
        return FailureOrFallback(stateRoot, runtimeRoot, error);
    }
    for (const DistributionFile& file : manifest->files)
    {
        if (!ValidateFile(distribution / file.relativePath, file, error))
            return FailureOrFallback(stateRoot, runtimeRoot, error);
    }

    if (!ValidatePlainDirectoryNoReparse(
            stateRoot, "Steam runtime state directory", error) ||
        !ValidatePlainDirectoryNoReparse(
            runtimeRoot, "Steam runtime directory", error))
    {
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
            !NormalizeStagedFileAttributes(fileDestination, error) ||
            !ValidateFile(fileDestination, file, error) ||
            !FlushPlainFile(fileDestination,
                "staged Steam runtime file", error))
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
        std::string cleanupError;
        if (!RemoveStagingDirectorySafely(
                staging, runtimeRoot, cleanupError))
        {
            error += "; staged cleanup failed: " + cleanupError;
        }
        return FailureOrFallback(stateRoot, runtimeRoot, error);
    }

    if (!MoveFileExW(staging.c_str(), destination.path.c_str(),
            MOVEFILE_WRITE_THROUGH))
    {
        const DWORD moveError = GetLastError();
        std::string publishError =
            "cannot publish the staged Steam runtime (Win32 error " +
            std::to_string(moveError) + ')';
        std::string cleanupError;
        if (!RemoveStagingDirectorySafely(
                staging, runtimeRoot, cleanupError))
        {
            publishError += "; staged cleanup failed: " + cleanupError;
        }
        return FailureOrFallback(
            stateRoot, runtimeRoot, std::move(publishError));
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

PruneResult PruneInactiveRuntimes(
    const std::filesystem::path& installRoot,
    const std::filesystem::path& currentExecutable)
{
    PruneResult result;
    const auto context =
        snowdesktop::deployment::ResolveRuntimeDeploymentContext(
            currentExecutable, false);
    if (context.kind !=
            snowdesktop::deployment::RuntimeDeploymentKind::SteamManaged)
    {
        result.error =
            "refusing to prune runtimes outside a managed Steam deployment";
        return result;
    }

    std::error_code pathError;
    if (!std::filesystem::equivalent(
            installRoot, context.installRoot, pathError) || pathError)
    {
        result.error =
            "the selected Steam runtime does not belong to the install root";
        return result;
    }

    const std::filesystem::path stateRoot = installRoot / kStateDirectory;
    const std::filesystem::path runtimeRoot = stateRoot / kRuntimeDirectory;
    std::string error;
    if (!ValidatePlainDirectoryNoReparse(
            stateRoot, "Steam runtime state directory", error) ||
        !ValidatePlainDirectoryNoReparse(
            runtimeRoot, "Steam runtime directory", error))
    {
        result.error = std::move(error);
        return result;
    }

    ExclusiveFile lock = AcquireUpdateLock(stateRoot / kUpdateLockFilename);
    if (!lock.valid())
    {
        result.error = "cannot acquire the Steam runtime cleanup lock";
        return result;
    }
    if (!CleanupAbandonedStagingDirectories(runtimeRoot, error))
    {
        result.error = std::move(error);
        return result;
    }

    std::string pointerError;
    const auto active = ReadFallback(stateRoot, runtimeRoot, pointerError);
    pathError.clear();
    if (!active || !std::filesystem::equivalent(
            active->first, currentExecutable, pathError) || pathError)
    {
        result.error = pointerError.empty()
            ? "the selected executable is not the active Steam runtime"
            : std::move(pointerError);
        return result;
    }

    const std::filesystem::path currentRuntime =
        currentExecutable.parent_path();
    std::vector<std::filesystem::path> inactive;
    std::error_code iteratorError;
    std::filesystem::directory_iterator iterator(
        runtimeRoot, std::filesystem::directory_options::none, iteratorError);
    const std::filesystem::directory_iterator end;
    if (iteratorError)
    {
        result.error = "cannot enumerate Steam runtimes for cleanup";
        return result;
    }
    while (iterator != end)
    {
        const std::filesystem::path entry = iterator->path();
        pathError.clear();
        const bool isCurrent = std::filesystem::equivalent(
            entry, currentRuntime, pathError);
        if (!pathError && !isCurrent)
        {
            std::string validationError;
            const auto manifest = ValidatePublishedRuntime(
                entry, nullptr, validationError);
            if (manifest && DirectoryIdMatchesManifest(
                    entry.filename().string(), *manifest))
            {
                inactive.push_back(entry);
            }
        }
        iterator.increment(iteratorError);
        if (iteratorError)
        {
            result.error =
                "cannot enumerate the complete Steam runtime directory for cleanup";
            return result;
        }
    }

    for (const std::filesystem::path& entry : inactive)
    {
        const auto staging = SelectUniqueStagingPath(runtimeRoot, error);
        if (!staging)
        {
            result.error = std::move(error);
            return result;
        }
        if (!MoveFileExW(entry.c_str(), staging->c_str(),
                MOVEFILE_WRITE_THROUGH))
        {
            const DWORD moveError = GetLastError();
            if (moveError == ERROR_ACCESS_DENIED ||
                moveError == ERROR_SHARING_VIOLATION ||
                moveError == ERROR_LOCK_VIOLATION)
            {
                ++result.retained;
                continue;
            }
            result.error =
                "cannot retire an inactive Steam runtime (Win32 error " +
                std::to_string(moveError) + ')';
            return result;
        }
        if (!RemoveStagingDirectorySafely(*staging, runtimeRoot, error))
        {
            result.error =
                "cannot remove a retired Steam runtime: " + error;
            return result;
        }
        ++result.removed;
    }
    result.ok = true;
    return result;
}
}
