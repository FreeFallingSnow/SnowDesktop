#include "full_data_backup.h"

#include "json_value.h"
#include "portable_data_migration.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace snowdesktop::backup
{
namespace
{
constexpr int kBackupSchemaVersion = 1;
constexpr wchar_t kManifestName[] = L"backup.json";
constexpr char kCancelledError[] = "operation cancelled";

bool StopRequested(const CancellationContext& cancellation,
    bool& cancelled, std::string& error)
{
    if (!cancellation.stopToken.stop_requested())
        return false;
    cancelled = true;
    error = kCancelledError;
    return true;
}

bool BeginNonInterruptible(const CancellationContext& cancellation,
    bool& cancelled, std::string& error)
{
    if (StopRequested(cancellation, cancelled, error))
        return false;
    if (cancellation.beginNonInterruptible &&
        !cancellation.beginNonInterruptible())
    {
        cancelled = true;
        error = kCancelledError;
        return false;
    }
    return true;
}

struct FileRecord
{
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
};

struct BackupManifest
{
    int schemaVersion = kBackupSchemaVersion;
    std::string id;
    std::string createdAt;
    std::string hostVersion;
    std::string sourceType;
    std::vector<FileRecord> files;
    std::uint64_t totalBytes = 0;
};

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), required,
        nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty())
        return {};
    const int required = MultiByteToWideChar(CP_UTF8,
        MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        result.data(), required);
    return result;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

std::string JsonEscape(std::string_view value)
{
    std::string result;
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                char escaped[7]{};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                result += escaped;
            }
            else
            {
                result.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return result;
}

std::string TimestampToken()
{
    static std::atomic_uint64_t sequence{0};
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer),
        "%04u%02u%02u-%02u%02u%02u-%03u-%lu-%llu-%llu",
        time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, GetCurrentProcessId(),
        static_cast<unsigned long long>(GetTickCount64()),
        static_cast<unsigned long long>(
            sequence.fetch_add(1, std::memory_order_relaxed)));
    return buffer;
}

std::string DisplayTimestamp()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer),
        "%04u-%02u-%02u %02u:%02u:%02u",
        time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

std::filesystem::path ExtendedLengthPath(
    const std::filesystem::path& path, std::error_code& error)
{
    error.clear();
    const auto absolute = std::filesystem::absolute(path, error);
    if (error)
        return {};

    const std::wstring value = absolute.lexically_normal().wstring();
    if (value.starts_with(LR"(\\?\)"))
        return std::filesystem::path(value);
    if (value.starts_with(LR"(\\)"))
    {
        return std::filesystem::path(
            std::wstring(LR"(\\?\UNC\)") + value.substr(2));
    }
    return std::filesystem::path(
        std::wstring(LR"(\\?\)") + value);
}

std::uintmax_t RemoveTree(
    const std::filesystem::path& path, std::error_code& error)
{
    const auto extended = ExtendedLengthPath(path, error);
    if (error)
        return 0;
    return std::filesystem::remove_all(extended, error);
}

bool IsReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool LooksLikeDataDirectory(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_directory(path, error) ||
        IsReparsePoint(path))
    {
        return false;
    }
    return std::filesystem::is_regular_file(
               path / L"SnowDesktop.layout.json", error) ||
        std::filesystem::is_regular_file(
            path / L"SnowDesktop.general.json", error) ||
        std::filesystem::is_directory(path / L"widgets", error) ||
        std::filesystem::is_directory(path / L"backups", error);
}

bool IsExcludedDataPath(const std::filesystem::path& relative)
{
    if (relative.empty())
        return false;
    auto part = relative.begin();
    const std::wstring first = part->wstring();
    if (_wcsicmp(first.c_str(), L"crashdumps") == 0)
        return true;
    if (_wcsicmp(first.c_str(), L"SnowDesktop.log") == 0 ||
        _wcsicmp(first.c_str(), L"SnowDesktop.log.1") == 0 ||
        _wcsicmp(first.c_str(), L"SnowDesktop_crash.log") == 0)
        return true;
    if (_wcsicmp(first.c_str(), L"widgets") == 0)
    {
        ++part;
        if (part != relative.end())
        {
            const std::wstring second = part->wstring();
            if (_wcsicmp(second.c_str(), L"staging") == 0 ||
                _wcsicmp(second.c_str(), L"quarantine") == 0)
            {
                return true;
            }
        }
    }
    return false;
}

std::string RelativePathUtf8(
    const std::filesystem::path& path,
    const std::filesystem::path& root,
    std::error_code& error)
{
    error.clear();
    const auto relative = path.lexically_relative(root);
    if (relative.empty() ||
        relative.native().starts_with(L".."))
    {
        error = std::make_error_code(std::errc::invalid_argument);
        return {};
    }
    std::string result = WideToUtf8(relative.generic_wstring());
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool IsSafeArchivePath(std::string_view name)
{
    if (name.empty() || name.size() > 4096 ||
        name.front() == '/' || name.front() == '\\' ||
        name.find('\0') != std::string_view::npos ||
        name.find(':') != std::string_view::npos)
    {
        return false;
    }
    std::size_t begin = 0;
    while (begin <= name.size())
    {
        const std::size_t end = name.find_first_of("/\\", begin);
        const std::string_view part = name.substr(begin,
            (end == std::string_view::npos ? name.size() : end) - begin);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return true;
}

std::string Sha256File(const std::filesystem::path& path,
    const CancellationContext& cancellation, bool& cancelled,
    std::string& error)
{
    if (StopRequested(cancellation, cancelled, error))
        return {};
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    DWORD hashSize = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm,
            BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &resultSize, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
            &resultSize, 0) < 0)
    {
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    std::vector<UCHAR> object(objectSize);
    std::vector<UCHAR> digest(hashSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
            nullptr, 0, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    std::array<char, 64 * 1024> buffer{};
    while (file)
    {
        if (StopRequested(cancellation, cancelled, error))
        {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }
        file.read(buffer.data(), buffer.size());
        const auto count = file.gcount();
        if (StopRequested(cancellation, cancelled, error))
        {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }
        if (count > 0 &&
            BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count), 0) < 0)
        {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }
    }

    const bool ok =
        BCryptFinishHash(hash, digest.data(), hashSize, 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok)
        return {};

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const UCHAR byte : digest)
        output << std::setw(2) << static_cast<int>(byte);
    return output.str();
}

