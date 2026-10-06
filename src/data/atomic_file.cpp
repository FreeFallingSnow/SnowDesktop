#include "atomic_file.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

namespace snowdesktop::atomic_file
{
namespace
{
std::atomic<unsigned long long> temporaryFileCounter = 0;

void SetError(std::string* error, std::string_view operation, DWORD code)
{
    if (!error) return;
    *error = std::string(operation);
    *error += " failed with Win32 error ";
    *error += std::to_string(code);
}

std::filesystem::path MakeTemporaryPath(
    const std::filesystem::path& destination)
{
    std::wstring path = destination.native();
    path += L".tmp.";
    path += std::to_wstring(GetCurrentProcessId());
    path += L".";
    path += std::to_wstring(
        temporaryFileCounter.fetch_add(1, std::memory_order_relaxed));
    return path;
}

bool WriteHandle(HANDLE file, std::string_view contents, std::string* error)
{
    size_t offset = 0;
    while (offset < contents.size())
    {
        const size_t remaining = contents.size() - offset;
        const DWORD request = static_cast<DWORD>(std::min(
            remaining,
            static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(file, contents.data() + offset, request,
                &written, nullptr) || written != request)
        {
            SetError(error, "WriteFile", GetLastError());
            return false;
        }
        offset += written;
    }
    if (!FlushFileBuffers(file))
    {
        SetError(error, "FlushFileBuffers", GetLastError());
        return false;
    }
    return true;
}

bool CopyBackupAtomically(const std::filesystem::path& source,
    const std::filesystem::path& backup, std::string* error)
{
    if (backup.empty() ||
        GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return true;
    }

    const auto temporaryBackup = MakeTemporaryPath(backup);
    if (!CopyFileW(source.c_str(), temporaryBackup.c_str(), FALSE))
    {
        SetError(error, "CopyFileW", GetLastError());
        DeleteFileW(temporaryBackup.c_str());
        return false;
    }
    if (!MoveFileExW(temporaryBackup.c_str(), backup.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        SetError(error, "MoveFileExW(backup)", GetLastError());
        DeleteFileW(temporaryBackup.c_str());
        return false;
    }
    return true;
}
}

bool ReadAll(const std::filesystem::path& path, std::string& contents,
    std::string* error)
{
    if (error) error->clear();
    contents.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        if (error) *error = "cannot open file for reading";
        return false;
    }
    contents.assign(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
    if (file.bad())
    {
        if (error) *error = "cannot read complete file";
        contents.clear();
        return false;
    }
    return true;
}

bool WriteAll(const std::filesystem::path& path, std::string_view contents,
    const std::filesystem::path& backupPath, std::string* error)
{
    if (error) error->clear();
    std::error_code directoryError;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(
            path.parent_path(), directoryError);
        if (directoryError)
        {
            if (error)
                *error = "cannot create parent directory: " +
                    directoryError.message();
            return false;
        }
    }

    const auto temporary = MakeTemporaryPath(path);
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        SetError(error, "CreateFileW", GetLastError());
        return false;
    }

    const bool wrote = WriteHandle(file, contents, error);
    const bool closed = CloseHandle(file) != FALSE;
    if (!wrote || !closed)
    {
        if (wrote && !closed)
            SetError(error, "CloseHandle", GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }

    if (!CopyBackupAtomically(path, backupPath, error))
    {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        SetError(error, "MoveFileExW", GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}
}
