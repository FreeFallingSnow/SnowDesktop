#pragma once

#include <windows.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <optional>
#include <iterator>
#include <utility>
#include <string>

namespace snowdesktop::dock_explorer_pin
{
struct Folder
{
    std::wstring path;
    std::wstring name;
};

inline std::wstring ExecutablePath()
{
    wchar_t windows[32768]{};
    const UINT size = GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)));
    if (!size || size >= std::size(windows)) return {};
    return std::wstring(windows, size) + L"\\explorer.exe";
}

inline bool IsFolderWindow(HWND window)
{
    wchar_t name[64]{};
    return window && IsWindow(window) &&
        GetClassNameW(window, name, static_cast<int>(std::size(name))) &&
        (_wcsicmp(name, L"CabinetWClass") == 0 || _wcsicmp(name, L"ExploreWClass") == 0);
}

// Called only on a COM Shell worker. Window titles and URLs are not paths:
// obtain the visible view's PIDL and ask Shell for its filesystem identity.
inline std::optional<Folder> ReadCurrentFolder(HWND window)
{
    using Microsoft::WRL::ComPtr;
    if (!IsFolderWindow(window)) return std::nullopt;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (!thread || !process) return std::nullopt;
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
            IID_PPV_ARGS(&windows)))) return std::nullopt;
    long count = 0;
    if (FAILED(windows->get_Count(&count))) return std::nullopt;
    std::optional<Folder> result;
    for (long index = 0; index < count; ++index)
    {
        VARIANT item{};
        item.vt = VT_I4;
        item.lVal = index;
        ComPtr<IDispatch> dispatch;
        ComPtr<IWebBrowser2> automation;
        SHANDLE_PTR handle = 0;
        if (FAILED(windows->Item(item, &dispatch)) || !dispatch ||
            FAILED(dispatch.As(&automation)) || FAILED(automation->get_HWND(&handle)) ||
            reinterpret_cast<HWND>(handle) != window) continue;
        ComPtr<IServiceProvider> provider;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> shellView;
        ComPtr<IFolderView> view;
        ComPtr<IPersistFolder2> folder;
        HWND viewWindow = nullptr;
        if (FAILED(dispatch.As(&provider)) ||
            FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
            FAILED(browser->QueryActiveShellView(&shellView)) || !shellView ||
            FAILED(shellView->GetWindow(&viewWindow)) || !IsWindowVisible(viewWindow) ||
            FAILED(shellView.As(&view)) || FAILED(view->GetFolder(IID_PPV_ARGS(&folder))))
            continue;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(folder->GetCurFolder(&pidl)) || !pidl) continue;
        PWSTR path = nullptr, name = nullptr;
        Folder current;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &path)) && path)
            current.path = path;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &name)) && name)
            current.name = name;
        CoTaskMemFree(path);
        CoTaskMemFree(name);
        CoTaskMemFree(pidl);
        if (current.path.empty()) continue;
        if (current.name.empty()) current.name = current.path;
        // Fail closed if Windows exposes more than one visible tab with
        // different locations; never pin the first unrelated tab by accident.
        if (result && _wcsicmp(result->path.c_str(), current.path.c_str()) != 0)
            return std::nullopt;
        result = std::move(current);
    }
    DWORD currentProcess = 0;
    const DWORD currentThread = GetWindowThreadProcessId(window, &currentProcess);
    return IsFolderWindow(window) && currentProcess == process && currentThread == thread
        ? result : std::nullopt;
}
} // namespace snowdesktop::dock_explorer_pin