bool CollectRecords(const std::filesystem::path& data,
    std::vector<FileRecord>& records, std::uint64_t& totalBytes,
    std::string& error, const CancellationContext& cancellation,
    bool& cancelled)
{
    records.clear();
    totalBytes = 0;
    std::error_code filesystemError;
    const auto extendedData =
        ExtendedLengthPath(data, filesystemError);
    if (filesystemError)
    {
        error = "cannot resolve backup data path: " +
            filesystemError.message();
        return false;
    }
    for (std::filesystem::recursive_directory_iterator entry(
            extendedData, filesystemError), end;
        !filesystemError && entry != end;
        entry.increment(filesystemError))
    {
        if (StopRequested(cancellation, cancelled, error))
            return false;
        if (IsReparsePoint(entry->path()))
        {
            error = "backup data contains a reparse point";
            return false;
        }
        if (!entry->is_regular_file(filesystemError))
            continue;

        FileRecord record;
        record.path = RelativePathUtf8(
            entry->path(), extendedData, filesystemError);
        if (filesystemError || !IsSafeArchivePath(record.path))
        {
            error = "backup data contains an unsafe path";
            return false;
        }
        record.size =
            std::filesystem::file_size(entry->path(), filesystemError);
        if (filesystemError || record.size > kMaxFileBytes ||
            totalBytes > kMaxExtractedBytes - record.size)
        {
            error = "backup data exceeds size limits";
            return false;
        }
        record.sha256 = Sha256File(entry->path(), cancellation,
            cancelled, error);
        if (record.sha256.empty())
        {
            if (!cancelled)
                error = "cannot hash backup file: " + record.path;
            return false;
        }
        totalBytes += record.size;
        records.push_back(std::move(record));
        if (records.size() > kMaxFiles)
        {
            error = "backup data contains too many files";
            return false;
        }
    }
    if (filesystemError)
    {
        error = "cannot enumerate backup data: " +
            filesystemError.message();
        return false;
    }
    std::sort(records.begin(), records.end(),
        [](const FileRecord& left, const FileRecord& right) {
            return left.path < right.path;
        });
    return !records.empty();
}

std::string ManifestJson(const BackupManifest& manifest)
{
    std::ostringstream output;
    output << "{\n"
        << "  \"schemaVersion\": " << manifest.schemaVersion << ",\n"
        << "  \"id\": \"" << JsonEscape(manifest.id) << "\",\n"
        << "  \"createdAt\": \"" << JsonEscape(manifest.createdAt)
        << "\",\n"
        << "  \"hostVersion\": \"" << JsonEscape(manifest.hostVersion)
        << "\",\n"
        << "  \"sourceType\": \"" << JsonEscape(manifest.sourceType)
        << "\",\n"
        << "  \"fileCount\": " << manifest.files.size() << ",\n"
        << "  \"totalBytes\": " << manifest.totalBytes << ",\n"
        << "  \"files\": [\n";
    for (std::size_t index = 0; index < manifest.files.size(); ++index)
    {
        const auto& file = manifest.files[index];
        output << "    {\"path\":\"" << JsonEscape(file.path)
            << "\",\"size\":" << file.size
            << ",\"sha256\":\"" << file.sha256 << "\"}";
        if (index + 1 < manifest.files.size())
            output << ',';
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

bool WriteTextFile(const std::filesystem::path& path,
    const std::string& text, std::string& error)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output)
    {
        error = "cannot write backup manifest";
        return false;
    }
    return true;
}

