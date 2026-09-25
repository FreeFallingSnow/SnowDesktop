#pragma once

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <thread>

namespace snowdesktop::steam_runtime::detail
{
struct PublishResult
{
    DWORD error = ERROR_SUCCESS;
    unsigned attempts = 0;
};

// A reader of a child file can make a directory rename return ACCESS_DENIED,
// even when the launcher has permission to create and write the entire tree.
// Retry only these potentially transient errors, with at most two seconds of
// waiting. Never copy over or replace an existing published runtime.
// The wait boundary is injectable so tests can release real Windows handles
// after a failed rename without depending on thread scheduling or sleeps.
template <typename WaitForRetry>
PublishResult PublishRuntimeDirectory(const std::filesystem::path& staging,
    const std::filesystem::path& destination, WaitForRetry&& waitForRetry)
{
    constexpr unsigned maximumRetries = 20;
    PublishResult result;
    for (;;)
    {
        ++result.attempts;
        if (MoveFileExW(staging.c_str(), destination.c_str(),
                MOVEFILE_WRITE_THROUGH))
        {
            result.error = ERROR_SUCCESS;
            return result;
        }
        result.error = GetLastError();
        if ((result.error != ERROR_ACCESS_DENIED &&
                result.error != ERROR_SHARING_VIOLATION &&
                result.error != ERROR_LOCK_VIOLATION) ||
            result.attempts > maximumRetries)
        {
            return result;
        }

        // An existing destination is a collision, not a transient reader.
        // Inspection failures other than absence must also fail closed.
        if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES)
            return result;
        const DWORD inspectError = GetLastError();
        if (inspectError != ERROR_FILE_NOT_FOUND &&
            inspectError != ERROR_PATH_NOT_FOUND)
        {
            return result;
        }
        waitForRetry(std::chrono::milliseconds(100));
    }
}

inline PublishResult PublishRuntimeDirectory(
    const std::filesystem::path& staging,
    const std::filesystem::path& destination)
{
    return PublishRuntimeDirectory(staging, destination,
        [](std::chrono::milliseconds delay) { std::this_thread::sleep_for(delay); });
}
}
