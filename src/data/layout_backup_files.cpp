#include "layout_backup_files.h"

#include "atomic_file.h"
#include "layout_storage.h"
#include "common/json_value.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <utility>

namespace snowdesktop::layout_backup
{
namespace
{
constexpr wchar_t kStorageBackupSuffix[] = L".storage.json";

std::wstring Trim(std::wstring value)
{
    const auto isSpace = [](wchar_t ch) {
        return std::iswspace(static_cast<wint_t>(ch)) != 0;
    };
    const auto first = std::find_if_not(value.begin(), value.end(), isSpace);
    if (first == value.end())
        return {};
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace)
                          .base();
    return std::wstring(first, last);
}

bool EndsWithInsensitive(
    std::wstring_view value,
    std::wstring_view suffix) noexcept
{
    if (suffix.size() > value.size())
        return false;
    const auto offset = value.size() - suffix.size();
    for (std::size_t index = 0; index < suffix.size(); ++index)
    {
        if (std::towlower(value[offset + index]) !=
            std::towlower(suffix[index]))
        {
            return false;
        }
    }
    return true;
}

std::wstring SanitizeLayoutBackupName(std::wstring name)
{
    name = Trim(std::move(name));
    if (name.empty())
        name = MakeLayoutTimestampName();

    // Keep the historical layout-backup naming convention. In particular,
    // the on-disk pair remains <name>.json and <name>.storage.json.
    for (wchar_t& ch : name)
    {
        if (ch < 0x20 || ch == L':' || ch == L'/' || ch == L'\\' ||
            ch == L'<' || ch == L'>' || ch == L'"' || ch == L'|' ||
            ch == L'?' || ch == L'*')
        {
            ch = L'_';
        }
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
        name.pop_back();
    if (name.empty())
        name = MakeLayoutTimestampName();
    constexpr std::size_t kMaximumBaseName = 160;
    if (name.size() > kMaximumBaseName)
        name.resize(kMaximumBaseName);
    return name;
}

std::string Win32Error(const char* operation, DWORD error)
{
    return std::string(operation) + ": Windows error " +
        std::to_string(error);
}

bool IsRegularFileWithoutReparsePoint(
    const std::filesystem::path& path) noexcept
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool ReadRegularFile(
    const std::filesystem::path& path,
    std::string& contents,
    std::string& error)
{
    if (ExistingPathHasReparsePoint(path) ||
        !IsRegularFileWithoutReparsePoint(path))
    {
        error = "backup input is missing, not a regular file, or uses a "
            "forbidden reparse point";
        return false;
    }
    return snowdesktop::atomic_file::ReadAll(path, contents, &error);
}
}

bool IsLayoutBackupId(std::wstring_view id)
{
    if (id.empty() || EndsWithInsensitive(id, kStorageBackupSuffix) ||
        !EndsWithInsensitive(id, L".json"))
    {
        return false;
    }
    const std::filesystem::path path(id);
    return path.filename().wstring() == id &&
        path.parent_path().empty() && id != L"." && id != L"..";
}

std::wstring StorageCompanionName(std::wstring_view layoutName)
{
    std::wstring result(layoutName);
    if (EndsWithInsensitive(result, L".json"))
        result.resize(result.size() - 5);
    result += kStorageBackupSuffix;
    return result;
}

std::wstring MakeLayoutTimestampName()
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u-%02u-%02u",
        time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

bool ExistingPathHasReparsePoint(
    const std::filesystem::path& input) noexcept
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(input, error);
    if (error || absolute.empty())
        return true;

    std::filesystem::path current = absolute.root_path();
    const auto relative = absolute.relative_path();
    for (auto part = relative.begin(); part != relative.end(); ++part)
    {
        current /= *part;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD win32Error = GetLastError();
            if (win32Error == ERROR_FILE_NOT_FOUND ||
                win32Error == ERROR_PATH_NOT_FOUND)
            {
                continue;
            }
            return true;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return true;
    }
    return false;
}

std::optional<std::filesystem::path> FindLayoutBackup(
    const std::filesystem::path& directory,
    std::wstring_view id)
{
    if (!IsLayoutBackupId(id) ||
        ExistingPathHasReparsePoint(directory))
        return std::nullopt;
    const auto candidate = directory / id;
    if (ExistingPathHasReparsePoint(candidate) ||
        !IsRegularFileWithoutReparsePoint(candidate))
    {
        return std::nullopt;
    }
    return candidate;
}