bool ReadTextFile(const std::filesystem::path& path,
    std::string& text, std::uint64_t limit = 16ull * 1024ull * 1024ull)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > limit)
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    text.assign(std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool ReadUnsigned(const JsonValue& object, const char* name,
    std::uint64_t& value)
{
    const JsonValue* item = object.Find(name);
    if (!item || !item->IsNumber() || item->number < 0 ||
        item->number > static_cast<double>(
            (std::numeric_limits<std::uint64_t>::max)()) ||
        item->number != std::floor(item->number))
    {
        return false;
    }
    value = static_cast<std::uint64_t>(item->number);
    return true;
}

bool ReadManifest(const std::filesystem::path& path,
    BackupManifest& manifest, std::string& error)
{
    std::string text;
    if (!ReadTextFile(path, text))
    {
        error = "cannot read backup manifest";
        return false;
    }
    JsonValue root;
    if (!ParseJson(text, root) || !root.IsObject())
    {
        error = "backup manifest is invalid JSON";
        return false;
    }
    const JsonValue* schema = root.Find("schemaVersion");
    const JsonValue* id = root.Find("id");
    const JsonValue* createdAt = root.Find("createdAt");
    const JsonValue* hostVersion = root.Find("hostVersion");
    const JsonValue* sourceType = root.Find("sourceType");
    const JsonValue* files = root.Find("files");
    if (!schema || !schema->IsNumber() ||
        schema->number != kBackupSchemaVersion ||
        !id || !id->IsString() || id->string.empty() ||
        !createdAt || !createdAt->IsString() ||
        !hostVersion || !hostVersion->IsString() ||
        !sourceType || !sourceType->IsString() ||
        !files || !files->IsArray() ||
        files->array.empty() || files->array.size() > kMaxFiles)
    {
        error = "backup manifest fields are invalid";
        return false;
    }

    manifest = {};
    manifest.id = id->string;
    manifest.createdAt = createdAt->string;
    manifest.hostVersion = hostVersion->string;
    manifest.sourceType = sourceType->string;
    std::set<std::string> paths;
    for (const JsonValue& item : files->array)
    {
        if (!item.IsObject())
        {
            error = "backup manifest file record is invalid";
            return false;
        }
        const JsonValue* recordPath = item.Find("path");
        const JsonValue* hash = item.Find("sha256");
        FileRecord record;
        if (!recordPath || !recordPath->IsString() ||
            !hash || !hash->IsString() ||
            !ReadUnsigned(item, "size", record.size) ||
            !IsSafeArchivePath(recordPath->string) ||
            hash->string.size() != 64 ||
            !std::all_of(hash->string.begin(), hash->string.end(),
                [](unsigned char ch) {
                    return std::isxdigit(ch) != 0;
                }))
        {
            error = "backup manifest file record fields are invalid";
            return false;
        }
        record.path = recordPath->string;
        record.sha256 = LowerAscii(hash->string);
        const std::string canonical = LowerAscii(record.path);
        if (!paths.insert(canonical).second ||
            record.size > kMaxFileBytes ||
            manifest.totalBytes >
                kMaxExtractedBytes - record.size)
        {
            error = "backup manifest exceeds limits or contains duplicates";
            return false;
        }
        manifest.totalBytes += record.size;
        manifest.files.push_back(std::move(record));
    }

    std::uint64_t declaredCount = 0;
    std::uint64_t declaredBytes = 0;
    if (!ReadUnsigned(root, "fileCount", declaredCount) ||
        !ReadUnsigned(root, "totalBytes", declaredBytes) ||
        declaredCount != manifest.files.size() ||
        declaredBytes != manifest.totalBytes)
    {
        error = "backup manifest totals do not match its file records";
        return false;
    }
    std::sort(manifest.files.begin(), manifest.files.end(),
        [](const FileRecord& left, const FileRecord& right) {
            return left.path < right.path;
        });
    return true;
}

bool ValidateBackupDirectory(const std::filesystem::path& root,
    BackupManifest& manifest, std::string& error,
    const CancellationContext& cancellation, bool& cancelled)
{
    if (StopRequested(cancellation, cancelled, error))
        return false;
    const auto data = root / L"data";
    if (!LooksLikeDataDirectory(data) ||
        !ReadManifest(root / kManifestName, manifest, error))
    {
        if (error.empty())
            error = "backup does not contain valid SnowDesktop data";
        return false;
    }

    std::error_code rootError;
    for (std::filesystem::directory_iterator entry(
            root, rootError), end;
        !rootError && entry != end;
        entry.increment(rootError))
    {
        const auto name = entry->path().filename();
        if (name == kManifestName)
        {
            if (!entry->is_regular_file(rootError) ||
                IsReparsePoint(entry->path()))
            {
                error = "backup manifest is not a regular file";
                return false;
            }
            continue;
        }
        if (name == L"data")
        {
            if (!entry->is_directory(rootError) ||
                IsReparsePoint(entry->path()))
            {
                error = "backup data is not a regular directory";
                return false;
            }
            continue;
        }
        error = "backup contains an unexpected root entry";
        return false;
    }
    if (rootError)
    {
        error = "cannot enumerate backup root: " +
            rootError.message();
        return false;
    }

    std::vector<FileRecord> actual;
    std::uint64_t actualBytes = 0;
    if (!CollectRecords(data, actual, actualBytes, error,
            cancellation, cancelled))
        return false;
    if (actual.size() != manifest.files.size() ||
        actualBytes != manifest.totalBytes)
    {
        error = "backup files do not match the manifest totals";
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index)
    {
        if (actual[index].path != manifest.files[index].path ||
            actual[index].size != manifest.files[index].size ||
            actual[index].sha256 != manifest.files[index].sha256)
        {
            error = "backup file hash mismatch: " + actual[index].path;
            return false;
        }
    }
    return true;
}

bool CopyDataForBackup(const std::filesystem::path& source,
    const std::filesystem::path& destination, std::string& error,
    const CancellationContext& cancellation, bool& cancelled)
{
    if (StopRequested(cancellation, cancelled, error))
        return false;
    if (!LooksLikeDataDirectory(source) || IsReparsePoint(source))
    {
        error = "source is not a valid SnowDesktop data directory";
        return false;
    }
    std::error_code filesystemError;
    const auto extendedSource =
        ExtendedLengthPath(source, filesystemError);
    if (filesystemError)
    {
        error = "cannot resolve backup source: " +
            filesystemError.message();
        return false;
    }
    const auto extendedDestination =
        ExtendedLengthPath(destination, filesystemError);
    if (filesystemError)
    {
        error = "cannot resolve backup destination: " +
            filesystemError.message();
        return false;
    }
    std::filesystem::create_directories(
        extendedDestination, filesystemError);
    if (filesystemError)
    {
        error = "cannot create backup data directory: " +
            filesystemError.message();
        return false;
    }
    for (std::filesystem::recursive_directory_iterator entry(
            extendedSource, filesystemError), end;
        !filesystemError && entry != end;
        entry.increment(filesystemError))
    {
        if (StopRequested(cancellation, cancelled, error))
            return false;
        const auto relative =
            entry->path().lexically_relative(extendedSource);
        if (relative.empty() ||
            relative.native().starts_with(L".."))
        {
            error = "backup source path escaped its root";
            return false;
        }
        if (filesystemError)
            break;
        if (IsExcludedDataPath(relative))
        {
            if (entry->is_directory(filesystemError))
                entry.disable_recursion_pending();
            continue;
        }
        if (IsReparsePoint(entry->path()))
        {
            error = "backup source contains a reparse point";
            return false;
        }
        const auto target = extendedDestination / relative;
        if (entry->is_directory(filesystemError))
        {
            std::filesystem::create_directories(target, filesystemError);
        }
        else if (entry->is_regular_file(filesystemError))
        {
            const auto size =
                std::filesystem::file_size(entry->path(), filesystemError);
            if (!filesystemError && size > kMaxFileBytes)
            {
                error = "backup source contains an oversized file";
                return false;
            }
            std::filesystem::create_directories(
                target.parent_path(), filesystemError);
            if (!filesystemError)
            {
                std::ifstream input(entry->path(), std::ios::binary);
                std::ofstream output(target,
                    std::ios::binary | std::ios::trunc);
                if (!input || !output)
                {
                    error = "cannot open backup source or destination file";
                    return false;
                }
                std::array<char, 64 * 1024> buffer{};
                while (input)
                {
                    if (StopRequested(
                            cancellation, cancelled, error))
                    {
                        return false;
                    }
                    input.read(buffer.data(), buffer.size());
                    const auto count = input.gcount();
                    if (StopRequested(
                            cancellation, cancelled, error))
                    {
                        return false;
                    }
                    if (count > 0)
                        output.write(buffer.data(), count);
                }
                output.flush();
                if (!input.eof() || !output)
                {
                    error = "cannot copy backup data file";
                    return false;
                }
            }
        }
        else
        {
            error = "backup source contains an unsupported file type";
            return false;
        }
        if (filesystemError)
            break;
    }
    if (filesystemError)
    {
        error = "cannot copy backup data: " +
            filesystemError.message();
        return false;
    }
    return true;
}

bool BuildBackupDirectory(const std::filesystem::path& sourceData,
    const std::filesystem::path& destination,
    const std::string& id, const std::string& hostVersion,
    const std::string& sourceType, BackupInfo& info,
    std::string& error, const CancellationContext& cancellation,
    bool& cancelled)
{
    if (!CopyDataForBackup(
            sourceData, destination / L"data", error,
            cancellation, cancelled))
    {
        return false;
    }
    BackupManifest manifest;
    manifest.id = id;
    manifest.createdAt = DisplayTimestamp();
    manifest.hostVersion = hostVersion;
    manifest.sourceType = sourceType;
    if (!CollectRecords(destination / L"data",
            manifest.files, manifest.totalBytes, error,
            cancellation, cancelled) ||
        !WriteTextFile(destination / kManifestName,
            ManifestJson(manifest), error))
    {
        return false;
    }
    info.id = Utf8ToWide(id);
    info.root = destination;
    info.data = destination / L"data";
    info.createdAt = manifest.createdAt;
    info.hostVersion = manifest.hostVersion;
    info.sourceType = manifest.sourceType;
    info.fileCount = manifest.files.size();
    info.totalBytes = manifest.totalBytes;
    return true;
}

std::uint32_t Crc32Update(std::uint32_t crc,
    const unsigned char* data, std::size_t size)
{
    for (std::size_t index = 0; index < size; ++index)
    {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^
                (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

void Write16(std::ostream& output, std::uint16_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
    };
    output.write(bytes, 2);
}

void Write32(std::ostream& output, std::uint32_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    output.write(bytes, 4);
}

std::uint16_t Read16(const unsigned char* data)
{
    return static_cast<std::uint16_t>(
        data[0] | (static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint32_t Read32(const unsigned char* data)
{
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

struct ZipItem
{
    std::string path;
    std::filesystem::path source;
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    std::uint32_t offset = 0;
};

bool CalculateCrc(const std::filesystem::path& path,
    std::uint32_t& crc, std::string& error,
    const CancellationContext& cancellation, bool& cancelled)
{
    if (StopRequested(cancellation, cancelled, error))
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "cannot read backup archive source";
        return false;
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    std::uint32_t state = 0xffffffffu;
    while (input)
    {
        if (StopRequested(cancellation, cancelled, error))
            return false;
        input.read(reinterpret_cast<char*>(buffer.data()),
            buffer.size());
        const auto count = input.gcount();
        if (StopRequested(cancellation, cancelled, error))
            return false;
        if (count > 0)
            state = Crc32Update(state, buffer.data(),
                static_cast<std::size_t>(count));
    }
    crc = ~state;
    return true;
}

bool WriteStoreZip(const std::filesystem::path& root,
    const std::filesystem::path& output, std::string& error,
    const CancellationContext& cancellation, bool& cancelled)
{
    if (StopRequested(cancellation, cancelled, error))
        return false;
    std::vector<ZipItem> items;
    std::uint64_t archivePayload = 0;
    std::error_code filesystemError;
    const auto extendedRoot =
        ExtendedLengthPath(root, filesystemError);
    if (filesystemError)
    {
        error = "cannot resolve backup archive source: " +
            filesystemError.message();
        return false;
    }
    for (std::filesystem::recursive_directory_iterator entry(
            extendedRoot, filesystemError), end;
        !filesystemError && entry != end;
        entry.increment(filesystemError))
    {
        if (StopRequested(cancellation, cancelled, error))
            return false;
        if (IsReparsePoint(entry->path()))
        {
            error = "backup archive source contains a reparse point";
            return false;
        }
        if (!entry->is_regular_file(filesystemError))
            continue;
        ZipItem item;
        item.path = RelativePathUtf8(
            entry->path(), extendedRoot, filesystemError);
        item.source = entry->path();
        item.size =
            std::filesystem::file_size(entry->path(), filesystemError);
        if (filesystemError || !IsSafeArchivePath(item.path) ||
            item.size > (std::numeric_limits<std::uint32_t>::max)() ||
            archivePayload > kMaxArchiveBytes - item.size ||
            !CalculateCrc(item.source, item.crc, error,
                cancellation, cancelled))
        {
            if (error.empty())
                error = "backup archive exceeds ZIP32 limits";
            return false;
        }
        archivePayload += item.size;
        items.push_back(std::move(item));
        if (items.size() > kMaxFiles)
        {
            error = "backup archive contains too many files";
            return false;
        }
    }
    if (filesystemError || items.empty())
    {
        error = filesystemError
            ? "cannot enumerate backup archive source: " +
                filesystemError.message()
            : "backup archive contains no files";
        return false;
    }
    std::sort(items.begin(), items.end(),
        [](const ZipItem& left, const ZipItem& right) {
            return left.path < right.path;
        });

    const auto extendedOutput =
        ExtendedLengthPath(output, filesystemError);
    if (filesystemError)
    {
        error = "cannot resolve backup archive path: " +
            filesystemError.message();
        return false;
    }
    if (!extendedOutput.parent_path().empty())
    {
        std::filesystem::create_directories(
            extendedOutput.parent_path(), filesystemError);
        if (filesystemError)
        {
            error = "cannot create backup archive directory: " +
                filesystemError.message();
            return false;
        }
    }
    const std::filesystem::path temporary =
        extendedOutput.wstring() + L".tmp";
    std::ofstream archive(
        temporary, std::ios::binary | std::ios::trunc);
    if (!archive)
    {
        error = "cannot create backup archive";
        return false;
    }
    const auto cancelTemporary = [&]() {
        archive.close();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    };
    std::array<char, 64 * 1024> buffer{};
    for (auto& item : items)
    {
        if (StopRequested(cancellation, cancelled, error))
            return cancelTemporary();
        const auto position = archive.tellp();
        if (position < 0 ||
            position > (std::numeric_limits<std::uint32_t>::max)() ||
            item.path.size() >
                (std::numeric_limits<std::uint16_t>::max)())
        {
            error = "backup archive exceeds ZIP32 limits";
            return false;
        }
        item.offset = static_cast<std::uint32_t>(position);
        Write32(archive, 0x04034b50);
        Write16(archive, 20);
        Write16(archive, 0x0800);
        Write16(archive, 0);
        Write16(archive, 0);
        Write16(archive, 0);
        Write32(archive, item.crc);
        Write32(archive, static_cast<std::uint32_t>(item.size));
        Write32(archive, static_cast<std::uint32_t>(item.size));
        Write16(archive, static_cast<std::uint16_t>(item.path.size()));
        Write16(archive, 0);
        archive.write(item.path.data(),
            static_cast<std::streamsize>(item.path.size()));

        std::ifstream source(item.source, std::ios::binary);
        while (source)
        {
            if (StopRequested(cancellation, cancelled, error))
                return cancelTemporary();
            source.read(buffer.data(), buffer.size());
            const auto count = source.gcount();
            if (StopRequested(cancellation, cancelled, error))
                return cancelTemporary();
            if (count > 0)
                archive.write(buffer.data(), count);
        }
        if (!source.eof() || !archive)
        {
            error = "cannot write backup archive payload";
            return false;
        }
    }

    const auto centralOffsetValue = archive.tellp();
    if (centralOffsetValue < 0 ||
        centralOffsetValue >
            (std::numeric_limits<std::uint32_t>::max)())
    {
        error = "backup archive exceeds ZIP32 limits";
        return false;
    }
    const auto centralOffset =
        static_cast<std::uint32_t>(centralOffsetValue);
    for (const auto& item : items)
    {
        if (StopRequested(cancellation, cancelled, error))
            return cancelTemporary();
        Write32(archive, 0x02014b50);
        Write16(archive, 20);
        Write16(archive, 20);
        Write16(archive, 0x0800);
        Write16(archive, 0);
        Write16(archive, 0);
        Write16(archive, 0);
        Write32(archive, item.crc);
        Write32(archive, static_cast<std::uint32_t>(item.size));
        Write32(archive, static_cast<std::uint32_t>(item.size));
        Write16(archive, static_cast<std::uint16_t>(item.path.size()));
        Write16(archive, 0);
        Write16(archive, 0);
        Write16(archive, 0);
        Write16(archive, 0);
        Write32(archive, 0);
        Write32(archive, item.offset);
        archive.write(item.path.data(),
            static_cast<std::streamsize>(item.path.size()));
    }

    const auto endValue = archive.tellp();
    if (endValue < 0 ||
        endValue > (std::numeric_limits<std::uint32_t>::max)() ||
        items.size() >
            (std::numeric_limits<std::uint16_t>::max)())
    {
        error = "backup archive exceeds ZIP32 limits";
        return false;
    }
    const auto centralSize =
        static_cast<std::uint32_t>(endValue) - centralOffset;
    Write32(archive, 0x06054b50);
    Write16(archive, 0);
    Write16(archive, 0);
    Write16(archive, static_cast<std::uint16_t>(items.size()));
    Write16(archive, static_cast<std::uint16_t>(items.size()));
    Write32(archive, centralSize);
    Write32(archive, centralOffset);
    Write16(archive, 0);
    archive.flush();
    if (!archive)
    {
        error = "cannot finish backup archive";
        archive.close();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    archive.close();
    if (!BeginNonInterruptible(cancellation, cancelled, error))
    {
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), extendedOutput.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "cannot publish backup archive: " +
            std::to_string(GetLastError());
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    return true;
}

bool ExtractStoreZip(const std::filesystem::path& archivePath,
    const std::filesystem::path& destination, std::string& error,
    const CancellationContext& cancellation, bool& cancelled)
{
    if (StopRequested(cancellation, cancelled, error))
        return false;
    std::error_code filesystemError;
    const auto extendedArchive =
        ExtendedLengthPath(archivePath, filesystemError);
    if (filesystemError)
    {
        error = "cannot resolve backup archive path";
        return false;
    }
    const auto archiveSize =
        std::filesystem::file_size(
            extendedArchive, filesystemError);
    if (filesystemError || archiveSize == 0 ||
        archiveSize > kMaxArchiveBytes)
    {
        error = "backup archive is missing or exceeds 2 GiB";
        return false;
    }
    std::ifstream archive(extendedArchive, std::ios::binary);
    if (!archive)
    {
        error = "cannot open backup archive";
        return false;
    }
    const auto extendedDestination =
        ExtendedLengthPath(destination, filesystemError);
    if (filesystemError)
    {
        error = "cannot resolve backup extraction directory";
        return false;
    }
    std::filesystem::create_directories(
        extendedDestination, filesystemError);
    if (filesystemError)
    {
        error = "cannot create backup extraction directory";
        return false;
    }

    std::set<std::string> names;
    std::size_t fileCount = 0;
    std::uint64_t totalBytes = 0;
    std::array<unsigned char, 30> header{};
    std::array<unsigned char, 64 * 1024> buffer{};
    while (archive)
    {
        if (StopRequested(cancellation, cancelled, error))
            return false;
        const auto headerPosition = archive.tellg();
        archive.read(reinterpret_cast<char*>(header.data()), 4);
        if (archive.gcount() == 0)
            break;
        if (archive.gcount() != 4)
        {
            error = "backup archive has a truncated ZIP header";
            return false;
        }
        const std::uint32_t signature = Read32(header.data());
        if (signature == 0x02014b50 || signature == 0x06054b50)
            break;
        if (signature != 0x04034b50)
        {
            error = "backup archive contains an invalid ZIP record";
            return false;
        }
        archive.seekg(headerPosition);
        archive.read(reinterpret_cast<char*>(header.data()),
            header.size());
        if (archive.gcount() !=
            static_cast<std::streamsize>(header.size()))
        {
            error = "backup archive has a truncated local header";
            return false;
        }

        const std::uint16_t flags = Read16(header.data() + 6);
        const std::uint16_t method = Read16(header.data() + 8);
        const std::uint32_t expectedCrc =
            Read32(header.data() + 14);
        const std::uint32_t compressedSize =
            Read32(header.data() + 18);
        const std::uint32_t uncompressedSize =
            Read32(header.data() + 22);
        const std::uint16_t nameSize =
            Read16(header.data() + 26);
        const std::uint16_t extraSize =
            Read16(header.data() + 28);
        if ((flags & 0x0001) != 0 || (flags & 0x0008) != 0 ||
            method != 0 || compressedSize != uncompressedSize)
        {
            error = "backup ZIP must use unencrypted store entries";
            return false;
        }
        if (nameSize == 0 || nameSize > 4096 ||
            uncompressedSize > kMaxFileBytes ||
            totalBytes > kMaxExtractedBytes - uncompressedSize)
        {
            error = "backup archive entry exceeds extraction limits";
            return false;
        }

        std::string name(nameSize, '\0');
        archive.read(name.data(), nameSize);
        archive.seekg(extraSize, std::ios::cur);
        if (!archive || !IsSafeArchivePath(
                name.ends_with('/') ? name.substr(0, name.size() - 1) :
                    name))
        {
            error = "backup archive contains an unsafe path";
            return false;
        }
        std::replace(name.begin(), name.end(), '\\', '/');
        const bool directory = name.ends_with('/');
        const std::string canonical = LowerAscii(name);
        if (!names.insert(canonical).second)
        {
            error = "backup archive contains a duplicate path";
            return false;
        }

        std::filesystem::path relative =
            Utf8ToWide(directory ? name.substr(0, name.size() - 1) :
                name);
        if (relative.empty())
        {
            error = "backup archive path is not valid UTF-8";
            return false;
        }
        relative.make_preferred();
        const auto target = extendedDestination / relative;
        if (directory)
        {
            std::filesystem::create_directories(
                target, filesystemError);
            if (filesystemError)
            {
                error = "cannot create extracted backup directory";
                return false;
            }
            continue;
        }

        ++fileCount;
        totalBytes += uncompressedSize;
        if (fileCount > kMaxFiles)
        {
            error = "backup archive contains too many files";
            return false;
        }
        std::filesystem::create_directories(
            target.parent_path(), filesystemError);
        if (filesystemError)
        {
            error = "cannot create extracted backup parent";
            return false;
        }
        HANDLE output = CreateFileW(target.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE)
        {
            error = "cannot create extracted backup file: " +
                std::to_string(GetLastError()) + " [" +
                WideToUtf8(target.wstring()) + "]";
            return false;
        }
        std::uint64_t remaining = uncompressedSize;
        std::uint32_t crc = 0xffffffffu;
        while (remaining > 0)
        {
            if (StopRequested(cancellation, cancelled, error))
            {
                CloseHandle(output);
                return false;
            }
            const auto chunk = static_cast<std::streamsize>(
                (std::min<std::uint64_t>)(remaining, buffer.size()));
            archive.read(
                reinterpret_cast<char*>(buffer.data()), chunk);
            if (archive.gcount() != chunk)
            {
                CloseHandle(output);
                error = "backup archive entry is truncated";
                return false;
            }
            if (StopRequested(cancellation, cancelled, error))
            {
                CloseHandle(output);
                return false;
            }
            DWORD written = 0;
            if (!WriteFile(output, buffer.data(),
                    static_cast<DWORD>(chunk), &written, nullptr) ||
                written != static_cast<DWORD>(chunk))
            {
                const DWORD writeError = GetLastError();
                CloseHandle(output);
                error = "cannot write extracted backup file: " +
                    std::to_string(writeError);
                return false;
            }
            crc = Crc32Update(
                crc, buffer.data(), static_cast<std::size_t>(chunk));
            remaining -= static_cast<std::uint64_t>(chunk);
        }
        const bool closed = CloseHandle(output) != FALSE;
        if (!closed || ~crc != expectedCrc)
        {
            error = !closed
                ? "cannot finish extracted backup file"
                : "backup archive entry CRC mismatch";
            return false;
        }
    }
    if (fileCount == 0)
    {
        error = "backup archive contains no files";
        return false;
    }
    return true;
}

bool IsContainedPath(const std::filesystem::path& child,
    const std::filesystem::path& parent)
{
    std::error_code error;
    const auto canonicalChild =
        std::filesystem::weakly_canonical(child, error);
    if (error)
        return false;
    const auto canonicalParent =
        std::filesystem::weakly_canonical(parent, error);
    if (error)
        return false;
    auto childPart = canonicalChild.begin();
    for (auto parentPart = canonicalParent.begin();
        parentPart != canonicalParent.end();
        ++parentPart, ++childPart)
    {
        if (childPart == canonicalChild.end() ||
            _wcsicmp(parentPart->wstring().c_str(),
                childPart->wstring().c_str()) != 0)
        {
            return false;
        }
    }
    return true;
}

BackupInfo ManifestToInfo(const std::filesystem::path& root,
    const BackupManifest& manifest)
{
    BackupInfo info;
    info.id = Utf8ToWide(manifest.id);
    info.root = root;
    info.data = root / L"data";
    info.createdAt = manifest.createdAt;
    info.hostVersion = manifest.hostVersion;
    info.sourceType = manifest.sourceType;
    info.fileCount = manifest.files.size();
    info.totalBytes = manifest.totalBytes;
    return info;
}

OperationResult QueueDataDirectory(
    const std::filesystem::path& stateRoot,
    const std::filesystem::path& sourceData,
    const CancellationContext& cancellation)
{
    OperationResult result;
    if (StopRequested(cancellation, result.cancelled, result.error))
        return result;
    if (!LooksLikeDataDirectory(sourceData))
    {
        result.error =
            "selected backup does not contain valid SnowDesktop data";
        return result;
    }
    const std::string token = TimestampToken();
    const std::wstring wideToken = Utf8ToWide(token);
    const auto staging = stateRoot / L"TempState" /
        L"PortableMigration" / (L"staging-" + wideToken);
    const auto copy =
        snowdesktop::migration::CopyDataTree(
            sourceData, staging, cancellation);
    if (!copy.ok)
    {
        result.cancelled = copy.cancelled;
        result.error = copy.error;
        std::error_code cleanupError;
        RemoveTree(staging, cleanupError);
        return result;
    }
    std::string queueError;
    bool queueCancelled = false;
    if (!snowdesktop::migration::Queue(
            stateRoot, wideToken, queueError,
            cancellation, &queueCancelled))
    {
        std::error_code cleanupError;
        RemoveTree(staging, cleanupError);
        result.cancelled = queueCancelled;
        result.error = queueError;
        return result;
    }
    result.ok = true;
    result.backup.data = sourceData;
    result.backup.fileCount = copy.files;
    result.backup.totalBytes = copy.bytes;
    return result;
}
}

FullDataBackupManager::FullDataBackupManager(
    std::filesystem::path stateRoot,
    std::filesystem::path activeData,
    std::string hostVersion,
    std::string sourceType)
    : stateRoot_(std::move(stateRoot)),
      activeData_(std::move(activeData)),
      backupRoot_(stateRoot_ / L"FullBackups"),
      hostVersion_(std::move(hostVersion)),
      sourceType_(std::move(sourceType))
{
}

std::vector<BackupInfo> FullDataBackupManager::List() const
{
    std::vector<BackupInfo> result;
    std::error_code error;
    for (std::filesystem::directory_iterator entry(
            backupRoot_, error), end;
        !error && entry != end;
        entry.increment(error))
    {
        if (!entry->is_directory(error) ||
            IsReparsePoint(entry->path()))
        {
            continue;
        }
        BackupManifest manifest;
        std::string manifestError;
        if (ReadManifest(entry->path() / kManifestName,
                manifest, manifestError) &&
            LooksLikeDataDirectory(entry->path() / L"data"))
        {
            result.push_back(
                ManifestToInfo(entry->path(), manifest));
        }
    }

    error.clear();
    const auto migrationRoot =
        stateRoot_ / L"TempState" / L"PortableMigration";
    for (std::filesystem::directory_iterator entry(
            migrationRoot, error), end;
        !error && entry != end;
        entry.increment(error))
    {
        if (!entry->is_directory(error) ||
            IsReparsePoint(entry->path()) ||
            !entry->path().filename().wstring().starts_with(L"backup-") ||
            !LooksLikeDataDirectory(entry->path()))
        {
            continue;
        }
        BackupInfo info;
        info.id = entry->path().filename().wstring();
        info.root = entry->path();
        info.data = entry->path();
        info.sourceType = "migration";
        info.migrationRollback = true;
        const auto name = info.id.substr(7);
        if (name.size() >= 15)
        {
            info.createdAt = WideToUtf8(
                name.substr(0, 4) + L"-" + name.substr(4, 2) +
                L"-" + name.substr(6, 2) + L" " +
                name.substr(9, 2) + L":" + name.substr(11, 2) +
                L":" + name.substr(13, 2));
        }
        result.push_back(std::move(info));
    }
    std::sort(result.begin(), result.end(),
        [](const BackupInfo& left, const BackupInfo& right) {
            return left.createdAt > right.createdAt;
        });
    return result;
}

OperationResult FullDataBackupManager::Create(
    const CancellationContext& cancellation)
{
    OperationResult result;
    if (StopRequested(cancellation, result.cancelled, result.error))
        return result;
    const std::string token = TimestampToken();
    const auto staging =
        backupRoot_ / (L".staging-" + Utf8ToWide(token));
    const auto destination =
        backupRoot_ / (L"backup-" + Utf8ToWide(token));
    std::error_code error;
    std::filesystem::create_directories(backupRoot_, error);
    if (error)
    {
        result.error = "cannot create complete backup directory: " +
            error.message();
        return result;
    }
    BackupInfo info;
    if (!BuildBackupDirectory(activeData_, staging, token,
            hostVersion_, sourceType_, info, result.error,
            cancellation, result.cancelled))
    {
        RemoveTree(staging, error);
        return result;
    }
    if (!BeginNonInterruptible(
            cancellation, result.cancelled, result.error))
    {
        RemoveTree(staging, error);
        return result;
    }
    std::filesystem::rename(staging, destination, error);
    if (error)
    {
        result.error = "cannot publish complete backup: " +
            error.message();
        RemoveTree(staging, error);
        return result;
    }
    info.root = destination;
    info.data = destination / L"data";
    result.ok = true;
    result.backup = std::move(info);
    return result;
}

OperationResult FullDataBackupManager::Export(
    const BackupInfo& backup,
    const std::filesystem::path& archive,
    const CancellationContext& cancellation) const
{
    OperationResult result;
    if (StopRequested(cancellation, result.cancelled, result.error))
        return result;
    if (archive.empty())
    {
        result.error = "backup archive path is empty";
        return result;
    }
    const std::string extension =
        LowerAscii(archive.extension().string());
    if (extension != ".snowbackup" && extension != ".zip")
    {
        result.error = "backup archive must use .snowbackup or .zip";
        return result;
    }

    std::filesystem::path exportRoot = backup.root;
    std::filesystem::path temporaryRoot;
    std::error_code error;
    if (backup.migrationRollback)
    {
        const std::string token = TimestampToken();
        temporaryRoot = stateRoot_ / L"TempState" /
            L"BackupExport" / Utf8ToWide(token);
        BackupInfo generated;
        if (!BuildBackupDirectory(backup.data, temporaryRoot,
                token, hostVersion_, "migration", generated,
                result.error, cancellation, result.cancelled))
        {
            RemoveTree(temporaryRoot, error);
            return result;
        }
        exportRoot = temporaryRoot;
    }
    BackupManifest manifest;
    if (!ValidateBackupDirectory(exportRoot, manifest, result.error,
            cancellation, result.cancelled) ||
        !WriteStoreZip(exportRoot, archive, result.error,
            cancellation, result.cancelled))
    {
        if (!temporaryRoot.empty())
            RemoveTree(temporaryRoot, error);
        return result;
    }
    if (!temporaryRoot.empty())
        RemoveTree(temporaryRoot, error);
    result.ok = true;
    result.backup = backup;
    return result;
}

OperationResult FullDataBackupManager::QueueRestore(
    const BackupInfo& backup,
    const CancellationContext& cancellation) const
{
    OperationResult result;
    if (StopRequested(cancellation, result.cancelled, result.error))
        return result;
    if (!backup.migrationRollback)
    {
        BackupManifest manifest;
        if (!ValidateBackupDirectory(
                backup.root, manifest, result.error,
                cancellation, result.cancelled))
        {
            return result;
        }
    }
    result = QueueDataDirectory(
        stateRoot_, backup.data, cancellation);
    if (result.ok)
        result.backup = backup;
    return result;
}

OperationResult FullDataBackupManager::ImportAndQueue(
    const std::filesystem::path& archive,
    const CancellationContext& cancellation) const
{
    OperationResult result;
    if (StopRequested(cancellation, result.cancelled, result.error))
        return result;
    const std::string extension =
        LowerAscii(archive.extension().string());
    if (extension != ".snowbackup" && extension != ".zip")
    {
        result.error = "backup archive must use .snowbackup or .zip";
        return result;
    }
    const std::string token = TimestampToken();
    const auto extraction = stateRoot_ / L"TempState" /
        L"BackupImport" / Utf8ToWide(token);
    std::error_code cleanupError;
    if (!ExtractStoreZip(archive, extraction, result.error,
            cancellation, result.cancelled))
    {
        RemoveTree(extraction, cleanupError);
        return result;
    }
    BackupManifest manifest;
    if (!ValidateBackupDirectory(
            extraction, manifest, result.error,
            cancellation, result.cancelled))
    {
        RemoveTree(extraction, cleanupError);
        return result;
    }
    result = QueueDataDirectory(
        stateRoot_, extraction / L"data", cancellation);
    if (result.ok)
        result.backup = ManifestToInfo(extraction, manifest);
    RemoveTree(extraction, cleanupError);
    return result;
}

OperationResult FullDataBackupManager::QueueDirectory(
    const std::filesystem::path& sourceData,
    const CancellationContext& cancellation) const
{
    return QueueDataDirectory(stateRoot_, sourceData, cancellation);
}

OperationResult FullDataBackupManager::Delete(
    const BackupInfo& backup,
    const CancellationContext& cancellation) const
{
    OperationResult result;
    if (StopRequested(cancellation, result.cancelled, result.error))
        return result;
    const auto allowedRoot = backup.migrationRollback
        ? stateRoot_ / L"TempState" / L"PortableMigration"
        : backupRoot_;
    if (backup.root.empty() ||
        backup.root == allowedRoot ||
        !IsContainedPath(backup.root, allowedRoot))
    {
        result.error = "backup path is outside the managed backup directory";
        return result;
    }
    std::error_code error;
    if (!BeginNonInterruptible(
            cancellation, result.cancelled, result.error))
    {
        return result;
    }
    const auto removed = RemoveTree(backup.root, error);
    if (error || removed == 0)
    {
        result.error = "cannot delete complete backup: " +
            (error ? error.message() : "backup not found");
        return result;
    }
    result.ok = true;
    result.backup = backup;
    return result;
}
}
