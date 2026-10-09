#pragma once
#include "desktop/desktop_source.h"

#include <filesystem>
#include <cstdint>
#include <string>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

namespace snowdesktop
{
// Invocation-only identity. Never include a transient HWND in selection/cache keys.
struct ShellInvocationOwner
{
    std::uint64_t window = 0;
    DWORD process = 0, thread = 0;
    static ShellInvocationOwner Capture(HWND owner)
    {
        ShellInvocationOwner result;
        if (owner && IsWindow(owner) && GetAncestor(owner, GA_ROOT) == owner &&
            GetParent(owner) != HWND_MESSAGE)
        {
            result.thread = GetWindowThreadProcessId(owner, &result.process);
            result.window = reinterpret_cast<UINT_PTR>(owner);
        }
        return result;
    }
    HWND Resolve() const
    {
        const HWND owner = reinterpret_cast<HWND>(static_cast<UINT_PTR>(window));
        DWORD currentProcess = 0;
        const DWORD currentThread = owner ? GetWindowThreadProcessId(owner, &currentProcess) : 0;
        return process && thread && currentProcess == process && currentThread == thread &&
            IsWindow(owner) && GetAncestor(owner, GA_ROOT) == owner &&
            GetParent(owner) != HWND_MESSAGE ? owner : nullptr;
    }
};

inline UINT FindNewFolderCommand(IContextMenu* contextMenu, HMENU menu)
{
    if (!contextMenu || !menu) return 0;
    for (int i = 0; i < GetMenuItemCount(menu); ++i)
    {
        const UINT candidate = GetMenuItemID(menu, i);
        if (candidate == 0 || candidate == static_cast<UINT>(-1)) continue;
        char verb[128]{};
        if (SUCCEEDED(contextMenu->GetCommandString(candidate - 1,
                GCS_VERBA, nullptr, verb, static_cast<UINT>(sizeof(verb)))) &&
            lstrcmpiA(verb, "NewFolder") == 0 &&
            (GetMenuState(menu, candidate, MF_BYCOMMAND) &
                (MF_DISABLED | MF_GRAYED)) == 0)
            return candidate;
    }
    return 0;
}

inline std::wstring DesktopShellInvocationDirectory()
{
    return desktop_source::Directory();
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
