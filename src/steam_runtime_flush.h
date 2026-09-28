#pragma once
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <thread>

namespace snowdesktop::steam_runtime::detail
{
struct FlushResult
{
    DWORD error = ERROR_SUCCESS;
    unsigned attempts = 0;
    // Only verified, reconstructible payloads may tolerate unavailable flushing.
    // Durable selection/history/manifest writes never use this policy.
    bool WarningOnly() const noexcept
    {
        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
            error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION;
    }
};

template<class Wait>
FlushResult FlushRuntimePayload(const std::filesystem::path& path, Wait&& wait)
{
    FlushResult result;
    for (;;)
    {
        ++result.attempts;
        // Readers need not be excluded. Still deny concurrent writers/deletion.
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) result.error = GetLastError();
        else
        {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(file, &info)) result.error = GetLastError();
            else if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) result.error = ERROR_INVALID_DATA;
            else if (!FlushFileBuffers(file)) result.error = GetLastError();
            else result.error = ERROR_SUCCESS;
            if (!CloseHandle(file) && result.error == ERROR_SUCCESS) result.error = GetLastError();
        }
        if ((result.error != ERROR_SHARING_VIOLATION && result.error != ERROR_LOCK_VIOLATION) ||
            result.attempts >= 6) return result;
        wait(std::chrono::milliseconds(100));
    }
}
inline FlushResult FlushRuntimePayload(const std::filesystem::path& path)
{
    return FlushRuntimePayload(path,
        [](std::chrono::milliseconds delay) { std::this_thread::sleep_for(delay); });
}
}
