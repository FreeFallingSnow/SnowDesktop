#include "portable_data_migration.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace snowdesktop::migration
{
namespace
{
constexpr wchar_t kPendingMarker[] = L"pending.txt";

std::filesystem::path ExtendedLengthPath(
    const std::filesystem::path& path, std::error_code& ec)
{
    ec.clear();
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec)
        return {};

    const std::wstring value = absolute.lexically_normal().wstring();
    if (value.starts_with(LR"(\\?\)"))
        return std::filesystem::path(value);
    if (value.starts_with(LR"(\\)"))
        return std::filesystem::path(
            std::wstring(LR"(\\?\UNC\)") + value.substr(2));
    return std::filesystem::path(std::wstring(LR"(\\?\)") + value);
}

std::string PathForError(const std::filesystem::path& path)
{
    const std::wstring value = path.wstring();
    if (value.empty())
        return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return "<unprintable path>";
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()),
        result.data(), required, nullptr, nullptr);
    return result;
}

void SetCopyError(CopyResult& result, const char* operation,
    const std::filesystem::path& path, const std::error_code& ec = {})
{
    result.error = operation;
    if (!path.empty())
        result.error += " [" + PathForError(path) + "]";
    if (ec)
        result.error += ": " + ec.message();
}

bool IsReparsePoint(const std::filesystem::path& path,
    CopyResult& result)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        SetCopyError(result, "cannot inspect migration source", path,
            std::error_code(static_cast<int>(GetLastError()),
                std::system_category()));
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        SetCopyError(result,
            "migration source contains a forbidden reparse point", path);
        return true;
    }
    return false;
}

std::filesystem::path MigrationRoot(const std::filesystem::path& stateRoot)
{
    return stateRoot / L"TempState" / L"PortableMigration";
}

bool IsSafeToken(const std::wstring& token)
{
    return !token.empty() && token.size() <= 96 &&
        std::all_of(token.begin(), token.end(), [](wchar_t ch) {
            return (ch >= L'0' && ch <= L'9') || ch == L'-';
        });
}

bool LooksLikeDataDirectory(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec))
        return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return false;
    }
    return std::filesystem::is_regular_file(
               path / L"SnowDesktop.layout.json", ec) ||
        std::filesystem::is_regular_file(
            path / L"SnowDesktop.general.json", ec) ||
        std::filesystem::is_directory(path / L"widgets", ec) ||
        std::filesystem::is_directory(path / L"backups", ec);
}

