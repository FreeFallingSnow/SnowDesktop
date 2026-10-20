#pragma once

#include "system/calendar/calendar_service.h"

#include <windows.h>
#include <array>
#include <filesystem>

namespace snowdesktop::widget_runtime
{
// The service's legacy loader treats every non-regular path as a missing file.
// Check both actual stores before calling it: only genuinely missing stores may
// become a new empty calendar. Existing files remain readable and cannot be
// replaced while the real loader reads them. Repeating this operation after an
// explicit host reinitialization can recover a repaired or unlocked store.
inline bool LoadWidgetCalendarProvider(
    snowdesktop::calendar::CalendarService& service,
    const std::filesystem::path& calendarPath)
{
    const std::array paths{calendarPath, calendarPath.parent_path() /
        (calendarPath.stem().wstring() + L"-series.json")};
    struct Leases
    {
        std::array<HANDLE, 2> files{INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
        ~Leases()
        {
            for (const HANDLE file : files)
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        }
    } leases;
    for (std::size_t index = 0; index < paths.size(); ++index)
    {
        const auto& path = paths[index];
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                continue;
            return false;
        }
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        leases.files[index] = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (leases.files[index] == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(leases.files[index], &information) ||
            (information.dwFileAttributes &
                (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            return false;
    }
    return service.Load();
}
}
