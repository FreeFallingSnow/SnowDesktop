#include "item_location.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <vector>

namespace snowdesktop::item_location
{
namespace
{

using Microsoft::WRL::ComPtr;

bool HasLinkExtension(const std::wstring& path)
{
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    return extension && lstrcmpiW(extension, L".lnk") == 0;
}

std::wstring ExpandEnvironmentPath(const std::wstring& path)
{
    if (path.empty())
        return {};

    const DWORD required = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (required == 0)
        return path;

    std::vector<wchar_t> expanded(required);
    if (ExpandEnvironmentStringsW(path.c_str(), expanded.data(), required) == 0)
        return path;
    return expanded.data();
}

std::wstring ResolveShellLinkTarget(const std::wstring& linkPath)
{
    ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(shellLink.GetAddressOf()))) ||
        !shellLink)
        return {};

    ComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink.As(&persistFile)) ||
        FAILED(persistFile->Load(linkPath.c_str(), STGM_READ)))
        return {};

    // Resolve without UI so stale links can still use the path stored in the
    // shortcut without interrupting a context-menu action.
    shellLink->Resolve(nullptr, SLR_NO_UI | SLR_NOUPDATE | SLR_NOTRACK);

    std::vector<wchar_t> target(32768);
    WIN32_FIND_DATAW findData{};
    if (SUCCEEDED(shellLink->GetPath(target.data(),
            static_cast<int>(target.size()), &findData, SLGP_RAWPATH)) &&
        target[0] != L'\0')
    {
        std::wstring expanded = ExpandEnvironmentPath(target.data());
        if (!expanded.empty())
            return expanded;
    }

    PIDLIST_ABSOLUTE targetPidl = nullptr;
    if (SUCCEEDED(shellLink->GetIDList(&targetPidl)) && targetPidl)
    {
        wchar_t fileSystemPath[MAX_PATH]{};
        const bool converted =
            SHGetPathFromIDListW(targetPidl, fileSystemPath) != FALSE;
        CoTaskMemFree(targetPidl);
        if (converted && fileSystemPath[0] != L'\0')
            return fileSystemPath;
    }

    return {};
}

} // namespace

std::wstring ResolveRevealPath(const std::wstring& path)
{
    if (path.empty())
        return {};

    if (HasLinkExtension(path))
    {
        std::wstring target = ResolveShellLinkTarget(path);
        if (!target.empty() &&
            GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
            return target;
    }

    return path;
}

bool CanReveal(const std::wstring& path)
{
    const std::wstring resolved = ResolveRevealPath(path);
    return !resolved.empty() &&
        GetFileAttributesW(resolved.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool Reveal(HWND owner, const std::wstring& path)
{
    const std::wstring resolved = ResolveRevealPath(path);
    if (resolved.empty() ||
        GetFileAttributesW(resolved.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    const std::wstring parameters = L"/select,\"" + resolved + L"\"";
    HINSTANCE result = ShellExecuteW(owner, L"open", L"explorer.exe",
        parameters.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

} // namespace snowdesktop::item_location
