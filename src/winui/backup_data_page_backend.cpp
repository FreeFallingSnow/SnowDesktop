#include "pch.h"

#include "backup_data_page_backend.h"

#include "../atomic_file.h"
#include "../data_paths.h"
#include "../deployment_context.h"
#include "../full_data_backup.h"
#include "../json_value.h"
#include "../layout_storage.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <map>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>

namespace snowdesktop::winui
{
namespace
{

constexpr wchar_t kLayoutFileName[] = L"SnowDesktop.layout.json";
constexpr wchar_t kStorageFileName[] = L"SnowDesktop.storage.json";
constexpr wchar_t kStorageBackupSuffix[] = L".storage.json";

struct BackendPaths
{
    std::filesystem::path stateRoot;
    std::filesystem::path dataDirectory;
    std::filesystem::path layoutFile;
    std::filesystem::path storageFile;
    std::filesystem::path layoutBackupDirectory;
    std::filesystem::path fullBackupDirectory;
};

struct BackupInventory
{
    std::vector<LayoutBackupEntry> layoutEntries;
    std::vector<FullDataBackupEntry> fullEntries;
    std::map<std::wstring, std::filesystem::path> fullBackupRoots;
};

enum class WorkKind : std::uint8_t
{
    Refresh,
    Action,
};

struct WorkContext
{
    WorkKind kind = WorkKind::Refresh;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::uint64_t requestId = 0;
    BackupDataActionRequest request;
    BackendPaths paths;
    std::string hostVersion;
    std::string sourceType;
    std::wstring beforeRestoreSuffix;
};

struct WorkResult
{
    bool ok = false;
    bool cancelled = false;
    bool replacementQueued = false;
    std::optional<LayoutRestorePayload> layoutRestore;
    bool hasInventory = false;
    std::string error;
    BackupInventory inventory;
};

struct WorkCompletion
{
    WorkContext context;
    WorkResult result;
};

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty())
        return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0)
        return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        result.data(), required);
    return result;
}

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

std::wstring FormatFileTime(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
            &attributes))
    {
        return {};
    }
    SYSTEMTIME time{};
    if (!FileTimeToSystemTime(&attributes.ftLastWriteTime, &time))
        return {};
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute);
    return buffer;
}

std::string Win32Error(const char* operation, DWORD error)
{
    return std::string(operation) + ": Windows error " +
        std::to_string(error);
}

bool IsReparsePoint(const std::filesystem::path& path) noexcept
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
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

std::string DefaultHostVersion()
{
#ifdef SNOWDESKTOP_VERSION
    return SNOWDESKTOP_VERSION;
#else
    return "unknown";
#endif
}

BackendPaths ResolvePaths(const BackupDataPageBackendOptions& options)
{
    BackendPaths paths;
    if (options.dataDirectory.empty())
    {
        paths.dataDirectory = GetDataDirectoryPath();
        paths.layoutFile = GetDataFilePath(kLayoutFileName);
        paths.storageFile = GetDataFilePath(kStorageFileName);
        paths.layoutBackupDirectory = GetDataSubdirectoryPath(L"backups");
    }
    else
    {
        paths.dataDirectory = options.dataDirectory;
        paths.layoutFile = paths.dataDirectory / kLayoutFileName;
        paths.storageFile = paths.dataDirectory / kStorageFileName;
        paths.layoutBackupDirectory = paths.dataDirectory / L"backups";
    }

    paths.stateRoot = options.stateRoot;
    if (paths.stateRoot.empty())
    {
        paths.stateRoot =
            snowdesktop::deployment::GetPackageLocalStatePath();
        if (paths.stateRoot.empty())
            paths.stateRoot = paths.dataDirectory.parent_path();
    }
    paths.fullBackupDirectory = paths.stateRoot / L"FullBackups";
    return paths;
}

snowdesktop::backup::FullDataBackupManager MakeFullBackupManager(
    const WorkContext& context)
{
    return snowdesktop::backup::FullDataBackupManager(
        context.paths.stateRoot,
        context.paths.dataDirectory,
        context.hostVersion,
        context.sourceType);
}

std::vector<LayoutBackupEntry> ListLayoutBackups(
    const BackendPaths& paths)
{
    std::vector<LayoutBackupEntry> entries;
    if (ExistingPathHasReparsePoint(paths.layoutBackupDirectory))
        return entries;
    std::error_code error;
    for (std::filesystem::directory_iterator item(
            paths.layoutBackupDirectory, error), end;
        !error && item != end;
        item.increment(error))
    {
        std::error_code itemError;
        const auto status = item->symlink_status(itemError);
        if (itemError || !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status))
        {
            continue;
        }
        const std::wstring id = item->path().filename().wstring();
        if (!IsLayoutBackupId(id))
            continue;

        LayoutBackupEntry entry;
        entry.id = id;
        entry.displayName = id.substr(0, id.size() - 5);
        entry.createdAt = FormatFileTime(item->path());
        entry.hasStorageCompanion = std::filesystem::is_regular_file(
            paths.layoutBackupDirectory / StorageCompanionName(id),
            itemError);
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
        [](const LayoutBackupEntry& left, const LayoutBackupEntry& right) {
            if (left.createdAt != right.createdAt)
                return left.createdAt > right.createdAt;
            return left.id > right.id;
        });
    return entries;
}