Result Create(
    const std::filesystem::path& layoutFile,
    const std::filesystem::path& storageFile,
    const std::filesystem::path& directory,
    std::wstring requestedName, std::stop_token stop,
    const std::function<bool()>& beginCommit)
{
    Result result;
    if (stop.stop_requested())
    {
        result.cancelled = true;
        return result;
    }

    std::error_code error;
    if (ExistingPathHasReparsePoint(directory) ||
        ExistingPathHasReparsePoint(layoutFile) ||
        !IsRegularFileWithoutReparsePoint(layoutFile))
    {
        result.error = "the active layout or backup directory is unsafe";
        return result;
    }
    std::filesystem::create_directories(
        directory, error);
    if (error)
    {
        result.error = "cannot create the layout backup directory: " +
            error.message();
        return result;
    }
    if (ExistingPathHasReparsePoint(directory))
    {
        result.error = "the layout backup directory uses a forbidden "
            "reparse point";
        return result;
    }

    const std::wstring baseName =
        SanitizeLayoutBackupName(std::move(requestedName));
    std::filesystem::path layoutDestination =
        directory / (baseName + L".json");
    std::filesystem::path storageDestination =
        directory /
        (baseName + kStorageBackupSuffix);
    for (std::uint32_t index = 1;
        (std::filesystem::exists(layoutDestination, error) ||
            std::filesystem::exists(storageDestination, error)) && !error;
        ++index)
    {
        if (stop.stop_requested())
        {
            result.cancelled = true;
            return result;
        }
        const std::wstring unique =
            baseName + L"(" + std::to_wstring(index) + L")";
        layoutDestination =
            directory / (unique + L".json");
        storageDestination = directory /
            (unique + kStorageBackupSuffix);
    }
    if (error)
    {
        result.error = "cannot inspect the layout backup destination: " +
            error.message();
        return result;
    }

    bool layoutCreated = false;
    bool storageCreated = false;
    if (!beginCommit())
    {
        result.cancelled = true;
        return result;
    }
    if (!CopyFileW(layoutFile.c_str(),
            layoutDestination.c_str(), TRUE))
    {
        result.error = Win32Error(
            "cannot copy the layout backup", GetLastError());
        return result;
    }
    layoutCreated = true;

    if (stop.stop_requested())
    {
        if (layoutCreated)
            std::filesystem::remove(layoutDestination, error);
        result.cancelled = true;
        return result;
    }

    const DWORD storageAttributes =
        GetFileAttributesW(storageFile.c_str());
    if (storageAttributes != INVALID_FILE_ATTRIBUTES &&
        ((storageAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (storageAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0))
    {
        if (layoutCreated)
            std::filesystem::remove(layoutDestination, error);
        result.error = "the active layout storage file is unsafe";
        return result;
    }
    if (storageAttributes != INVALID_FILE_ATTRIBUTES &&
        !CopyFileW(storageFile.c_str(),
            storageDestination.c_str(), TRUE))
    {
        const DWORD copyError = GetLastError();
        storageCreated = GetFileAttributesW(
            storageDestination.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (layoutCreated)
            std::filesystem::remove(layoutDestination, error);
        if (storageCreated)
            std::filesystem::remove(storageDestination, error);
        result.error = Win32Error(
            "cannot copy the layout storage companion", copyError);
        return result;
    }
    storageCreated = storageAttributes != INVALID_FILE_ATTRIBUTES;

    if (stop.stop_requested())
    {
        if (layoutCreated)
            std::filesystem::remove(layoutDestination, error);
        error.clear();
        if (storageCreated)
            std::filesystem::remove(storageDestination, error);
        result.cancelled = true;
        return result;
    }

    result.ok = true;
    return result;
}

RestoreFiles ReadForRestore(
    const std::filesystem::path& directory,
    std::wstring_view id, std::stop_token stop)
{
    RestoreFiles result;
    const auto source =
        FindLayoutBackup(directory, id);
    if (!source)
    {
        result.error = "the selected layout backup no longer exists";
        return result;
    }
    if (stop.stop_requested())
    {
        result.cancelled = true;
        return result;
    }

    if (!ReadRegularFile(*source, result.layout, result.error) ||
        !snowdesktop::layout_storage::ValidateDocument(
            result.layout, &result.error))
    {
        if (result.error.empty())
            result.error = "the selected layout backup is invalid";
        return result;
    }
    if (stop.stop_requested())
    {
        result.cancelled = true;
        return result;
    }

    const auto storageSource = directory /
        StorageCompanionName(id);
    const DWORD storageSourceAttributes =
        GetFileAttributesW(storageSource.c_str());
    if (storageSourceAttributes != INVALID_FILE_ATTRIBUTES)
    {
        std::string contents;
        if (!ReadRegularFile(storageSource, contents, result.error))
            return result;
        JsonValue storageRoot;
        if (!ParseJson(contents, storageRoot, &result.error) ||
            !storageRoot.IsObject())
        {
            if (result.error.empty())
                result.error = "the layout storage companion is invalid";
            return result;
        }
        result.storage = std::move(contents);
    }
    if (stop.stop_requested())
    {
        result.cancelled = true;
        return result;
    }

    result.ok = true;
    return result;
}
}
