#pragma once

#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace snowdesktop::data_paths
{
// The reader parameter is the OS boundary; no current-directory fallback is
// permitted because this path selects the user's persistent data directory.
inline std::filesystem::path ExecutableDirectory(
    DWORD (WINAPI* read)(HMODULE, LPWSTR, DWORD) = GetModuleFileNameW)
{
    for (DWORD capacity = MAX_PATH; capacity <= 32768;)
    {
        std::vector<wchar_t> buffer(capacity);
        const DWORD length = read(nullptr, buffer.data(), capacity);
        if (!length)
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(), "cannot locate the executable");
        if (length < capacity)
        {
            const std::filesystem::path path(std::wstring(buffer.data(), length));
            if (!path.is_absolute() || path.parent_path().empty())
                throw std::runtime_error("the executable path is not absolute");
            return path.parent_path();
        }
        if (capacity == 32768) break;
        capacity = (std::min)(capacity * 2, DWORD{32768});
    }
    throw std::system_error(ERROR_FILENAME_EXCED_RANGE,
        std::system_category(), "the executable path exceeds the Windows limit");
}
}