FullDataBackupEntry ToPresenterEntry(
    const snowdesktop::backup::BackupInfo& backup)
{
    FullDataBackupEntry entry;
    entry.id = backup.id;
    entry.displayName = backup.id;
    entry.createdAt = Utf8ToWide(backup.createdAt);
    entry.sourceType = Utf8ToWide(backup.sourceType);
    entry.fileCount = backup.fileCount;
    entry.totalBytes = backup.totalBytes;
    entry.migrationRollback = backup.migrationRollback;
    return entry;
}

BackupInventory BuildInventory(const WorkContext& context)
{
    BackupInventory inventory;
    inventory.layoutEntries = ListLayoutBackups(context.paths);
    auto manager = MakeFullBackupManager(context);
    for (const auto& backup : manager.List())
    {
        inventory.fullEntries.push_back(ToPresenterEntry(backup));
        inventory.fullBackupRoots.emplace(backup.id, backup.root);
    }
    return inventory;
}

std::optional<snowdesktop::backup::BackupInfo> FindFullBackup(
    snowdesktop::backup::FullDataBackupManager& manager,
    std::wstring_view id)
{
    std::optional<snowdesktop::backup::BackupInfo> found;
    for (auto& backup : manager.List())
    {
        if (backup.id != id)
            continue;
        if (found)
            return std::nullopt;
        found = std::move(backup);
    }
    return found;
}

std::optional<std::filesystem::path> FindLayoutBackup(
    const BackendPaths& paths,
    std::wstring_view id)
{
    if (!IsLayoutBackupId(id) ||
        ExistingPathHasReparsePoint(paths.layoutBackupDirectory))
        return std::nullopt;
    const auto candidate = paths.layoutBackupDirectory / id;
    if (ExistingPathHasReparsePoint(candidate) ||
        !IsRegularFileWithoutReparsePoint(candidate))
    {
        return std::nullopt;
    }
    return candidate;
}

WorkResult CreateLayoutBackup(
    const WorkContext& context,
    std::wstring requestedName,
    std::stop_token stop)
{
    WorkResult result;
    if (stop.stop_requested())
    {
        result.cancelled = true;
        return result;
    }

    std::error_code error;
    if (ExistingPathHasReparsePoint(context.paths.layoutBackupDirectory) ||
        ExistingPathHasReparsePoint(context.paths.layoutFile) ||
        !IsRegularFileWithoutReparsePoint(context.paths.layoutFile))
    {
        result.error = "the active layout or backup directory is unsafe";
        return result;
    }
    std::filesystem::create_directories(
        context.paths.layoutBackupDirectory, error);
    if (error)
    {
        result.error = "cannot create the layout backup directory: " +
            error.message();
        return result;
    }
    if (ExistingPathHasReparsePoint(context.paths.layoutBackupDirectory))
    {
        result.error = "the layout backup directory uses a forbidden "
            "reparse point";
        return result;
    }

    const std::wstring baseName =
        SanitizeLayoutBackupName(std::move(requestedName));
    std::filesystem::path layoutDestination =
        context.paths.layoutBackupDirectory / (baseName + L".json");
    std::filesystem::path storageDestination =
        context.paths.layoutBackupDirectory /
        (baseName + kStorageBackupSuffix);
    for (std::uint32_t index = 1;
        (std::filesystem::exists(layoutDestination, error) ||
            std::filesystem::exists(storageDestination, error)) && !error;
        ++index)
    {
        const std::wstring unique =
            baseName + L"(" + std::to_wstring(index) + L")";
        layoutDestination =
            context.paths.layoutBackupDirectory / (unique + L".json");
        storageDestination = context.paths.layoutBackupDirectory /
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
    if (!CopyFileW(context.paths.layoutFile.c_str(),
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
        GetFileAttributesW(context.paths.storageFile.c_str());
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
        !CopyFileW(context.paths.storageFile.c_str(),
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

    result.ok = true;
    return result;
}

WorkResult RestoreLayoutBackup(
    const WorkContext& context,
    std::stop_token stop)
{
    WorkResult result;
    const auto source =
        FindLayoutBackup(context.paths, context.request.subjectId);
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

    std::string replacementLayout;
    if (!ReadRegularFile(*source, replacementLayout, result.error) ||
        !snowdesktop::layout_storage::ValidateDocument(
            replacementLayout, &result.error))
    {
        if (result.error.empty())
            result.error = "the selected layout backup is invalid";
        return result;
    }

    const auto storageSource = context.paths.layoutBackupDirectory /
        StorageCompanionName(context.request.subjectId);
    std::optional<std::string> replacementStorage;
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
        replacementStorage = std::move(contents);
    }

    WorkResult safetyBackup = CreateLayoutBackup(context,
        MakeLayoutTimestampName() + context.beforeRestoreSuffix, stop);
    if (!safetyBackup.ok)
        return safetyBackup;

    // The worker stops at a validated immutable payload. Only DesktopApp's
    // settings STA may replace live files and reconcile the in-memory model.
    if (stop.stop_requested())
    {
        result.cancelled = true;
        return result;
    }
    result.layoutRestore = LayoutRestorePayload{
        std::move(replacementLayout), std::move(replacementStorage)};
    result.ok = true;
    return result;
}

WorkResult DeleteLayoutBackup(
    const WorkContext& context,
    std::stop_token stop)
{
    WorkResult result;
    const auto source =
        FindLayoutBackup(context.paths, context.request.subjectId);
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

    std::error_code error;
    const auto companion = context.paths.layoutBackupDirectory /
        StorageCompanionName(context.request.subjectId);
    std::filesystem::remove(companion, error);
    error.clear();
    if (!std::filesystem::remove(*source, error) || error)
    {
        result.error = "cannot delete the layout backup";
        if (error)
            result.error += ": " + error.message();
        return result;
    }
    result.ok = true;
    return result;
}

std::wstring NormalizePathForComparison(
    const std::filesystem::path& path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error)
    {
        error.clear();
        normalized = std::filesystem::absolute(path, error);
    }
    if (error)
        normalized = path.lexically_normal();
    std::wstring value = normalized.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 3 && value.back() == L'\\')
        value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return std::towlower(ch); });
    return value;
}

