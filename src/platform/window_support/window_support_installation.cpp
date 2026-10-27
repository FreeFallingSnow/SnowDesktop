#include "window_support_installation.h"
#include <new>

namespace snowdesktop::window_support
{
DWORD PublishHelperBundle(const std::filesystem::path& staging, const std::filesystem::path& destination,
    std::filesystem::path& retiredDirectory) noexcept
{
    try
    {
        if (!staging.is_absolute() || !destination.is_absolute() || staging == destination ||
            staging.parent_path() != destination.parent_path()) return ERROR_INVALID_PARAMETER;
        const auto attributes = GetFileAttributesW(staging.c_str());
        const auto existing = GetFileAttributesW(destination.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_PATH_NOT_FOUND;
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || (existing != INVALID_FILE_ATTRIBUTES &&
            ((existing & FILE_ATTRIBUTE_REPARSE_POINT) || !(existing & FILE_ATTRIBUTE_DIRECTORY)))) return ERROR_ACCESS_DENIED;
        retiredDirectory = destination.parent_path() / (destination.filename().wstring() + L".retired-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        const bool present = existing != INVALID_FILE_ATTRIBUTES;
        if (present && !MoveFileExW(destination.c_str(), retiredDirectory.c_str(), MOVEFILE_WRITE_THROUGH)) return GetLastError();
        if (!MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            const DWORD error = GetLastError();
            if (present) MoveFileExW(retiredDirectory.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH);
            return error;
        }
        if (!present) retiredDirectory.clear();
        return ERROR_SUCCESS;
    }
    catch (const std::bad_alloc&) { return ERROR_OUTOFMEMORY; }
    catch (...) { return ERROR_GEN_FAILURE; }
}
}