bool FinalizeMarker(const std::filesystem::path& marker,
    const std::filesystem::path& migrationRoot, const std::wstring& token,
    std::string& error)
{
    const auto completed =
        migrationRoot / (std::wstring(L"completed-") + token + L".txt");
    if (MoveFileExW(marker.c_str(), completed.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return true;
    }
    const DWORD moveError = GetLastError();
    if (DeleteFileW(marker.c_str()))
        return true;
    error = "data migration completed but marker cleanup failed: " +
        std::to_string(moveError) + "/" + std::to_string(GetLastError());
    return false;
}

bool ReadPendingToken(const std::filesystem::path& marker,
    std::wstring& token, std::string& error)
{
    std::ifstream input(marker, std::ios::binary);
    if (!input)
    {
        error = "cannot read pending migration marker";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    while (!text.empty() &&
        (text.back() == '\r' || text.back() == '\n' ||
            std::isspace(static_cast<unsigned char>(text.back()))))
    {
        text.pop_back();
    }
    token.assign(text.begin(), text.end());
    if (!IsSafeToken(token))
    {
        error = "pending migration marker contains an invalid token";
        return false;
    }
    return true;
}
}

CopyResult CopyDataTree(const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    CopyResult result;
    std::error_code ec;
    const auto extendedSource = ExtendedLengthPath(source, ec);
    if (ec)
    {
        SetCopyError(result, "cannot resolve migration source", source, ec);
        return result;
    }
    const auto extendedDestination = ExtendedLengthPath(destination, ec);
    if (ec)
    {
        SetCopyError(result, "cannot resolve migration destination",
            destination, ec);
        return result;
    }

    if (IsReparsePoint(extendedSource, result))
        return result;

    const auto sourceStatus =
        std::filesystem::symlink_status(extendedSource, ec);
    if (ec || !std::filesystem::is_directory(sourceStatus))
    {
        SetCopyError(result, "migration source is not a directory",
            source, ec);
        return result;
    }

    std::filesystem::create_directories(extendedDestination, ec);
    if (ec)
    {
        SetCopyError(result, "cannot create migration staging directory",
            destination, ec);
        return result;
    }

    std::filesystem::recursive_directory_iterator entry(
        extendedSource, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec)
    {
        SetCopyError(result, "cannot enumerate migration source",
            source, ec);
        return result;
    }

    while (entry != end)
    {
        const auto current = entry->path();
        if (IsReparsePoint(current, result))
            return result;

        const auto status = entry->symlink_status(ec);
        if (ec)
        {
            SetCopyError(result, "cannot inspect migration source item",
                current, ec);
            return result;
        }
        if (std::filesystem::is_symlink(status))
        {
            SetCopyError(result,
                "migration source contains a forbidden symbolic link",
                current);
            return result;
        }

        const auto relative =
            current.lexically_relative(extendedSource);
        if (relative.empty() ||
            relative.native().starts_with(L".."))
        {
            SetCopyError(result, "migration source path escaped its root",
                current);
            return result;
        }
        const auto target = extendedDestination / relative;

        if (std::filesystem::is_directory(status))
        {
            std::filesystem::create_directories(target, ec);
            if (ec)
            {
                SetCopyError(result,
                    "cannot create migration staging subdirectory",
                    target, ec);
                return result;
            }
        }
        else if (std::filesystem::is_regular_file(status))
        {
            std::filesystem::create_directories(
                target.parent_path(), ec);
            if (ec)
            {
                SetCopyError(result,
                    "cannot create migration staging parent",
                    target.parent_path(), ec);
                return result;
            }
            std::filesystem::copy_file(current, target,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                SetCopyError(result,
                    "cannot copy migration source file", current, ec);
                return result;
            }
            ++result.files;
            result.bytes += std::filesystem::file_size(current, ec);
            if (ec)
            {
                SetCopyError(result,
                    "cannot read copied migration file size", current, ec);
                return result;
            }
        }
        else
        {
            SetCopyError(result,
                "migration source contains an unsupported file type",
                current);
            return result;
        }

        entry.increment(ec);
        if (ec)
        {
            SetCopyError(result, "cannot continue migration enumeration",
                current, ec);
            return result;
        }
    }

    result.ok = true;
    return result;
}

bool Queue(const std::filesystem::path& stateRoot,
    const std::wstring& token, std::string& error)
{
    error.clear();
    if (!IsSafeToken(token))
    {
        error = "invalid migration token";
        return false;
    }
    const auto root = MigrationRoot(stateRoot);
    const auto staging =
        root / (std::wstring(L"staging-") + token);
    if (!LooksLikeDataDirectory(staging))
    {
        error = "staged portable data is missing or invalid";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec)
    {
        error = "cannot create migration state directory: " + ec.message();
        return false;
    }
    const auto temporary = root / L"pending.tmp";
    {
        std::ofstream output(temporary,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "cannot create pending migration marker";
            return false;
        }
        for (const wchar_t ch : token)
            output.put(static_cast<char>(ch));
        output.put('\n');
        output.flush();
        if (!output)
        {
            error = "cannot write pending migration marker";
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    const auto marker = root / kPendingMarker;
    if (!MoveFileExW(temporary.c_str(), marker.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "cannot publish pending migration marker: " +
            std::to_string(GetLastError());
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

ApplyResult ApplyPending(const std::filesystem::path& stateRoot)
{
    ApplyResult result;
    const auto root = MigrationRoot(stateRoot);
    const auto marker = root / kPendingMarker;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(marker, ec))
        return result;

    result.pending = true;
    std::wstring token;
    if (!ReadPendingToken(marker, token, result.error))
    {
        result.ok = false;
        return result;
    }

    const auto staging =
        root / (std::wstring(L"staging-") + token);
    const auto backup =
        root / (std::wstring(L"backup-") + token);
    const auto data = stateRoot / L"data";
    result.backup = backup;

    const bool stagingExists =
        std::filesystem::is_directory(staging, ec);
    ec.clear();
    const bool backupExists =
        std::filesystem::is_directory(backup, ec);
    ec.clear();
    const bool dataExists =
        std::filesystem::is_directory(data, ec);
    ec.clear();

    // A previous startup may have committed the directory exchange and then
    // terminated before retiring the marker. Finalize that transaction.
    if (!stagingExists && backupExists && dataExists)
    {
        result.applied = true;
        result.ok = FinalizeMarker(
            marker, root, token, result.error);
        return result;
    }
    if (!stagingExists || !LooksLikeDataDirectory(staging))
    {
        result.ok = false;
        result.error = "staged portable data is missing or invalid";
        return result;
    }
    if (backupExists && dataExists)
    {
        result.ok = false;
        result.error = "migration backup and active data both already exist";
        return result;
    }

    bool movedCurrent = false;
    if (!backupExists && dataExists)
    {
        std::filesystem::rename(data, backup, ec);
        if (ec)
        {
            result.ok = false;
            result.error = "cannot back up active data: " + ec.message();
            return result;
        }
        movedCurrent = true;
    }

    std::filesystem::rename(staging, data, ec);
    if (ec)
    {
        result.ok = false;
        result.error = "cannot activate staged portable data: " + ec.message();
        if (movedCurrent)
        {
            std::error_code rollbackError;
            std::filesystem::rename(backup, data, rollbackError);
            if (rollbackError)
            {
                result.error += "; rollback failed: " +
                    rollbackError.message();
            }
        }
        return result;
    }

    result.applied = true;
    result.ok = FinalizeMarker(marker, root, token, result.error);
    return result;
}
}