bool PathsOverlap(
    const std::filesystem::path& leftPath,
    const std::filesystem::path& rightPath)
{
    const std::wstring left = NormalizePathForComparison(leftPath);
    const std::wstring right = NormalizePathForComparison(rightPath);
    if (left.empty() || right.empty())
        return true;
    if (left == right)
        return true;
    return left.starts_with(right + L"\\") ||
        right.starts_with(left + L"\\");
}

bool LooksLikeDataDirectory(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_directory(path, error))
        return false;
    return std::filesystem::is_regular_file(
               path / kLayoutFileName, error) ||
        std::filesystem::is_regular_file(
            path / L"SnowDesktop.general.json", error) ||
        std::filesystem::is_directory(path / L"widgets", error) ||
        std::filesystem::is_directory(path / L"backups", error);
}

std::filesystem::path ResolveMigrationSource(
    const std::filesystem::path& selected)
{
    const auto nested = selected / L"data";
    return LooksLikeDataDirectory(nested) ? nested : selected;
}

WorkResult RunAction(const WorkContext& context, std::stop_token stop)
{
    WorkResult result;
    if (stop.stop_requested())
    {
        result.cancelled = true;
        return result;
    }

    if (context.request.command == BackupDataCommand::CreateLayoutBackup)
    {
        return CreateLayoutBackup(
            context, context.request.displayName, stop);
    }
    if (context.request.command == BackupDataCommand::RestoreLayoutBackup)
        return RestoreLayoutBackup(context, stop);
    if (context.request.command == BackupDataCommand::DeleteLayoutBackup)
        return DeleteLayoutBackup(context, stop);

    auto manager = MakeFullBackupManager(context);
    snowdesktop::backup::OperationResult storageResult;
    switch (context.request.command)
    {
    case BackupDataCommand::CreateFullBackup:
        storageResult = manager.Create();
        break;
    case BackupDataCommand::ImportAndRestoreFullBackup:
        if (context.request.selectedPath.empty())
        {
            result.error = "a backup archive was not selected";
            return result;
        }
        storageResult = manager.ImportAndQueue(
            context.request.selectedPath);
        result.replacementQueued = storageResult.ok;
        break;
    case BackupDataCommand::ExportFullBackup:
    {
        if (context.request.selectedPath.empty())
        {
            result.error = "an export destination was not selected";
            return result;
        }
        const auto backup =
            FindFullBackup(manager, context.request.subjectId);
        if (!backup)
        {
            result.error = "the selected complete backup no longer exists";
            return result;
        }
        storageResult = manager.Export(
            *backup, context.request.selectedPath);
        break;
    }
    case BackupDataCommand::RestoreFullBackup:
    {
        const auto backup =
            FindFullBackup(manager, context.request.subjectId);
        if (!backup)
        {
            result.error = "the selected complete backup no longer exists";
            return result;
        }
        storageResult = manager.QueueRestore(*backup);
        result.replacementQueued = storageResult.ok;
        break;
    }
    case BackupDataCommand::DeleteFullBackup:
    {
        const auto backup =
            FindFullBackup(manager, context.request.subjectId);
        if (!backup)
        {
            result.error = "the selected complete backup no longer exists";
            return result;
        }
        storageResult = manager.Delete(*backup);
        break;
    }
    case BackupDataCommand::MigrateData:
    {
        if (context.request.selectedPath.empty())
        {
            result.error = "a migration source was not selected";
            return result;
        }
        const auto source =
            ResolveMigrationSource(context.request.selectedPath);
        if (!LooksLikeDataDirectory(source) ||
            PathsOverlap(source, context.paths.dataDirectory))
        {
            result.error =
                "the selected migration source is invalid or overlaps "
                "the active data directory";
            return result;
        }
        if (stop.stop_requested())
        {
            result.cancelled = true;
            return result;
        }
        // QueueDirectory reuses FullDataBackupManager's validated hand-off to
        // portable_data_migration; no second archive or migration format is
        // introduced by the WinUI backend.
        storageResult = manager.QueueDirectory(source);
        result.replacementQueued = storageResult.ok;
        break;
    }
    default:
        result.error = "the requested backup operation is unsupported";
        return result;
    }

    result.ok = storageResult.ok;
    result.error = std::move(storageResult.error);
    return result;
}

WorkResult RunWork(const WorkContext& context, std::stop_token stop)
{
    WorkResult result;
    try
    {
        if (context.kind == WorkKind::Refresh)
        {
            if (stop.stop_requested())
            {
                result.cancelled = true;
                return result;
            }
            result.inventory = BuildInventory(context);
            if (stop.stop_requested())
            {
                result.cancelled = true;
                return result;
            }
            result.ok = true;
            result.hasInventory = true;
            return result;
        }

        result = RunAction(context, stop);
        // A queued replacement is an irreversible hand-off. A late cancel
        // must not hide it or skip the required dirty-discard/restart path.
        if (!result.ok || result.replacementQueued)
            return result;

        switch (context.request.completionPolicy)
        {
        case BackupDataCompletionPolicy::RefreshBackupLists:
        case BackupDataCompletionPolicy::ReloadDesktopLayout:
            result.inventory = BuildInventory(context);
            result.hasInventory = true;
            break;
        default:
            break;
        }
        return result;
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        result.ok = false;
        result.error = error.what();
        return result;
    }
    catch (const std::exception& error)
    {
        result.ok = false;
        result.error = error.what();
        return result;
    }
    catch (...)
    {
        result.ok = false;
        result.error = "an unknown backup operation error occurred";
        return result;
    }
}

