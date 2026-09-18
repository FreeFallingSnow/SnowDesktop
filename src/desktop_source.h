#pragma once

#include "debug_profile.h"
#include <windows.h>
#include <shlobj.h>

namespace snowdesktop::desktop_source
{
inline std::wstring Directory()
{
    if (debug_profile::Enabled()) return debug_profile::Current().configuration.desktop.wstring();
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &raw))) return {};
    const std::wstring result(raw);
    CoTaskMemFree(raw);
    return result;
}

// Compatibility adapter for existing Shell call sites with MAX_PATH buffers.
template<size_t N> bool CopyDirectory(wchar_t (&buffer)[N])
{
    const auto path = Directory();
    return !path.empty() && path.size() < N && wcscpy_s(buffer, path.c_str()) == 0;
}

inline std::vector<std::filesystem::path> SystemDesktops()
{
    std::vector<std::filesystem::path> result;
    for (const auto id : {FOLDERID_Desktop, FOLDERID_PublicDesktop})
    {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)))
        { result.emplace_back(raw); CoTaskMemFree(raw); }
    }
    return result;
}
}
