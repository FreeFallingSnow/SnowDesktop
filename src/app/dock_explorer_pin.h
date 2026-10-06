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
#include <vector>
#include <algorithm>

namespace snowdesktop::dock_explorer_pin
{
struct Folder
{
    std::wstring path;
    std::wstring name;
};

// The source token binds a menu read to the current Dock entry and item stamp.
// Sources include native shortcuts, direct folders and folder-mapping widgets.
struct FolderPinSource
{
    std::wstring path;
    std::wstring sourceToken;
    bool operator==(const FolderPinSource&) const = default;
};

inline std::wstring NormalizeFolderPath(std::wstring path)
{
    if (path.empty()) return {};
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    std::vector<wchar_t> expanded(32768);
    const DWORD expandedSize = ExpandEnvironmentStringsW(path.c_str(), expanded.data(),
        static_cast<DWORD>(expanded.size()));
    if (expandedSize && expandedSize <= expanded.size()) path.assign(expanded.data());
    std::vector<wchar_t> absolute(32768);
    const DWORD size = GetFullPathNameW(path.c_str(), static_cast<DWORD>(absolute.size()),
        absolute.data(), nullptr);
    if (size && size < absolute.size()) path.assign(absolute.data(), size);
    // Explorer may report long names while a saved link or TEMP uses 8.3
    // aliases (for example GUOYUN~1). Compare the same directory spelling.
    const DWORD longSize = GetLongPathNameW(path.c_str(), absolute.data(),
        static_cast<DWORD>(absolute.size()));
    if (longSize && longSize < absolute.size()) path.assign(absolute.data(), longSize);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

// COM/Shell worker only. Load the saved link target without Resolve/search,
// so deduplication cannot launch a program or retarget an existing shortcut.
inline bool AlreadyPinnedFolder(const std::wstring& folder,
    const std::vector<FolderPinSource>& sources)
{
    using Microsoft::WRL::ComPtr;
    const auto expected = NormalizeFolderPath(folder);
    if (expected.empty()) return false;
    for (const auto& source : sources)
    {
        auto target = source.path;
        if (target.size() >= 4 && _wcsicmp(target.c_str() + target.size() - 4, L".lnk") == 0)
        {
            ComPtr<IShellLinkW> link;
            ComPtr<IPersistFile> file;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&link))) || FAILED(link.As(&file)) ||
                FAILED(file->Load(target.c_str(), STGM_READ))) continue;
            wchar_t path[32768]{};
            target.clear();
            if (SUCCEEDED(link->GetPath(path, static_cast<int>(std::size(path)), nullptr,
                    SLGP_RAWPATH)) && path[0]) target = path;
            else
            {
                PIDLIST_ABSOLUTE pidl = nullptr;
                PWSTR filesystem = nullptr;
                if (SUCCEEDED(link->GetIDList(&pidl)) && pidl &&
                    SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &filesystem)) && filesystem)
                    target = filesystem;
                CoTaskMemFree(filesystem);
                CoTaskMemFree(pidl);
            }
        }
        const auto normalized = NormalizeFolderPath(std::move(target));
        if (!normalized.empty() && _wcsicmp(expected.c_str(), normalized.c_str()) == 0)
            return true;
    }
    return false;
}

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