BackupDataOperation ToOperation(BackupDataCommand command) noexcept
{
    switch (command)
    {
    case BackupDataCommand::CreateLayoutBackup:
        return BackupDataOperation::CreateLayoutBackup;
    case BackupDataCommand::RestoreLayoutBackup:
        return BackupDataOperation::RestoreLayoutBackup;
    case BackupDataCommand::DeleteLayoutBackup:
        return BackupDataOperation::DeleteLayoutBackup;
    case BackupDataCommand::CreateFullBackup:
        return BackupDataOperation::CreateFullBackup;
    case BackupDataCommand::ImportAndRestoreFullBackup:
        return BackupDataOperation::ImportAndRestoreFullBackup;
    case BackupDataCommand::ExportFullBackup:
        return BackupDataOperation::ExportFullBackup;
    case BackupDataCommand::RestoreFullBackup:
        return BackupDataOperation::RestoreFullBackup;
    case BackupDataCommand::DeleteFullBackup:
        return BackupDataOperation::DeleteFullBackup;
    case BackupDataCommand::MigrateData:
        return BackupDataOperation::MigrateData;
    default:
        return BackupDataOperation::None;
    }
}

BackupDataCompletionPolicy RequiredCompletionPolicy(
    BackupDataCommand command) noexcept
{
    switch (command)
    {
    case BackupDataCommand::CreateLayoutBackup:
    case BackupDataCommand::DeleteLayoutBackup:
    case BackupDataCommand::CreateFullBackup:
    case BackupDataCommand::DeleteFullBackup:
        return BackupDataCompletionPolicy::RefreshBackupLists;
    case BackupDataCommand::RestoreLayoutBackup:
        return BackupDataCompletionPolicy::ReloadDesktopLayout;
    case BackupDataCommand::ExportFullBackup:
        return BackupDataCompletionPolicy::ShowResultOnly;
    case BackupDataCommand::ImportAndRestoreFullBackup:
    case BackupDataCommand::RestoreFullBackup:
    case BackupDataCommand::MigrateData:
        return BackupDataCompletionPolicy::ClearDirtyThenRestartApplication;
    default:
        return BackupDataCompletionPolicy::None;
    }
}

bool IsWorkerCommand(BackupDataCommand command) noexcept
{
    return ToOperation(command) != BackupDataOperation::None;
}

bool IsOpenCommand(BackupDataCommand command) noexcept
{
    return command == BackupDataCommand::OpenDataDirectory ||
        command == BackupDataCommand::OpenFullBackupDirectory ||
        command == BackupDataCommand::OpenFullBackupItem;
}

} // namespace

