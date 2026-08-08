#pragma once

#include <filesystem>
#include <string>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

namespace snowdesktop
{

inline std::wstring DesktopShellInvocationDirectory()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &path)) ||
        !path)
    {
        return {};
    }
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

inline std::wstring ShellInvocationDirectoryForItem(
    const std::wstring& itemPath)
{
    if (itemPath.empty())
        return {};
    const DWORD attributes = GetFileAttributesW(itemPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return itemPath;
    }
    return std::filesystem::path(itemPath).parent_path().wstring();
}

inline void SetShellInvocationDirectory(
    CMINVOKECOMMANDINFOEX& invoke,
    const std::wstring& directory,
    std::string& ansiStorage)
{
    invoke.lpDirectory = nullptr;
    invoke.lpDirectoryW = directory.empty() ? nullptr : directory.c_str();
    ansiStorage.clear();
    if (directory.empty())
        return;

    const int byteCount = WideCharToMultiByte(
        CP_ACP, 0, directory.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
        return;
    ansiStorage.resize(static_cast<size_t>(byteCount));
    if (WideCharToMultiByte(
            CP_ACP, 0, directory.c_str(), -1,
            ansiStorage.data(), byteCount,
            nullptr, nullptr) > 0)
    {
        invoke.lpDirectory = ansiStorage.c_str();
    }
}

} // namespace snowdesktop
