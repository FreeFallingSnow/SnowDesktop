#pragma once

#include <windows.h>
#include <filesystem>

namespace snowdesktop::dock_pin_cleanup
{
inline bool ConfirmedMissing(DWORD driveType, DWORD error, bool parentReadable)
{
    return driveType == DRIVE_FIXED && parentReadable &&
        (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
}

// Run on a Shell worker. An absent model item or failed icon lookup is not
// deletion evidence: hidden items, disconnected drives and denied reads retain
// their pins. Probe the source file itself, never a shortcut's target.
inline bool IsMissingLocalFile(const std::wstring& value)
{
    const std::filesystem::path path(value);
    if (!path.is_absolute() || path.filename().empty()) return false;
    const DWORD driveType = GetDriveTypeW(path.root_path().c_str());
    if (driveType != DRIVE_FIXED) return false;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD firstError = GetLastError();
    if (!ConfirmedMissing(driveType, firstError, true)) return false;

    const auto parent = path.parent_path();
    const DWORD attributes = GetFileAttributesW(parent.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    WIN32_FIND_DATAW data{};
    const HANDLE search = FindFirstFileW((parent / L"*").c_str(), &data);
    const bool parentReadable = search != INVALID_HANDLE_VALUE || GetLastError() == ERROR_FILE_NOT_FOUND;
    if (search != INVALID_HANDLE_VALUE) FindClose(search);
    // Recheck after the parent probe so a recreated file is retained.
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    return ConfirmedMissing(driveType, GetLastError(), parentReadable);
}
}