struct BackupDataPageBackend::State final
    : std::enable_shared_from_this<BackupDataPageBackend::State>
{
    State(SettingsController& owner, BackupDataPageBackendOptions configured)
        : controller(&owner), options(std::move(configured)),
          paths(ResolvePaths(options))
    {
        if (options.hostVersion.empty())
            options.hostVersion = DefaultHostVersion();
        if (options.sourceType.empty())
        {
            options.sourceType = snowdesktop::deployment::IsPackaged()
                ? "installed"
                : "portable";
        }
        snapshot.dataDirectory = paths.dataDirectory.wstring();
        snapshot.fullBackupDirectory = paths.fullBackupDirectory.wstring();
    }

    SettingsController* controller = nullptr;
    BackupDataPageBackendOptions options;
    BackendPaths paths;
    BackupDataPageSnapshot snapshot;
    SnapshotChangedCallback snapshotChanged;
    std::map<std::wstring, std::filesystem::path> fullBackupRoots;
    std::jthread worker;
    mutable std::mutex completionMutex;
    std::optional<WorkCompletion> pendingCompletion;
    std::uint64_t nextRequestId = 1;
    bool active = false;
    bool closed = false;

    std::wstring L(std::string_view key, std::wstring_view fallback) const
    {
        if (options.localize)
        {
            std::wstring translated = options.localize(key);
            const std::wstring untranslated(key.begin(), key.end());
            if (!translated.empty() && translated != untranslated)
                return translated;
        }
        return std::wstring(fallback);
    }

    void Publish()
    {
        if (closed)
            return;
        ++snapshot.revision;
        if (snapshotChanged)
            snapshotChanged(snapshot);
    }

    bool IsCurrent(
        std::uint64_t generation,
        std::uint64_t revision) const noexcept
    {
        return !closed && active && snapshot.initialized &&
            generation == snapshot.generation &&
            revision == snapshot.revision;
    }

    void SetNotice(
        BackupDataNoticeSeverity severity,
        std::wstring title,
        std::wstring message)
    {
        snapshot.notice = BackupDataNotice{
            severity, std::move(title), std::move(message)};
    }

    std::wstring OperationTitle(BackupDataCommand command) const
    {
        switch (command)
        {
        case BackupDataCommand::CreateLayoutBackup:
        case BackupDataCommand::RestoreLayoutBackup:
        case BackupDataCommand::DeleteLayoutBackup:
            return L("app.settings.layout_backups", L"Layout backups");
        case BackupDataCommand::MigrateData:
            return L("app.settings.data_migration", L"Data migration");
        default:
            return L("app.settings.full_data_backups",
                L"Complete data backups");
        }
    }

    std::wstring RunningMessage(BackupDataCommand command) const
    {
        switch (command)
        {
        case BackupDataCommand::CreateLayoutBackup:
            return L("app.settings.save_backup", L"Saving layout backup…");
        case BackupDataCommand::RestoreLayoutBackup:
            return L("app.settings.restore", L"Restoring layout backup…");
        case BackupDataCommand::DeleteLayoutBackup:
            return L("app.settings.delete", L"Deleting layout backup…");
        case BackupDataCommand::CreateFullBackup:
            return L("app.settings.create_full_backup",
                L"Creating complete backup…");
        case BackupDataCommand::ImportAndRestoreFullBackup:
            return L("app.settings.restore_from_backup_file",
                L"Validating and staging backup…");
        case BackupDataCommand::ExportFullBackup:
            return L("app.settings.export_backup", L"Exporting backup…");
        case BackupDataCommand::RestoreFullBackup:
            return L("app.settings.restore", L"Staging complete backup…");
        case BackupDataCommand::DeleteFullBackup:
            return L("app.settings.delete", L"Deleting complete backup…");
        case BackupDataCommand::MigrateData:
            return L("app.settings.migrate_all_data", L"Staging data…");
        default:
            return L("settings.nav.backup", L"Refreshing backups…");
        }
    }

    std::wstring SuccessMessage(BackupDataCommand command) const
    {
        switch (command)
        {
        case BackupDataCommand::CreateLayoutBackup:
            return L("app.settings.save_backup", L"The layout backup was saved.");
        case BackupDataCommand::RestoreLayoutBackup:
            return L("app.settings.restore", L"The layout backup was restored.");
        case BackupDataCommand::DeleteLayoutBackup:
            return L("app.settings.delete", L"The layout backup was deleted.");
        case BackupDataCommand::CreateFullBackup:
            return L("app.settings.create_full_backup_success",
                L"The complete backup was created.");
        case BackupDataCommand::ImportAndRestoreFullBackup:
            return L("app.settings.restore_backup_file_success",
                L"The backup will be restored after restart.");
        case BackupDataCommand::ExportFullBackup:
            return L("app.settings.export_backup_success",
                L"The backup file was exported.");
        case BackupDataCommand::RestoreFullBackup:
            return L("app.settings.restore_full_backup_success",
                L"The backup will be restored after restart.");
        case BackupDataCommand::DeleteFullBackup:
            return L("app.settings.delete", L"The complete backup was deleted.");
        case BackupDataCommand::MigrateData:
            return L("app.settings.migrate_data_success",
                L"The data will be moved in after restart.");
        default:
            return L"The backup operation completed.";
        }
    }

    std::wstring FailureMessage(BackupDataCommand command) const
    {
        switch (command)
        {
        case BackupDataCommand::CreateFullBackup:
            return L("app.settings.create_full_backup_failed",
                L"Could not create the complete backup.");
        case BackupDataCommand::ImportAndRestoreFullBackup:
            return L("app.settings.restore_backup_file_failed",
                L"Could not restore this backup file.");
        case BackupDataCommand::ExportFullBackup:
            return L("app.settings.export_backup_failed",
                L"Could not export the backup file.");
        case BackupDataCommand::RestoreFullBackup:
            return L("app.settings.restore_full_backup_failed",
                L"Could not restore this backup.");
        case BackupDataCommand::DeleteFullBackup:
            return L("app.settings.delete_full_backup_failed",
                L"Could not delete this complete backup.");
        case BackupDataCommand::MigrateData:
            return L("app.settings.migrate_data_failed",
                L"Data migration failed.");
        default:
            return L"The layout backup operation failed.";
        }
    }

    void AppendError(std::wstring& message, const std::string& error) const
    {
        if (!error.empty())
            message += L"\n\n" + Utf8ToWide(error);
    }

    void ApplyInventory(BackupInventory inventory)
    {
        snapshot.layoutBackups = std::move(inventory.layoutEntries);
        snapshot.fullBackups = std::move(inventory.fullEntries);
        fullBackupRoots = std::move(inventory.fullBackupRoots);
    }

    void JoinPreviousWorker()
    {
        if (worker.joinable())
            worker.join();
    }

    bool StartWork(WorkContext context, BackupDataOperation operation,
        std::wstring message)
    {
        if (closed || !active || snapshot.operation.running)
            return false;
        if (!options.postToUi)
        {
            SetNotice(BackupDataNoticeSeverity::Error,
                L("settings.nav.backup", L"Backup & data"),
                L"The settings UI dispatcher is unavailable.");
            Publish();
            return false;
        }

        JoinPreviousWorker();
        context.generation = snapshot.generation;
        context.requestId = nextRequestId++;
        context.paths = paths;
        context.hostVersion = options.hostVersion;
        context.sourceType = options.sourceType;
        context.beforeRestoreSuffix = L(
            "app.settings.backup_before_restore_suffix",
            L" (Before Restore)");

        snapshot.notice.reset();
        snapshot.operation.requestId = context.requestId;
        snapshot.operation.operation = operation;
        snapshot.operation.running = true;
        snapshot.operation.cancellable = context.kind == WorkKind::Refresh ||
            context.request.command ==
                BackupDataCommand::CreateLayoutBackup ||
            context.request.command ==
                BackupDataCommand::RestoreLayoutBackup ||
            context.request.command ==
                BackupDataCommand::DeleteLayoutBackup;
        snapshot.operation.indeterminate = true;
        snapshot.operation.progress = 0.0;
        snapshot.operation.message = std::move(message);
        Publish();
        context.revision = snapshot.revision;

        const auto weak = weak_from_this();
        worker = std::jthread(
            [weak, context = std::move(context)](
                std::stop_token stop) mutable {
                WorkCompletion completion;
                completion.context = std::move(context);
                completion.result = RunWork(completion.context, stop);
                const auto self = weak.lock();
                if (!self)
                    return;
                {
                    std::lock_guard lock(self->completionMutex);
                    self->pendingCompletion = std::move(completion);
                }
                const auto requestId =
                    self->pendingCompletionRequestId();
                if (requestId == 0)
                    return;
                try
                {
                    (void)self->options.postToUi(
                        [weak, requestId] {
                            if (const auto state = weak.lock())
                                state->DrainCompletion(requestId);
                        });
                }
                catch (...)
                {
                    // Close() or the next UI action drains the stored result.
                }
            });
        return true;
    }

    std::uint64_t pendingCompletionRequestId() const
    {
        std::lock_guard lock(completionMutex);
        return pendingCompletion
            ? pendingCompletion->context.requestId
            : 0;
    }

    void DrainCompletion(std::uint64_t requestId)
    {
        std::optional<WorkCompletion> completion;
        {
            std::lock_guard lock(completionMutex);
            if (!pendingCompletion ||
                pendingCompletion->context.requestId != requestId)
            {
                return;
            }
            completion = std::move(pendingCompletion);
            pendingCompletion.reset();
        }
        Finish(std::move(*completion));
    }

    void DrainAnyCompletion()
    {
        const std::uint64_t requestId = pendingCompletionRequestId();
        if (requestId != 0)
            DrainCompletion(requestId);
    }

    SettingsActionResult OpenPath(const std::filesystem::path& path)
    {
        const HWND owner = options.ownerWindow ? options.ownerWindow() : nullptr;
        if (options.openPath)
            return options.openPath(owner, path);
        if (path.empty() || reinterpret_cast<INT_PTR>(ShellExecuteW(
                owner, L"open", path.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL)) <= 32)
        {
            return SettingsActionResult::Failure(
                L"The requested data location could not be opened.");
        }
        return SettingsActionResult::Success();
    }

    void InvokeOpen(BackupDataActionRequest request)
    {
        std::filesystem::path path;
        switch (request.command)
        {
        case BackupDataCommand::OpenDataDirectory:
            path = paths.dataDirectory;
            break;
        case BackupDataCommand::OpenFullBackupDirectory:
            path = paths.fullBackupDirectory;
            break;
        case BackupDataCommand::OpenFullBackupItem:
        {
            const auto found = fullBackupRoots.find(request.subjectId);
            if (found != fullBackupRoots.end())
                path = found->second;
            break;
        }
        default:
            break;
        }
        const auto result = path.empty()
            ? SettingsActionResult::Failure(
                  L"The requested backup no longer exists.")
            : OpenPath(path);
        if (!result.Succeeded())
        {
            SetNotice(BackupDataNoticeSeverity::Error,
                OperationTitle(request.command), result.message);
            Publish();
        }
    }

    void Invoke(std::uint64_t generation, std::uint64_t revision,
        BackupDataActionRequest request)
    {
        DrainAnyCompletion();
        if (!IsCurrent(generation, revision) ||
            snapshot.replacementPending)
            return;
        if (IsOpenCommand(request.command))
        {
            if (request.completionPolicy != BackupDataCompletionPolicy::None)
                return;
            InvokeOpen(std::move(request));
            return;
        }
        if (!IsWorkerCommand(request.command) || snapshot.operation.running ||
            request.completionPolicy !=
                RequiredCompletionPolicy(request.command))
        {
            return;
        }

        // Capture a coherent application-owned state boundary before any
        // worker reads the live data tree. In particular, a complete backup
        // must include the final coalesced setting values and a layout restore
        // must not race an already-pending desktop-layout commit.
        const SettingsActionResult flush = controller->FlushAll();
        if (!flush.Succeeded())
        {
            SetNotice(BackupDataNoticeSeverity::Error,
                OperationTitle(request.command), flush.message);
            Publish();
            return;
        }

        WorkContext context;
        context.kind = WorkKind::Action;
        context.request = std::move(request);
        const BackupDataOperation operation =
            ToOperation(context.request.command);
        const std::wstring message =
            RunningMessage(context.request.command);
        (void)StartWork(
            std::move(context), operation, std::move(message));
    }

    void Cancel(std::uint64_t generation, std::uint64_t revision,
        std::uint64_t requestId)
    {
        if (!IsCurrent(generation, revision) ||
            !snapshot.operation.running ||
            snapshot.operation.requestId != requestId ||
            !snapshot.operation.cancellable)
        {
            return;
        }
        if (worker.joinable())
            worker.request_stop();
        snapshot.operation.cancellable = false;
        snapshot.operation.message =
            L("settings.backup.cancelling", L"Canceling…");
        Publish();
    }

    void Confirm(std::uint64_t generation, std::uint64_t revision,
        BackupDataConfirmationRequest request,
        BackupDataPageActions::ConfirmationCompletion completion)
    {
        if (!completion || !IsCurrent(generation, revision) ||
            snapshot.replacementPending || !options.confirm)
        {
            if (completion)
                completion(false);
            return;
        }
        const HWND owner = options.ownerWindow ? options.ownerWindow() : nullptr;
        const auto weak = weak_from_this();
        auto once = std::make_shared<std::atomic_bool>(false);
        auto sharedCompletion = std::make_shared<
            BackupDataPageActions::ConfirmationCompletion>(
                std::move(completion));
        try
        {
            options.confirm(owner, std::move(request),
                [weak, once, generation, revision,
                 sharedCompletion](bool confirmed) mutable {
                    if (once->exchange(true))
                        return;
                    const auto state = weak.lock();
                    if (!state || !state->options.postToUi)
                        return;
                    (void)state->options.postToUi(
                        [weak, generation, revision, confirmed,
                         sharedCompletion]() mutable {
                            const auto current = weak.lock();
                            if (current && current->IsCurrent(
                                    generation, revision))
                            {
                                (*sharedCompletion)(confirmed);
                            }
                        });
                });
        }
        catch (...)
        {
            if (!once->exchange(true) && *sharedCompletion)
                (*sharedCompletion)(false);
        }
    }

    void PickPath(std::uint64_t generation, std::uint64_t revision,
        BackupDataPickerRequest request,
        BackupDataPageActions::PickerCompletion completion)
    {
        if (!completion || !IsCurrent(generation, revision) ||
            snapshot.replacementPending || !options.pickPath)
        {
            if (completion)
                completion(std::nullopt);
            return;
        }
        const HWND owner = options.ownerWindow ? options.ownerWindow() : nullptr;
        const auto weak = weak_from_this();
        auto once = std::make_shared<std::atomic_bool>(false);
        auto sharedCompletion = std::make_shared<
            BackupDataPageActions::PickerCompletion>(
                std::move(completion));
        try
        {
            // owner is deliberately resolved here, not captured when the
            // backend is constructed, so every picker is bound to the live
            // settings top-level window.
            options.pickPath(owner, std::move(request),
                [weak, once, generation, revision,
                 sharedCompletion](
                    std::optional<std::filesystem::path> selected) mutable {
                    if (once->exchange(true))
                        return;
                    const auto state = weak.lock();
                    if (!state || !state->options.postToUi)
                        return;
                    (void)state->options.postToUi(
                        [weak, generation, revision,
                         selected = std::move(selected),
                         sharedCompletion]() mutable {
                            const auto current = weak.lock();
                            if (current && current->IsCurrent(
                                    generation, revision))
                            {
                                (*sharedCompletion)(std::move(selected));
                            }
                        });
                });
        }
        catch (...)
        {
            if (!once->exchange(true) && *sharedCompletion)
                (*sharedCompletion)(std::nullopt);
        }
    }

    void CompleteQueuedReplacement(const WorkCompletion& completion)
    {
        // Queue() has atomically published the next-start replacement. From
        // this point it is a committed transaction: do not read the old tree,
        // do not call FlushPending/FlushAll, and do not expose a path that can
        // continue without restart.
        controller->PrepareForExternalDataReplacement();

        snapshot.generation = controller->Generation();
        snapshot.revision = 0;
        snapshot.operation = {};
        snapshot.replacementPending = true;
        SetNotice(BackupDataNoticeSeverity::Success,
            OperationTitle(completion.context.request.command),
            SuccessMessage(completion.context.request.command));
        Publish();

        SettingsHostActions::Request restart;
        restart.action = SettingsHostActions::Action::RestartApplication;
        const SettingsActionResult restartResult =
            controller->InvokeHostAction(restart);
        if (!restartResult.Succeeded())
        {
            SetNotice(BackupDataNoticeSeverity::Error,
                OperationTitle(completion.context.request.command),
                restartResult.message);
            Publish();
        }
    }

    void Finish(WorkCompletion completion)
    {
        const bool matchingRequest =
            snapshot.operation.running &&
            snapshot.operation.requestId == completion.context.requestId &&
            snapshot.revision >= completion.context.revision;

        // A successful QueueRestore/ImportAndQueue/QueueDirectory must always
        // finish its discard-and-restart transaction, even if the page was
        // deactivated while the non-interruptible storage call completed.
        if (completion.result.replacementQueued && completion.result.ok)
        {
            CompleteQueuedReplacement(completion);
            return;
        }

        // The validated layout payload crosses exactly one application-owned
        // boundary on the STA. Keep this before page-lifetime gates: once the
        // worker has completed, closing the page cannot split file commit from
        // in-memory desktop reconciliation.
        if (completion.result.layoutRestore && completion.result.ok)
        {
            snapshot.operation = {};
            const SettingsActionResult commitResult =
                options.commitLayoutRestore
                ? options.commitLayoutRestore(
                      std::move(*completion.result.layoutRestore))
                : SettingsActionResult::Failure(
                      L"The application layout restore service is unavailable.");
            if (!active || closed)
                return;
            if (!commitResult.Succeeded())
            {
                std::wstring message = FailureMessage(
                    completion.context.request.command);
                AppendError(message, completion.result.error);
                if (!commitResult.message.empty())
                    message += L"\n\n" + commitResult.message;
                SetNotice(BackupDataNoticeSeverity::Error,
                    OperationTitle(completion.context.request.command),
                    std::move(message));
            }
            else
            {
                if (completion.result.hasInventory)
                    ApplyInventory(std::move(completion.result.inventory));
                SetNotice(BackupDataNoticeSeverity::Success,
                    OperationTitle(completion.context.request.command),
                    SuccessMessage(completion.context.request.command));
            }
            Publish();
            return;
        }
        if (!matchingRequest)
            return;

        snapshot.operation = {};
        if (!active || closed ||
            completion.context.generation != snapshot.generation)
        {
            return;
        }
        if (completion.result.cancelled)
        {
            SetNotice(BackupDataNoticeSeverity::Informational,
                OperationTitle(completion.context.request.command),
                L("settings.backup.cancelled", L"The operation was canceled."));
            Publish();
            return;
        }
        if (!completion.result.ok)
        {
            std::wstring message = FailureMessage(
                completion.context.request.command);
            AppendError(message, completion.result.error);
            SetNotice(BackupDataNoticeSeverity::Error,
                OperationTitle(completion.context.request.command),
                std::move(message));
            Publish();
            return;
        }

        if (completion.result.hasInventory)
            ApplyInventory(std::move(completion.result.inventory));
        if (completion.context.kind == WorkKind::Refresh)
        {
            Publish();
            return;
        }

        SetNotice(BackupDataNoticeSeverity::Success,
            OperationTitle(completion.context.request.command),
            SuccessMessage(completion.context.request.command));
        Publish();
    }

    void Refresh()
    {
        DrainAnyCompletion();
        if (closed || !active || snapshot.operation.running ||
            snapshot.replacementPending)
            return;
        WorkContext context;
        context.kind = WorkKind::Refresh;
        (void)StartWork(std::move(context), BackupDataOperation::None,
            L("settings.nav.backup", L"Refreshing backups…"));
    }

    void Activate(std::uint64_t generation)
    {
        DrainAnyCompletion();
        if (closed)
            return;
        generation = controller->Generation();
        active = true;
        if (!snapshot.initialized || snapshot.generation != generation)
        {
            if (worker.joinable())
            {
                worker.request_stop();
                worker.join();
                DrainAnyCompletion();
            }
            if (closed)
                return;
            snapshot = {};
            snapshot.generation = generation;
            snapshot.initialized = true;
            snapshot.dataDirectory = paths.dataDirectory.wstring();
            snapshot.fullBackupDirectory = paths.fullBackupDirectory.wstring();
            fullBackupRoots.clear();
        }
        Publish();
        if (!snapshot.operation.running)
            Refresh();
    }

    void Deactivate() noexcept
    {
        active = false;
        if (worker.joinable() && snapshot.operation.running)
        {
            worker.request_stop();
            snapshot.operation.cancellable = false;
        }
    }

    void Close() noexcept
    {
        if (closed)
            return;
        active = false;
        if (worker.joinable())
        {
            worker.request_stop();
            worker.join();
        }
        // Drain before marking closed. In particular, a storage call that
        // already queued a replacement still performs dirty-discard/restart.
        DrainAnyCompletion();
        closed = true;
        snapshotChanged = {};
    }
};

BackupDataPageBackend::BackupDataPageBackend(
    SettingsController& controller,
    BackupDataPageBackendOptions options)
    : state_(std::make_shared<State>(controller, std::move(options)))
{
}

BackupDataPageBackend::~BackupDataPageBackend()
{
    Close();
}

BackupDataPageActions BackupDataPageBackend::Actions()
{
    BackupDataPageActions actions;
    const std::weak_ptr<State> weak = state_;
    actions.invoke = [weak](std::uint64_t generation,
                         std::uint64_t revision,
                         BackupDataActionRequest request) {
        if (const auto state = weak.lock())
            state->Invoke(generation, revision, std::move(request));
    };
    actions.confirm = [weak](std::uint64_t generation,
                          std::uint64_t revision,
                          BackupDataConfirmationRequest request,
                          BackupDataPageActions::ConfirmationCompletion done) {
        if (const auto state = weak.lock())
        {
            state->Confirm(generation, revision,
                std::move(request), std::move(done));
        }
        else if (done)
        {
            done(false);
        }
    };
    actions.pickPath = [weak](std::uint64_t generation,
                           std::uint64_t revision,
                           BackupDataPickerRequest request,
                           BackupDataPageActions::PickerCompletion done) {
        if (const auto state = weak.lock())
        {
            state->PickPath(generation, revision,
                std::move(request), std::move(done));
        }
        else if (done)
        {
            done(std::nullopt);
        }
    };
    actions.cancel = [weak](std::uint64_t generation,
                         std::uint64_t revision,
                         std::uint64_t requestId) {
        if (const auto state = weak.lock())
            state->Cancel(generation, revision, requestId);
    };
    return actions;
}

void BackupDataPageBackend::SetSnapshotChangedCallback(
    SnapshotChangedCallback callback)
{
    if (!state_ || state_->closed)
        return;
    state_->snapshotChanged = std::move(callback);
    if (state_->snapshotChanged)
        state_->snapshotChanged(state_->snapshot);
}

BackupDataPageSnapshot BackupDataPageBackend::CurrentSnapshot() const
{
    return state_ ? state_->snapshot : BackupDataPageSnapshot{};
}

void BackupDataPageBackend::Activate(std::uint64_t generation)
{
    if (state_)
        state_->Activate(generation);
}

void BackupDataPageBackend::Deactivate() noexcept
{
    if (state_)
        state_->Deactivate();
}

void BackupDataPageBackend::Refresh()
{
    if (state_)
        state_->Refresh();
}

void BackupDataPageBackend::Close() noexcept
{
    if (state_)
        state_->Close();
}

} // namespace snowdesktop::winui
