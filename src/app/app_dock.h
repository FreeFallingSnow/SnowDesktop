#pragma once

#include "../json_value.h"

#include <cmath>
#include <ctime>
#include <limits>
#include <numeric>

extern inline const GridPage* FindGridPage(
    const std::vector<GridPage>& pages, const std::wstring& pageId);
extern inline int SlotFromCell(
    const std::vector<GridPage>& pages, const GridCell& cell);

inline std::wstring NormalizeDockExecutablePath(std::wstring path)
{
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
        path = path.substr(1, path.size() - 2);

    const DWORD expandedLength = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (expandedLength > 1)
    {
        std::vector<wchar_t> expanded(expandedLength);
        if (ExpandEnvironmentStringsW(path.c_str(), expanded.data(), expandedLength) > 0)
            path.assign(expanded.data());
    }

    std::vector<wchar_t> fullPath(32768);
    const DWORD fullLength = GetFullPathNameW(path.c_str(),
        static_cast<DWORD>(fullPath.size()), fullPath.data(), nullptr);
    if (fullLength > 0 && fullLength < fullPath.size())
        path.assign(fullPath.data(), fullLength);

    std::replace(path.begin(), path.end(), L'/', L'\\');
    return ToUpperInvariant(path);
}

inline std::wstring ReadDockRegistryString(
    HKEY root, const std::wstring& subKey, const wchar_t* valueName)
{
    DWORD size = 0;
    if (RegGetValueW(root, subKey.c_str(), valueName,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t))
        return {};

    std::vector<wchar_t> value(size / sizeof(wchar_t));
    if (RegGetValueW(root, subKey.c_str(), valueName,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS)
        return {};
    return value.data();
}

inline std::string ReadDockTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

inline std::vector<std::string> ReadDockVdfQuotedValues(
    const std::string& text, const std::string& key)
{
    std::vector<std::string> values;
    const std::string token = "\"" + key + "\"";
    size_t position = 0;
    while ((position = text.find(token, position)) != std::string::npos)
    {
        size_t cursor = position + token.size();
        while (cursor < text.size() &&
            (text[cursor] == ' ' || text[cursor] == '\t' ||
                text[cursor] == '\r' || text[cursor] == '\n'))
            ++cursor;
        if (cursor >= text.size() || text[cursor] != '"')
        {
            position += token.size();
            continue;
        }

        ++cursor;
        std::string value;
        while (cursor < text.size())
        {
            const char current = text[cursor++];
            if (current == '"') break;
            if (current == '\\' && cursor < text.size())
            {
                const char escaped = text[cursor];
                if (escaped == '\\' || escaped == '"')
                {
                    value.push_back(escaped);
                    ++cursor;
                    continue;
                }
            }
            value.push_back(current);
        }
        if (!value.empty()) values.push_back(std::move(value));
        position = cursor;
    }
    return values;
}

inline std::wstring ParseDockSteamAppId(const std::wstring& shortcutPath)
{
    wchar_t url[2048]{};
    GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url,
        static_cast<DWORD>(std::size(url)), shortcutPath.c_str());
    const std::wstring upper = ToUpperInvariant(url);
    constexpr std::array prefixes{
        std::wstring_view(L"STEAM://RUNGAMEID/"),
        std::wstring_view(L"STEAM://RUN/")
    };
    for (const std::wstring_view prefix : prefixes)
    {
        if (!upper.starts_with(prefix)) continue;
        size_t end = prefix.size();
        while (end < upper.size() && upper[end] >= L'0' && upper[end] <= L'9')
            ++end;
        if (end == prefix.size()) return {};
        if (end < upper.size() && upper[end] != L'/' && upper[end] != L'?' &&
            upper[end] != L'#' && upper[end] != L' ' && upper[end] != L'\t')
            return {};
        return upper.substr(prefix.size(), end - prefix.size());
    }
    return {};
}

inline std::wstring FindDockSteamAppInstallDirectory(const std::wstring& appId)
{
    std::wstring steamPath = ReadDockRegistryString(
        HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steamPath.empty())
        steamPath = ReadDockRegistryString(
            HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steamPath.empty()) return {};

    std::vector<std::filesystem::path> libraries;
    libraries.emplace_back(steamPath);
    const std::string libraryFolders = ReadDockTextFile(
        std::filesystem::path(steamPath) / L"steamapps" / L"libraryfolders.vdf");
    for (const std::string& path : ReadDockVdfQuotedValues(libraryFolders, "path"))
    {
        const std::wstring widePath = Utf8ToWide(path);
        if (!widePath.empty()) libraries.emplace_back(widePath);
    }

    std::unordered_set<std::wstring> visited;
    for (const std::filesystem::path& library : libraries)
    {
        const std::wstring normalizedLibrary = NormalizeDockExecutablePath(library.wstring());
        if (!visited.insert(normalizedLibrary).second) continue;

        const std::filesystem::path manifest = library / L"steamapps" /
            (L"appmanifest_" + appId + L".acf");
        std::error_code error;
        if (!std::filesystem::is_regular_file(manifest, error)) continue;
        const std::string manifestText = ReadDockTextFile(manifest);
        const std::vector<std::string> installDirs =
            ReadDockVdfQuotedValues(manifestText, "installdir");
        if (installDirs.empty()) continue;
        const std::wstring installDir = Utf8ToWide(installDirs.front());
        if (installDir.empty()) continue;
        return NormalizeDockExecutablePath(
            (library / L"steamapps" / L"common" / installDir).wstring());
    }
    return {};
}

inline bool IsDockSteamAppRunning(const std::wstring& appId)
{
    if (appId.empty()) return false;
    DWORD running = 0;
    DWORD size = sizeof(running);
    const std::wstring subKey = L"Software\\Valve\\Steam\\Apps\\" + appId;
    return RegGetValueW(HKEY_CURRENT_USER, subKey.c_str(), L"Running",
        RRF_RT_REG_DWORD, nullptr, &running, &size) == ERROR_SUCCESS && running != 0;
}

inline bool IsDockPathInsideDirectory(
    const std::wstring& path, const std::wstring& directory)
{
    if (path.empty() || directory.empty() || path.size() <= directory.size() ||
        path.compare(0, directory.size(), directory) != 0)
        return false;
    return directory.back() == L'\\' || path[directory.size()] == L'\\';
}

inline std::wstring ReadDockStringProperty(
    IPropertyStore* propertyStore, REFPROPERTYKEY propertyKey)
{
    if (!propertyStore) return {};
    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(propertyStore->GetValue(propertyKey, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal)
        result = value.pwszVal;
    PropVariantClear(&value);
    return result;
}

inline std::wstring ReadDockAppUserModelId(IPropertyStore* propertyStore)
{
    return ToUpperInvariant(ReadDockStringProperty(
        propertyStore, PKEY_AppUserModel_ID));
}

inline std::wstring ReadDockShellItemStringProperty(
    const std::wstring& path, REFPROPERTYKEY propertyKey)
{
    ComPtr<IShellItem2> shellItem;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr,
            IID_PPV_ARGS(&shellItem))) || !shellItem)
        return {};
    PWSTR value = nullptr;
    std::wstring result;
    if (SUCCEEDED(shellItem->GetString(propertyKey, &value)) && value)
        result = value;
    if (value) CoTaskMemFree(value);
    return result;
}

inline std::wstring QueryDockWindowExecutablePath(HWND window)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!processId) return {};

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    wchar_t path[32768]{};
    DWORD pathLength = static_cast<DWORD>(std::size(path));
    const bool queried = QueryFullProcessImageNameW(process, 0, path, &pathLength) != FALSE;
    CloseHandle(process);
    return queried ? NormalizeDockExecutablePath(std::wstring(path, pathLength)) : std::wstring{};
}

inline std::wstring QueryDockWindowAppUserModelId(HWND window)
{
    ComPtr<IPropertyStore> propertyStore;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&propertyStore))))
    {
        std::wstring appUserModelId = ReadDockAppUserModelId(propertyStore.Get());
        if (!appUserModelId.empty()) return appUserModelId;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!processId) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    UINT32 length = 0;
    LONG result = GetApplicationUserModelId(process, &length, nullptr);
    std::wstring appUserModelId;
    if (result == ERROR_INSUFFICIENT_BUFFER && length > 1)
    {
        std::vector<wchar_t> value(length);
        if (GetApplicationUserModelId(process, &length, value.data()) == ERROR_SUCCESS)
            appUserModelId = ToUpperInvariant(value.data());
    }
    CloseHandle(process);
    return appUserModelId;
}

inline HBITMAP CreateDockShellIconBitmap(
    const std::wstring& parsingName, SIZE& bitmapSize,
    int* systemIconIndex = nullptr)
{
    if (systemIconIndex)
        *systemIconIndex = -1;
    if (parsingName.empty()) return nullptr;

    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF attributes = 0;
    if (FAILED(SHParseDisplayName(parsingName.c_str(), nullptr,
            &pidl, 0, &attributes)) || !pidl)
        return nullptr;

    SHFILEINFOW info{};
    int fallbackIndex = -1;
    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info,
            sizeof(info), SHGFI_PIDL | SHGFI_SYSICONINDEX))
        fallbackIndex = info.iIcon;
    if (systemIconIndex)
        *systemIconIndex = fallbackIndex;

    HBITMAP bitmap = GetHighResolutionShellIconBitmap(
        pidl, fallbackIndex, bitmapSize, false);
    CoTaskMemFree(pidl);
    return bitmap;
}

inline int QueryDockGenericExecutableIconIndex()
{
    static const int iconIndex = [] {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(
                L"SnowDesktop.GenericExecutable.exe",
                FILE_ATTRIBUTE_NORMAL, &info,
                sizeof(info),
                SHGFI_SYSICONINDEX |
                    SHGFI_USEFILEATTRIBUTES))
            return info.iIcon;
        return -1;
    }();
    return iconIndex;
}

inline HBITMAP CreateDockWindowProvidedIconBitmap(
    HWND window, SIZE& bitmapSize)
{
    if (!window || !IsWindow(window))
        return nullptr;

    HICON icon = nullptr;
    DWORD_PTR iconResult = 0;
    if (SendMessageTimeoutW(window, WM_GETICON, ICON_BIG, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 80, &iconResult))
        icon = reinterpret_cast<HICON>(iconResult);
    if (!icon && SendMessageTimeoutW(
            window, WM_GETICON, ICON_SMALL2, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 80, &iconResult))
        icon = reinterpret_cast<HICON>(iconResult);
    if (!icon && SendMessageTimeoutW(
            window, WM_GETICON, ICON_SMALL, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 80, &iconResult))
        icon = reinterpret_cast<HICON>(iconResult);
    if (!icon)
        icon = reinterpret_cast<HICON>(
            GetClassLongPtrW(window, GCLP_HICON));
    if (!icon)
        icon = reinterpret_cast<HICON>(
            GetClassLongPtrW(window, GCLP_HICONSM));
    if (!icon)
        return nullptr;
    return CreateAlphaBitmapFromIcon(
        icon, kIconBitmapSize, kIconBitmapSize,
        bitmapSize);
}

inline HBITMAP CreateDockWindowIconBitmap(
    HWND window, const std::wstring& executablePath,
    const std::wstring& appUserModelId, SIZE& bitmapSize)
{
    // Prefer stable high-resolution application identity icons. Some classic
    // Win32 hosts (for example Creo's xtop.exe) expose only the generic
    // executable icon through the Shell, so that result is allowed to fall
    // through to the window-provided icon below.
    if (!appUserModelId.empty())
    {
        if (HBITMAP bitmap = CreateDockShellIconBitmap(
                L"shell:AppsFolder\\" + appUserModelId, bitmapSize))
            return bitmap;
    }

    int executableIconIndex = -1;
    SIZE executableBitmapSize{};
    HBITMAP executableBitmap =
        CreateDockShellIconBitmap(
            executablePath, executableBitmapSize,
            &executableIconIndex);
    const int genericExecutableIconIndex =
        QueryDockGenericExecutableIconIndex();
    const bool executableIconIsGeneric =
        executableIconIndex >= 0 &&
        genericExecutableIconIndex >= 0 &&
        executableIconIndex ==
            genericExecutableIconIndex;
    if (snowdesktop::dock_window_rules::
            ResolveDockWindowIconSource(
                false, executableBitmap != nullptr,
                executableIconIsGeneric, false) ==
        snowdesktop::dock_window_rules::
            DockWindowIconSource::Executable)
    {
        bitmapSize = executableBitmapSize;
        return executableBitmap;
    }

    SIZE windowBitmapSize{};
    HBITMAP windowBitmap =
        CreateDockWindowProvidedIconBitmap(
            window, windowBitmapSize);
    const auto source =
        snowdesktop::dock_window_rules::
            ResolveDockWindowIconSource(
                false, executableBitmap != nullptr,
                executableIconIsGeneric,
                windowBitmap != nullptr);
    if (source ==
        snowdesktop::dock_window_rules::
            DockWindowIconSource::Window)
    {
        if (executableBitmap)
            DeleteObject(executableBitmap);
        bitmapSize = windowBitmapSize;
        return windowBitmap;
    }
    if (windowBitmap)
        DeleteObject(windowBitmap);
    if (executableBitmap)
    {
        bitmapSize = executableBitmapSize;
        return executableBitmap;
    }
    return nullptr;
}

inline bool DockWindowMatchesAppIdentity(
    HWND window, const DockAppIdentity& identity)
{
    if (!window || !IsWindow(window)) return false;
    window = GetAncestor(window, GA_ROOT);
    const std::wstring executablePath = QueryDockWindowExecutablePath(window);
    switch (identity.kind)
    {
    case DockAppIdentityKind::Executable:
        return !identity.executablePath.empty() &&
            executablePath == identity.executablePath;
    case DockAppIdentityKind::Applications:
        return !identity.appUserModelId.empty() &&
            QueryDockWindowAppUserModelId(window) == identity.appUserModelId;
    case DockAppIdentityKind::Steam:
        return (!identity.appUserModelId.empty() &&
                QueryDockWindowAppUserModelId(window) == identity.appUserModelId) ||
            IsDockPathInsideDirectory(executablePath,
                identity.steamInstallDirectory);
    default:
        return false;
    }
}

inline bool DockWindowsShareActivationGroup(HWND first, HWND second)
{
    if (!first || !second || !IsWindow(first) || !IsWindow(second))
        return false;
    const HWND firstRoot = GetAncestor(first, GA_ROOT);
    const HWND secondRoot = GetAncestor(second, GA_ROOT);
    const HWND firstOwner = GetAncestor(first, GA_ROOTOWNER);
    const HWND secondOwner = GetAncestor(second, GA_ROOTOWNER);
    return firstRoot == secondRoot || firstRoot == secondOwner ||
        firstOwner == secondRoot || firstOwner == secondOwner;
}

inline bool DockWindowsShareApplicationIdentity(
    HWND first, HWND second)
{
    if (!first || !second || !IsWindow(first) || !IsWindow(second))
        return false;
    first = GetAncestor(first, GA_ROOT);
    second = GetAncestor(second, GA_ROOT);
    if (first == second)
        return true;

    const std::wstring firstAppUserModelId =
        QueryDockWindowAppUserModelId(first);
    const std::wstring secondAppUserModelId =
        QueryDockWindowAppUserModelId(second);
    if (!firstAppUserModelId.empty() &&
        !secondAppUserModelId.empty())
    {
        return firstAppUserModelId ==
            secondAppUserModelId;
    }

    const std::wstring firstExecutable =
        QueryDockWindowExecutablePath(first);
    return !firstExecutable.empty() &&
        firstExecutable ==
            QueryDockWindowExecutablePath(second);
}

inline bool IsDockTaskWindow(HWND window)
{
    if (!window || GetAncestor(window, GA_ROOT) != window)
        return false;
    if (!IsWindowVisible(window) && !IsIconic(window))
        return false;
    wchar_t className[64]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"Progman") == 0 ||
        _wcsicmp(className, L"WorkerW") == 0 ||
        _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
        return false;
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!snowdesktop::dock_window_rules::IsTaskWindowStyleEligible(
            exStyle, GetWindow(window, GW_OWNER) != nullptr))
        return false;
    return true;
}

inline int DockRestoreShowCommand(HWND window)
{
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!window ||
        !GetWindowPlacement(window, &placement))
        return SW_RESTORE;
    return snowdesktop::dock_window_rules::
        ResolveDockRestoreShowCommand(
            placement.flags, placement.showCmd);
}

/**
 * @brief 请求最小化窗口，并为高完整性窗口提供默认系统命令回退。
 */
inline bool RequestDockWindowMinimize(HWND window)
{
    if (!window || !IsWindow(window))
        return false;
    const BOOL accepted =
        ShowWindowAsync(window, SW_MINIMIZE);
    if (snowdesktop::dock_window_rules::
            NeedsDockMinimizeSystemCommandFallback(
                accepted != FALSE))
    {
        DefWindowProcW(
            window, WM_SYSCOMMAND,
            SC_MINIMIZE, 0);
    }
    return accepted != FALSE ||
        IsIconic(window) != FALSE;
}

/**
 * @brief 请求窗口正常关闭，保留应用自己的保存确认与退出处理。
 */
inline bool RequestDockWindowClose(HWND window)
{
    if (!window || !IsWindow(window))
        return false;
    const BOOL accepted =
        PostMessageW(window, WM_CLOSE, 0, 0);
    if (snowdesktop::dock_window_rules::
            NeedsDockCloseSystemCommandFallback(
                accepted != FALSE))
    {
        DefWindowProcW(
            window, WM_SYSCOMMAND,
            SC_CLOSE, 0);
    }
    return accepted != FALSE ||
        !IsWindow(window);
}

inline std::wstring DockWindowPreviewIdentityKey(
    const DockAppIdentity& identity)
{
    switch (identity.kind)
    {
    case DockAppIdentityKind::Executable:
        return identity.executablePath.empty()
            ? std::wstring{} : L"EXE:" + identity.executablePath;
    case DockAppIdentityKind::Applications:
        return identity.appUserModelId.empty()
            ? std::wstring{} : L"AUMID:" + identity.appUserModelId;
    case DockAppIdentityKind::Steam:
        if (!identity.steamAppId.empty())
            return L"STEAM:" + identity.steamAppId;
        return identity.steamInstallDirectory.empty()
            ? std::wstring{}
            : L"STEAM_PATH:" + identity.steamInstallDirectory;
    default:
        return {};
    }
}

inline std::wstring DockWindowPreviewTargetToken(
    const std::wstring& identityKey, const RECT& anchorScreen)
{
    if (identityKey.empty())
        return {};
    return identityKey + L"@" +
        std::to_wstring(anchorScreen.left) + L"," +
        std::to_wstring(anchorScreen.top) + L"," +
        std::to_wstring(anchorScreen.right) + L"," +
        std::to_wstring(anchorScreen.bottom);
}

inline UINT QueryDockWindowPreviewHoverTime()
{
    UINT hoverTime = kDockWindowPreviewHoverFallbackMs;
    if (!SystemParametersInfoW(
            SPI_GETMOUSEHOVERTIME, 0, &hoverTime, 0))
        hoverTime = kDockWindowPreviewHoverFallbackMs;
    return std::max<UINT>(1, hoverTime);
}

inline void DesktopApp::PruneDockPendingCloseWindows()
{
    const ULONGLONG now = GetTickCount64();
    std::erase_if(dockPendingCloseWindows_,
        [now](const auto& entry) {
            return !entry.first ||
                !IsWindow(entry.first) ||
                now - entry.second >=
                    kDockWindowClosePendingTimeoutMs;
        });
}

inline bool DesktopApp::IsDockWindowClosePending(HWND window)
{
    PruneDockPendingCloseWindows();
    if (!window)
        return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root)
        root = window;
    return dockPendingCloseWindows_.contains(root);
}

inline bool DesktopApp::IsDockAppClosePending(
    const DockAppIdentity& identity)
{
    if (identity.kind == DockAppIdentityKind::None)
        return false;
    PruneDockPendingCloseWindows();
    return std::any_of(
        dockPendingCloseWindows_.begin(),
        dockPendingCloseWindows_.end(),
        [&identity](const auto& entry) {
            return DockWindowMatchesAppIdentity(
                entry.first, identity);
        });
}

inline bool DesktopApp::RequestTrackedDockWindowClose(
    HWND window)
{
    if (!window || !IsWindow(window))
        return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root)
        root = window;

    dockPendingCloseWindows_[root] = GetTickCount64();
    if (RequestDockWindowClose(root))
        return true;

    dockPendingCloseWindows_.erase(root);
    return false;
}

inline std::vector<DockWindowPreviewItem>
DesktopApp::CollectDockWindowPreviewItems(
    const DockAppIdentity& identity,
    bool includeCloaked)
{
    PruneDockPendingCloseWindows();
    struct PreviewEnumerationContext
    {
        const DockAppIdentity* identity = nullptr;
        std::vector<DockWindowPreviewItem>* items = nullptr;
        const std::unordered_map<HWND, ULONGLONG>*
            pendingCloseWindows = nullptr;
        bool includeCloaked = false;
    } context{
        &identity, nullptr, &dockPendingCloseWindows_,
        includeCloaked
    };

    std::vector<DockWindowPreviewItem> items;
    context.items = &items;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* context = reinterpret_cast<
            PreviewEnumerationContext*>(parameter);
        if (!context || !context->identity || !context->items ||
            !IsDockTaskWindow(window) ||
            (context->pendingCloseWindows &&
             context->pendingCloseWindows->contains(window)))
            return TRUE;

        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (!processId || processId == GetCurrentProcessId())
            return TRUE;

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(
                window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
            cloaked != 0 &&
            !context->includeCloaked)
            return TRUE;
        if (!DockWindowMatchesAppIdentity(window, *context->identity))
            return TRUE;

        wchar_t titleBuffer[512]{};
        GetWindowTextW(window, titleBuffer,
            static_cast<int>(std::size(titleBuffer)));
        std::wstring title = titleBuffer;
        if (title.empty())
        {
            const std::wstring executablePath =
                QueryDockWindowExecutablePath(window);
            title = PathFindFileNameW(executablePath.c_str());
        }
        context->items->push_back({ window, std::move(title) });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return items;
}

inline void DesktopApp::CloseDockWindowFromPreview(
    HWND window)
{
    DismissDockWindowPreviewUntilLeave();
    RequestTrackedDockWindowClose(window);
    RefreshDockRunningWindows();
}

inline void DesktopApp::CloseDockApplicationWindows(
    const DockAppIdentity& identity)
{
    DismissDockWindowPreviewUntilLeave();
    const std::vector<DockWindowPreviewItem> windows =
        CollectDockWindowPreviewItems(
            identity, true);
    for (const DockWindowPreviewItem& item : windows)
        RequestTrackedDockWindowClose(item.window);
    RefreshDockRunningWindows();
}

inline bool DesktopApp::ResolveDockWindowPreviewTarget(
    POINT clientPoint, DockWindowPreviewTarget& target)
{
    target = {};

    DockContainer* dock = GetDockContainerAtPoint(clientPoint);
    if (!dock || !dock->ContainsInteractivePoint(clientPoint))
        return false;
    target.floatingLayer =
        floatingDockVisible_ &&
        dock == floatingDockContainer_;

    RECT anchor{};
    bool found = false;
    if (DockRunningItem* running = dock->RunningItemAtPoint(clientPoint))
    {
        const size_t index = running->GetRunningIndex();
        if (index < dockUnpinnedRunningApps_.size())
        {
            const DockRunningAppInfo& app =
                dockUnpinnedRunningApps_[index];
            target.identity.executablePath = app.executablePath;
            target.identity.appUserModelId = app.appUserModelId;
            target.identity.kind =
                !target.identity.appUserModelId.empty()
                ? DockAppIdentityKind::Applications
                : DockAppIdentityKind::Executable;
            anchor = running->GetBounds();
            found = true;
        }
    }
    else if (DockEntryItem* entry = dock->EntryAtPoint(clientPoint))
    {
        const size_t entryIndex = entry->GetEntryIndex();
        if (entryIndex < dockEntries_.size() &&
            dockEntries_[entryIndex].type ==
                DockEntryType::DesktopItem)
        {
            const size_t itemIndex = FindItemIndexByKey(
                dockEntries_[entryIndex].reference);
            if (itemIndex < items_.size() &&
                GetDockWindowVisualState(itemIndex) !=
                    DockWindowVisualState::Closed)
            {
                target.identity = ResolveDockAppIdentity(itemIndex);
                anchor = entry->GetBounds();
                found = target.identity.kind !=
                    DockAppIdentityKind::None;
            }
        }
    }
    else if (DockFrequentItem* frequent =
        dock->FrequentItemAtPoint(clientPoint))
    {
        const size_t itemIndex = frequent->GetItemIndex();
        if (itemIndex < items_.size() &&
            GetDockWindowVisualState(itemIndex) !=
                DockWindowVisualState::Closed)
        {
            target.identity = ResolveDockAppIdentity(itemIndex);
            anchor = frequent->GetBounds();
            found = target.identity.kind !=
                DockAppIdentityKind::None;
        }
    }

    if (!found)
        return false;
    anchor = dock->GetElementVisualRect(anchor, clientPoint);
    target.identityKey =
        DockWindowPreviewIdentityKey(target.identity);
    if (target.identityKey.empty())
        return false;

    target.anchorScreen = anchor;
    MapWindowPoints(hwnd_, nullptr,
        reinterpret_cast<POINT*>(&target.anchorScreen), 2);
    target.targetToken = DockWindowPreviewTargetToken(
        target.identityKey, target.anchorScreen);
    return !target.targetToken.empty();
}

inline void DesktopApp::UpdateDockWindowPreview(POINT clientPoint)
{
    if (!dockWindowPreview_)
        return;
    if (!generalSettings_.dockEnabled || dragSession_.IsActive())
    {
        HideDockWindowPreview();
        return;
    }

    DockWindowPreviewTarget target;
    const bool hasTarget =
        ResolveDockWindowPreviewTarget(clientPoint, target);
    const bool previewVisible =
        dockWindowPreview_->IsVisible();
    const bool previewMatchesTarget =
        hasTarget && dockWindowPreviewKey_ == target.targetToken;
    const DockPreviewHoverTransition transition =
        dockWindowPreviewHover_.UpdateTarget(
            hasTarget ? target.targetToken : std::wstring{},
            previewVisible, previewMatchesTarget);

    if (transition.cancelTimer && hwnd_)
        KillTimer(hwnd_, kDockWindowPreviewHoverTimerId);
    if (transition.keepPreviewVisible)
        dockWindowPreview_->KeepVisible();
    if (transition.schedulePreviewHide)
        dockWindowPreview_->ScheduleHide();
    if (transition.armTimer && hwnd_)
    {
        SetTimer(hwnd_, kDockWindowPreviewHoverTimerId,
            QueryDockWindowPreviewHoverTime(), nullptr);
    }
}

inline void DesktopApp::OnDockWindowPreviewHoverTimer()
{
    if (!hwnd_ || !dockWindowPreview_)
        return;
    KillTimer(hwnd_, kDockWindowPreviewHoverTimerId);

    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    POINT cursorClient = cursorScreen;
    ScreenToClient(hwnd_, &cursorClient);

    DockWindowPreviewTarget target;
    const bool hasTarget =
        generalSettings_.dockEnabled &&
        !dragSession_.IsActive() &&
        ResolveDockWindowPreviewTarget(cursorClient, target);
    const std::wstring observedToken =
        hasTarget ? target.targetToken : std::wstring{};
    if (!dockWindowPreviewHover_.ConsumeTimer(observedToken))
    {
        UpdateDockWindowPreview(cursorClient);
        return;
    }

    std::vector<DockWindowPreviewItem> previewItems =
        CollectDockWindowPreviewItems(target.identity);
    if (previewItems.empty())
        return;
    dockWindowPreviewKey_ = target.targetToken;
    dockWindowPreviewAnchorScreen_ = target.anchorScreen;
    dockWindowPreview_->Show(
        previewItems, target.anchorScreen, dockSettings_.position,
        IsLightContentTheme(),
        target.floatingLayer
            ? floatingDockHwnd_ : nullptr);
    if (dockWindowPreview_->IsVisible())
        dockWindowPreviewHover_.MarkPreviewShown(
            target.targetToken);
}

inline void DesktopApp::HideDockWindowPreview()
{
    if (hwnd_)
        KillTimer(hwnd_, kDockWindowPreviewHoverTimerId);
    if (dockWindowPreview_)
        dockWindowPreview_->Hide();
    dockWindowPreviewHover_.Reset();
    dockWindowPreviewKey_.clear();
    dockWindowPreviewAnchorScreen_ = {};
}

inline void DesktopApp::DismissDockWindowPreviewUntilLeave()
{
    if (hwnd_)
        KillTimer(hwnd_, kDockWindowPreviewHoverTimerId);
    dockWindowPreviewHover_.SuppressForActivation();
    if (dockWindowPreview_)
        dockWindowPreview_->Hide();
    dockWindowPreviewKey_.clear();
    dockWindowPreviewAnchorScreen_ = {};
}

inline std::wstring DockItemWindowKey(const DesktopItem& item)
{
    return ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
}

inline void CALLBACK DesktopApp::DockForegroundWinEventProc(HWINEVENTHOOK,
    DWORD event, HWND window, LONG objectId, LONG childId, DWORD, DWORD)
{
    if (event == EVENT_SYSTEM_FOREGROUND && window)
    {
        const HWND previous = dockForegroundWindow_.exchange(window);
        if (previous != window)
        {
            dockPreviousForegroundWindow_.store(previous);
            dockForegroundChangedTick_.store(GetTickCount());
        }
    }

    if (event >= EVENT_OBJECT_CREATE &&
        (objectId != OBJID_WINDOW || childId != CHILDID_SELF))
        return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE)
    {
        if (!window || GetAncestor(window, GA_ROOT) != window)
            return;
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (!processId || processId == GetCurrentProcessId())
            return;
        wchar_t className[96]{};
        GetClassNameW(window, className,
            static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"Progman") == 0 ||
            _wcsicmp(className, L"WorkerW") == 0 ||
            _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
            _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
            return;
        const LONG_PTR exStyle =
            GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) != 0 ||
            (GetWindow(window, GW_OWNER) &&
                (exStyle & WS_EX_APPWINDOW) == 0))
            return;
        if (!IsWindowVisible(window) || IsIconic(window))
            return;

        const SystemTaskbarWindowObservation observation{
            MonitorFromWindow(window, MONITOR_DEFAULTTONULL),
            IsZoomed(window) != FALSE
        };
        std::scoped_lock lock(
            systemTaskbarWindowObservationMutex_);
        const auto found =
            systemTaskbarWindowObservations_.find(window);
        if (found !=
                systemTaskbarWindowObservations_.end() &&
            found->second.monitor == observation.monitor &&
            found->second.maximized == observation.maximized)
            return;
        systemTaskbarWindowObservations_[window] = observation;
    }
    systemTaskbarWindowStateChangedTick_.fetch_add(1,
        std::memory_order_relaxed);
    bool dockWindowListChanged =
        event == EVENT_SYSTEM_FOREGROUND ||
        event == EVENT_SYSTEM_MINIMIZESTART ||
        event == EVENT_SYSTEM_MINIMIZEEND ||
        (event >= EVENT_OBJECT_CREATE &&
            event <= EVENT_OBJECT_HIDE);
#ifdef EVENT_OBJECT_CLOAKED
    dockWindowListChanged = dockWindowListChanged ||
        event == EVENT_OBJECT_CLOAKED ||
        event == EVENT_OBJECT_UNCLOAKED;
#endif
    if (dockWindowListChanged)
        dockWindowListChangedTick_.fetch_add(
            1, std::memory_order_relaxed);
}

inline void DesktopApp::StartDockForegroundMonitor()
{
    if (dockForegroundEventHook_) return;
    const HWND foreground = GetForegroundWindow();
    dockForegroundWindow_.store(foreground);
    dockPreviousForegroundWindow_.store(nullptr);
    dockForegroundChangedTick_.store(GetTickCount());
    dockForegroundEventHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND, nullptr, &DesktopApp::DockForegroundWinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT);

    const auto addHook = [this](DWORD first, DWORD last) {
        if (HWINEVENTHOOK hook = SetWinEventHook(first, last, nullptr,
            &DesktopApp::DockForegroundWinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT))
            systemTaskbarWindowEventHooks_.push_back(hook);
    };
    addHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    addHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE);
    addHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE);
#ifdef EVENT_OBJECT_CLOAKED
    addHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);
#endif
    RestartSystemTaskbarShellVisibilityDetectors();
    systemTaskbarWindowStateChangedTick_.fetch_add(1,
        std::memory_order_relaxed);
    dockWindowListChangedTick_.fetch_add(
        1, std::memory_order_relaxed);
}

inline void DesktopApp::RestartSystemTaskbarShellVisibilityDetectors()
{
    taskbarAppVisibility_.Reset();
    taskbarAppVisibilityAttempted_ = false;
    taskbarSearchVisibility_.reset();
    taskbarSearchVisibility_ = std::make_unique<SearchVisibilityDetector>([] {
        systemTaskbarWindowStateChangedTick_.fetch_add(1,
            std::memory_order_relaxed);
    });
}

inline void DesktopApp::StopDockForegroundMonitor()
{
    if (dockForegroundEventHook_)
    {
        UnhookWinEvent(dockForegroundEventHook_);
        dockForegroundEventHook_ = nullptr;
    }
    for (HWINEVENTHOOK hook : systemTaskbarWindowEventHooks_)
        if (hook) UnhookWinEvent(hook);
    systemTaskbarWindowEventHooks_.clear();
    dockForegroundWindow_.store(nullptr);
    dockPreviousForegroundWindow_.store(nullptr);
    dockForegroundChangedTick_.store(0);
    systemTaskbarWindowStateChangedTick_.store(0);
    dockWindowListChangedTick_.store(0);
    systemTaskbarWindowStateObservedTick_ = 0;
    systemTaskbarWindowScanTick_ = 0;
    {
        std::scoped_lock lock(
            systemTaskbarWindowObservationMutex_);
        systemTaskbarWindowObservations_.clear();
    }
    dockRunningWindowsForegroundTick_ = 0;
    dockRunningWindowsStateTick_ = 0;
    dockRunningWindowsRefreshTick_ = 0;
    systemTaskbarMonitorWindowStates_.clear();
    systemTaskbarWindows_.clear();
    taskbarAppVisibility_.Reset();
    taskbarAppVisibilityAttempted_ = false;
    taskbarSearchVisibility_.reset();
}

inline bool DesktopApp::IsSystemTaskbarHookRequired(
    const DockSettings& settings) const
{
    const auto ruleNeedsHook = [](const SystemTaskbarDynamicRule& rule) {
        return rule.enabled &&
            rule.themeMode != SystemTaskbarThemeMode::Native;
    };
    return settings.systemTaskbarBackdropEnabled ||
        ruleNeedsHook(settings.systemTaskbarVisibleWindow) ||
        ruleNeedsHook(settings.systemTaskbarMaximizedWindow) ||
        ruleNeedsHook(settings.systemTaskbarShellUi);
}

inline PersonalizationSettings DesktopApp::ResolveSystemTaskbarDynamicAppearance(
    const SystemTaskbarDynamicRule& rule) const
{
    PersonalizationSettings result;
    switch (rule.themeMode)
    {
    case SystemTaskbarThemeMode::FollowGlobal:
        if (settingsWindow_)
            result = settingsWindow_->GetPersonalization();
        else
        {
            result = PersonalizationSettings::DarkPreset();
            LoadPersonalization(GetPersonalizationPath().c_str(), result);
        }
        break;
    case SystemTaskbarThemeMode::Dark:
        result = MakeAppearancePreset(kAppearancePresetDark);
        break;
    case SystemTaskbarThemeMode::Light:
        result = MakeAppearancePreset(kAppearancePresetLight);
        break;
    case SystemTaskbarThemeMode::GlassDark:
        result = MakeAppearancePreset(kAppearancePresetGlassDark);
        break;
    case SystemTaskbarThemeMode::GlassLight:
        result = MakeAppearancePreset(kAppearancePresetGlassLight);
        break;
    case SystemTaskbarThemeMode::AcrylicDark:
        result = MakeAppearancePreset(kAppearancePresetAcrylicDark);
        break;
    case SystemTaskbarThemeMode::AcrylicLight:
        result = MakeAppearancePreset(kAppearancePresetAcrylicLight);
        break;
    case SystemTaskbarThemeMode::Transparent:
        result = MakeTransparentTaskbarAppearance();
        break;
    case SystemTaskbarThemeMode::Custom:
        result = rule.appearance;
        break;
    case SystemTaskbarThemeMode::Native:
    default:
        result = PersonalizationSettings::DarkPreset();
        break;
    }
    if (rule.contentTheme >= 0)
        result.contentTheme = rule.contentTheme;
    return result;
}

inline bool IsSystemTaskbarCandidateWindow(HWND window,
    IVirtualDesktopManager* virtualDesktopManager)
{
    if (!window || GetAncestor(window, GA_ROOT) != window ||
        !IsWindowVisible(window) || IsIconic(window))
        return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!processId || processId == GetCurrentProcessId())
        return false;

    wchar_t className[96]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"Progman") == 0 ||
        _wcsicmp(className, L"WorkerW") == 0 ||
        _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
        return false;

    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 ||
        (GetWindow(window, GW_OWNER) && (exStyle & WS_EX_APPWINDOW) == 0))
        return false;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
        sizeof(cloaked))) && cloaked != 0)
        return false;

    if (virtualDesktopManager)
    {
        BOOL onCurrentDesktop = TRUE;
        if (SUCCEEDED(virtualDesktopManager->IsWindowOnCurrentVirtualDesktop(
            window, &onCurrentDesktop)) && !onCurrentDesktop)
            return false;
    }
    return true;
}

inline bool IsSystemTaskbarShellUiWindow(HWND window)
{
    if (!window) return false;
    wchar_t className[96]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"ControlCenterWindow") == 0 ||
        _wcsicmp(className, L"WindowsDashboard") == 0)
        return true;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!processId) return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
        processId);
    if (!process) return false;
    wchar_t path[MAX_PATH]{};
    DWORD pathLength = static_cast<DWORD>(std::size(path));
    const bool queried = QueryFullProcessImageNameW(process, 0, path,
        &pathLength) != FALSE;
    CloseHandle(process);
    if (!queried) return false;

    const wchar_t* fileName = wcsrchr(path, L'\\');
    fileName = fileName ? fileName + 1 : path;
    return _wcsicmp(fileName, L"SearchHost.exe") == 0 ||
        _wcsicmp(fileName, L"SearchApp.exe") == 0 ||
        _wcsicmp(fileName, L"WidgetBoard.exe") == 0 ||
        (_wcsicmp(fileName, L"ShellExperienceHost.exe") == 0 &&
            _wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0);
}

inline bool DesktopApp::RefreshSystemTaskbarWindowState()
{
    const auto previousStates = systemTaskbarMonitorWindowStates_;
    const bool previousShellUiActive =
        systemTaskbarShellUiActive_;
    const HMONITOR previousShellUiMonitor =
        systemTaskbarShellUiMonitor_;
    systemTaskbarMonitorWindowStates_.clear();

    ComPtr<IVirtualDesktopManager> virtualDesktopManager;
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&virtualDesktopManager));

    struct EnumerationContext
    {
        IVirtualDesktopManager* virtualDesktopManager = nullptr;
        std::unordered_map<HMONITOR,
            SystemTaskbarMonitorWindowState>* states = nullptr;
        std::unordered_map<HWND,
            SystemTaskbarWindowObservation>* observations = nullptr;
    } context{ virtualDesktopManager.Get(),
        &systemTaskbarMonitorWindowStates_ };
    std::unordered_map<HWND,
        SystemTaskbarWindowObservation> observations;
    context.observations = &observations;

    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        auto* context = reinterpret_cast<EnumerationContext*>(value);
        if (!IsSystemTaskbarCandidateWindow(window,
            context->virtualDesktopManager))
            return TRUE;
        const HMONITOR monitor = MonitorFromWindow(window,
            MONITOR_DEFAULTTONULL);
        if (!monitor) return TRUE;
        (*context->observations)[window] = {
            monitor, IsZoomed(window) != FALSE
        };
        auto& state = (*context->states)[monitor];
        state.visible = true;
        if (IsZoomed(window))
            state.maximized = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    {
        std::scoped_lock lock(
            systemTaskbarWindowObservationMutex_);
        systemTaskbarWindowObservations_ =
            std::move(observations);
    }

    bool launcherVisible = false;
    if (!taskbarAppVisibilityAttempted_)
    {
        taskbarAppVisibilityAttempted_ = true;
        CoCreateInstance(CLSID_AppVisibility, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&taskbarAppVisibility_));
    }
    if (taskbarAppVisibility_)
    {
        BOOL visible = FALSE;
        if (SUCCEEDED(taskbarAppVisibility_->IsLauncherVisible(&visible)))
            launcherVisible = visible != FALSE;
    }

    const HWND foreground = GetForegroundWindow();
    // ShellViewCoordinator is the preferred event source. On newer Windows
    // 11 builds it may keep reporting Hidden for Search and the system
    // sidebars, so retain their foreground identities as a narrow fallback.
    const bool shellViewVisible =
        (taskbarSearchVisibility_ &&
            taskbarSearchVisibility_->IsVisible()) ||
        IsSystemTaskbarShellUiWindow(foreground);
    systemTaskbarShellUiActive_ = launcherVisible || shellViewVisible;
    systemTaskbarShellUiMonitor_ = systemTaskbarShellUiActive_
        ? MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY)
        : nullptr;
    systemTaskbarWindowStateObservedTick_ =
        systemTaskbarWindowStateChangedTick_.load();

    const bool statesEqual =
        previousStates.size() ==
            systemTaskbarMonitorWindowStates_.size() &&
        std::all_of(previousStates.begin(), previousStates.end(),
            [this](const auto& entry) {
                const auto found =
                    systemTaskbarMonitorWindowStates_.find(
                        entry.first);
                return found !=
                        systemTaskbarMonitorWindowStates_.end() &&
                    found->second.visible ==
                        entry.second.visible &&
                    found->second.maximized ==
                        entry.second.maximized;
            });
    return !statesEqual ||
        previousShellUiActive != systemTaskbarShellUiActive_ ||
        previousShellUiMonitor != systemTaskbarShellUiMonitor_;
}

inline bool DesktopApp::RefreshSystemTaskbarAppearance(
    bool forceWindowScan, bool skipUnchangedWindowState)
{
    const bool hookRequired = IsSystemTaskbarHookRequired(dockSettings_);
    if (!hookRequired)
    {
        ApplySystemTaskbarBackdrop(false, false,
            ResolveSystemTaskbarAppearance(dockSettings_));
        systemTaskbarBackdropRefreshTick_ = GetTickCount();
        return true;
    }

    const DWORD changedTick = systemTaskbarWindowStateChangedTick_.load();
    if (forceWindowScan || changedTick != systemTaskbarWindowStateObservedTick_)
    {
        const bool windowStateChanged =
            RefreshSystemTaskbarWindowState();
        systemTaskbarWindowScanTick_ = GetTickCount();
        if (skipUnchangedWindowState &&
            !windowStateChanged)
            return false;
    }

    struct TaskbarEnumerationContext
    {
        std::vector<HWND> windows;
    } context;
    const auto appendTaskbar = [&context](HWND window) {
        if (!window || !IsWindow(window)) return;
        wchar_t className[64]{};
        GetClassNameW(window, className,
            static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"Shell_TrayWnd") != 0 &&
            _wcsicmp(className, L"Shell_SecondaryTrayWnd") != 0)
            return;
        if (std::find(context.windows.begin(), context.windows.end(),
            window) == context.windows.end())
            context.windows.push_back(window);
    };
    // Keep valid handles discovered before a shell surface temporarily
    // reparents every taskbar (Task View does this on all monitors).
    for (HWND taskbar : systemTaskbarWindows_)
        appendTaskbar(taskbar);
    // Shell surfaces temporarily reparent the primary taskbar while Start,
    // Search or Task View is open. It then disappears from EnumWindows even
    // though Shell_TrayWnd remains valid and visible. Always seed it directly.
    appendTaskbar(FindWindowW(L"Shell_TrayWnd", nullptr));
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        wchar_t className[64]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
            _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
        {
            auto& windows =
                reinterpret_cast<TaskbarEnumerationContext*>(value)->windows;
            if (std::find(windows.begin(), windows.end(), window) ==
                windows.end())
                windows.push_back(window);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    systemTaskbarWindows_ = context.windows;

    const PersonalizationSettings defaultAppearance =
        ResolveSystemTaskbarAppearance(dockSettings_);
    std::vector<SystemTaskbarTargetAppearance> targets;
    targets.reserve(context.windows.size());
    for (HWND taskbar : context.windows)
    {
        const HMONITOR monitor = MonitorFromWindow(taskbar,
            MONITOR_DEFAULTTONULL);
        const auto stateIt = systemTaskbarMonitorWindowStates_.find(monitor);
        const SystemTaskbarMonitorWindowState state =
            stateIt == systemTaskbarMonitorWindowStates_.end()
            ? SystemTaskbarMonitorWindowState{} : stateIt->second;

        const SystemTaskbarDynamicRule* selectedRule = nullptr;
        if (dockSettings_.systemTaskbarShellUi.enabled &&
            (systemTaskbarTaskViewActive_ ||
             (systemTaskbarShellUiActive_ &&
              monitor == systemTaskbarShellUiMonitor_)))
            selectedRule = &dockSettings_.systemTaskbarShellUi;
        else if (dockSettings_.systemTaskbarMaximizedWindow.enabled &&
            state.maximized)
            selectedRule = &dockSettings_.systemTaskbarMaximizedWindow;
        else if (dockSettings_.systemTaskbarVisibleWindow.enabled &&
            state.visible)
            selectedRule = &dockSettings_.systemTaskbarVisibleWindow;

        SystemTaskbarTargetAppearance target;
        target.taskbar = taskbar;
        if (selectedRule)
        {
            target.enabled =
                selectedRule->themeMode != SystemTaskbarThemeMode::Native;
            target.appearance =
                ResolveSystemTaskbarDynamicAppearance(*selectedRule);
        }
        else
        {
            target.enabled = dockSettings_.systemTaskbarBackdropEnabled;
            target.appearance = defaultAppearance;
        }
        targets.push_back(std::move(target));
    }

    ApplySystemTaskbarBackdrop(true,
        dockSettings_.systemTaskbarBackdropEnabled, defaultAppearance, targets);
    systemTaskbarBackdropRefreshTick_ = GetTickCount();
    return true;
}

inline void DesktopApp::UpdateSystemTaskbarRevealGuard()
{
    if (!generalSettings_.dockEnabled || !dockSettings_.systemTaskbarAutoHide ||
        dockSettings_.edgeAttached ||
        dockSettings_.position != DockPosition::Bottom)
        return;

    constexpr int kRevealGuardPixels = 6;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const HMONITOR cursorMonitor =
        MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO cursorMonitorInfo{};
    cursorMonitorInfo.cbSize = sizeof(cursorMonitorInfo);
    if (!cursorMonitor ||
        !GetMonitorInfoW(cursorMonitor, &cursorMonitorInfo) ||
        cursor.y < cursorMonitorInfo.rcMonitor.bottom -
            kRevealGuardPixels)
        return;

    if (!IsSystemTaskbarAutoHideEnabled())
        return;

    APPBARDATA taskbarPosition{};
    taskbarPosition.cbSize = sizeof(taskbarPosition);
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &taskbarPosition) ||
        taskbarPosition.uEdge != ABE_BOTTOM)
        return;

    if (!hwnd_) return;

    RECT screen{};
    bool foundProtectedDock = false;
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        RECT candidate = dock->GetBounds();
        POINT topLeft{ candidate.left, candidate.top };
        POINT bottomRight{ candidate.right, candidate.bottom };
        if (!ClientToScreen(hwnd_, &topLeft) ||
            !ClientToScreen(hwnd_, &bottomRight))
            continue;
        candidate = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
        const POINT center{
            (candidate.left + candidate.right) / 2,
            (candidate.top + candidate.bottom) / 2
        };
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        if (!monitor || monitor != cursorMonitor ||
            !GetMonitorInfoW(monitor, &monitorInfo))
            continue;
        if (cursor.x < candidate.left || cursor.x >= candidate.right ||
            cursor.y < candidate.top || cursor.y >= monitorInfo.rcMonitor.bottom)
            continue;
        screen = monitorInfo.rcMonitor;
        foundProtectedDock = true;
        break;
    }
    if (!foundProtectedDock) return;

    const int guardedEdgeTop = screen.bottom - kRevealGuardPixels;
    if (cursor.y >= guardedEdgeTop)
        SetCursorPos(cursor.x, guardedEdgeTop - 1);
}

inline void DesktopApp::ToggleWindowsStartMenu()
{
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_LWIN;
    input[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    input[1] = input[0];
    input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT));
}

inline size_t DesktopApp::FindWidgetIndexById(const std::wstring& id) const
{
    for (size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].id == id) return i;
    return static_cast<size_t>(-1);
}

inline bool DesktopApp::IsDockExclusiveItemKey(const std::wstring& key) const
{
    const std::wstring upper = ToUpperInvariant(key);
    return std::any_of(dockEntries_.begin(), dockEntries_.end(), [&](const DockEntry& entry) {
        return entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop &&
            ToUpperInvariant(entry.reference) == upper;
    });
}

inline bool DesktopApp::IsDockExclusiveWidgetId(const std::wstring& id) const
{
    return std::any_of(dockEntries_.begin(), dockEntries_.end(), [&](const DockEntry& entry) {
        return (entry.type == DockEntryType::Collection ||
                entry.type == DockEntryType::FolderMapping) &&
            entry.reference == id;
    });
}

inline snowdesktop::item_location::FolderTarget
DesktopApp::ResolveDockFolderTarget(const DockEntry& entry) const
{
    std::wstring sourcePath;
    if (entry.type == DockEntryType::FolderMapping)
    {
        std::wstring cacheKey =
            L"M:" + ToUpperInvariant(entry.reference);
        if (const auto cached = dockFolderTargetCache_.find(cacheKey);
            cached != dockFolderTargetCache_.end())
            return cached->second;

        const size_t widgetIndex = FindWidgetIndexById(entry.reference);
        if (widgetIndex >= widgets_.size() ||
            widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping)
            return {};
        sourcePath = widgets_[widgetIndex].sourceFolderPath;
        auto target =
            snowdesktop::item_location::ResolveFolderTarget(
                sourcePath);
        if (target.kind ==
                snowdesktop::item_location::
                    FolderTargetKind::None &&
            !sourcePath.empty())
        {
            target.path = sourcePath;
            target.kind =
                snowdesktop::item_location::
                    FolderTargetKind::Directory;
            target.available = false;
        }
        dockFolderTargetCache_.insert_or_assign(
            std::move(cacheKey), target);
        return target;
    }
    if (entry.type != DockEntryType::DesktopItem ||
        IsRecycleBinDockEntry(entry))
        return {};

    std::wstring cacheKey =
        L"I:" + ToUpperInvariant(entry.reference);
    if (const auto cached = dockFolderTargetCache_.find(cacheKey);
        cached != dockFolderTargetCache_.end())
        return cached->second;

    const size_t itemIndex = FindItemIndexByKey(entry.reference);
    const std::wstring& path = itemIndex < items_.size() &&
            !items_[itemIndex].parsingName.empty()
        ? items_[itemIndex].parsingName
        : entry.reference;
    auto target =
        snowdesktop::item_location::ResolveFolderTarget(path);
    dockFolderTargetCache_.insert_or_assign(
        std::move(cacheKey), target);
    return target;
}

inline bool DesktopApp::IsFolderDockEntry(const DockEntry& entry) const
{
    return entry.type == DockEntryType::FolderMapping ||
        ResolveDockFolderTarget(entry).kind !=
            snowdesktop::item_location::FolderTargetKind::None;
}

inline size_t DesktopApp::DockMainEntryCount() const
{
    return static_cast<size_t>(std::count_if(
        dockEntries_.begin(), dockEntries_.end(),
        [this](const DockEntry& entry) {
            return !IsRecycleBinDockEntry(entry) &&
                !IsFolderDockEntry(entry);
        }));
}

inline size_t DesktopApp::DockFolderEntryCount() const
{
    return static_cast<size_t>(std::count_if(
        dockEntries_.begin(), dockEntries_.end(),
        [this](const DockEntry& entry) {
            return !IsRecycleBinDockEntry(entry) &&
                IsFolderDockEntry(entry);
        }));
}

inline size_t DesktopApp::FindCollectionGroupIndexForChild(
    const std::wstring& childId) const
{
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const DesktopWidget& widget = widgets_[i];
        if (widget.type != DesktopWidgetType::CollectionGroup) continue;
        if (std::find(widget.childWidgetIds.begin(),
            widget.childWidgetIds.end(), childId) !=
            widget.childWidgetIds.end())
            return i;
    }
    return static_cast<size_t>(-1);
}

inline bool DesktopApp::IsGroupedCollection(
    const DesktopWidget& widget) const
{
    return widget.type == DesktopWidgetType::Collection &&
        FindCollectionGroupIndexForChild(widget.id) < widgets_.size();
}

inline size_t DesktopApp::FindFileGroupIndexForChild(
    const std::wstring& childId) const
{
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const DesktopWidget& widget = widgets_[i];
        if (widget.type != DesktopWidgetType::FileGroup) continue;
        if (std::find(widget.childWidgetIds.begin(),
                widget.childWidgetIds.end(), childId) !=
            widget.childWidgetIds.end())
            return i;
    }
    return static_cast<size_t>(-1);
}

inline bool DesktopApp::IsGroupedWidget(
    const DesktopWidget& widget) const
{
    if (IsGroupedCollection(widget)) return true;
    return (widget.type == DesktopWidgetType::FileCategories ||
            widget.type == DesktopWidgetType::FolderMapping) &&
        FindFileGroupIndexForChild(widget.id) < widgets_.size();
}

inline bool DesktopApp::IsRecycleBinDockEntry(const DockEntry& entry) const
{
    return entry.type == DockEntryType::DesktopItem &&
        _wcsicmp(entry.reference.c_str(), kDesktopIconClsidRecycleBin) == 0;
}

inline void DesktopApp::NormalizeDockRecycleBinPosition()
{
    snowdesktop::dock_folder_rules::StableNormalize(
        dockEntries_,
        [this](const DockEntry& entry) {
            if (IsRecycleBinDockEntry(entry))
                return snowdesktop::dock_folder_rules::
                    EntryGroup::Recycle;
            return IsFolderDockEntry(entry)
                ? snowdesktop::dock_folder_rules::
                    EntryGroup::Folder
                : snowdesktop::dock_folder_rules::
                    EntryGroup::Main;
        });
}

inline void DesktopApp::LoadDockUsageStats()
{
    dockUsageStats_.clear();
    std::ifstream file(GetDataFilePath(L"SnowDesktop.dock-usage.json"), std::ios::binary);
    if (!file) return;
    std::ostringstream stream;
    stream << file.rdbuf();
    JsonValue root;
    if (!ParseJson(stream.str(), root) || !root.IsObject()) return;
    const JsonValue* entries = root.Find("entries");
    if (!entries || !entries->IsArray()) return;

    auto readInteger = [](const JsonValue& object,
        std::string_view name, int& output)
    {
        const JsonValue* value = object.Find(name);
        if (!value || !value->IsNumber() ||
            !std::isfinite(value->number) ||
            std::trunc(value->number) != value->number ||
            value->number < std::numeric_limits<int>::min() ||
            value->number > std::numeric_limits<int>::max())
        {
            return false;
        }
        output = static_cast<int>(value->number);
        return true;
    };

    for (const JsonValue& entry : entries->array)
    {
        if (!entry.IsObject()) continue;
        const JsonValue* key = entry.Find("key");
        int launchCount = 0;
        int lastUsed = 0;
        if (key && key->IsString() &&
            readInteger(entry, "launchCount", launchCount) &&
            launchCount > 0)
        {
            readInteger(entry, "lastUsed", lastUsed);
            const std::wstring normalizedKey =
                ToUpperInvariant(Utf8ToWide(key->string));
            if (!normalizedKey.empty())
                dockUsageStats_[normalizedKey] =
                    { launchCount, std::max(0, lastUsed) };
        }
    }
}

inline void DesktopApp::SaveDockUsageStats() const
{
    std::ofstream file(GetDataFilePath(L"SnowDesktop.dock-usage.json"),
        std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << "{\n  \"entries\": [\n";
    size_t written = 0;
    for (const auto& [key, record] : dockUsageStats_)
    {
        if (key.empty() || record.launchCount <= 0) continue;
        const size_t itemIndex = FindItemIndexByKey(key);
        if (itemIndex >= items_.size() || !IsDockUsageEligibleItem(items_[itemIndex]))
            continue;
        if (written++ > 0) file << ",\n";
        file << "    { \"key\": \"" << JsonEscapeUtf8(key)
             << "\", \"launchCount\": " << record.launchCount
             << ", \"lastUsed\": " << record.lastUsed << " }";
    }
    file << "\n  ]\n}\n";
}

inline bool DesktopApp::IsDockUsageEligibleItem(const DesktopItem& item) const
{
    if (!item.desktopIconClsid.empty() || item.parsingName.empty())
        return false;
    const wchar_t* extension = PathFindExtensionW(item.parsingName.c_str());
    return extension &&
        (_wcsicmp(extension, L".lnk") == 0 || _wcsicmp(extension, L".url") == 0);
}

inline bool DesktopApp::RemoveDockDragOutItems(const std::vector<Item*>& sourceItems)
{
    bool usageChanged = false;
    std::vector<size_t> mappedEntryIndices;
    for (Item* source : sourceItems)
    {
        if (const auto* frequentItem = dynamic_cast<DockFrequentItem*>(source))
        {
            if (frequentItem->GetItemIndex() >= items_.size()) continue;
            const DesktopItem& item = items_[frequentItem->GetItemIndex()];
            const std::wstring key = ToUpperInvariant(
                item.layoutKey.empty() ? item.parsingName : item.layoutKey);
            if (!key.empty())
                usageChanged = dockUsageStats_.erase(key) > 0 || usageChanged;
            continue;
        }
        if (const auto* dockItem = dynamic_cast<DockEntryItem*>(source))
        {
            const size_t index = dockItem->GetEntryIndex();
            if (index < dockEntries_.size() && dockEntries_[index].keepOnDesktop)
                mappedEntryIndices.push_back(index);
        }
    }

    std::sort(mappedEntryIndices.begin(), mappedEntryIndices.end());
    mappedEntryIndices.erase(
        std::unique(mappedEntryIndices.begin(), mappedEntryIndices.end()),
        mappedEntryIndices.end());
    for (auto it = mappedEntryIndices.rbegin(); it != mappedEntryIndices.rend(); ++it)
        dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));

    if (usageChanged) SaveDockUsageStats();
    if (!mappedEntryIndices.empty())
    {
        NormalizeDockRecycleBinPosition();
        RefreshCollectedKeysCache();
    }
    if (!usageChanged && mappedEntryIndices.empty()) return false;
    InvalidateDockContainers();
    InvalidateDragStaticScene();
    return true;
}

inline bool DesktopApp::RemoveDockMappingAt(
    size_t entryIndex)
{
    if (!snowdesktop::
            desktop_item_reference_migration::
                RemoveDockMappingAt(
                    dockEntries_, entryIndex))
    {
        return false;
    }

    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    ClearSelection();
    SaveLayoutSlots();
    RebuildContainersAndItems();
    LayoutItems();
    InvalidateDragStaticScene();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

inline void DesktopApp::RecordDockItemUsage(size_t itemIndex)
{
    if (itemIndex >= items_.size()) return;
    const DesktopItem& item = items_[itemIndex];
    if (!IsDockUsageEligibleItem(item)) return;
    const std::wstring key = ToUpperInvariant(
        item.layoutKey.empty() ? item.parsingName : item.layoutKey);
    if (key.empty()) return;

    DockUsageRecord& record = dockUsageStats_[key];
    record.launchCount = std::min(record.launchCount + 1, std::numeric_limits<int>::max());
    const std::time_t now = std::time(nullptr);
    record.lastUsed = now > 0
        ? static_cast<int>(std::min<std::time_t>(now, std::numeric_limits<int>::max()))
        : record.lastUsed;
    SaveDockUsageStats();

    if (dockSettings_.showFrequentItems)
    {
        InvalidateDockContainers();
        InvalidateDragStaticScene();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

inline std::vector<size_t> DesktopApp::GetFrequentDockItemIndices()
{
    std::vector<size_t> result;
    if (!dockSettings_.showFrequentItems || dockSettings_.frequentItemCount <= 0)
        return result;

    std::unordered_set<std::wstring> fixedKeys;
    for (const DockEntry& entry : dockEntries_)
        if (entry.type == DockEntryType::DesktopItem)
            fixedKeys.insert(ToUpperInvariant(entry.reference));

    struct Candidate
    {
        size_t itemIndex = static_cast<size_t>(-1);
        DockUsageRecord usage;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(dockUsageStats_.size());
    for (const auto& [key, usage] : dockUsageStats_)
    {
        if (usage.launchCount <= 0 || fixedKeys.contains(key)) continue;
        const size_t itemIndex = FindItemIndexByKey(key);
        if (itemIndex >= items_.size() || !IsDockUsageEligibleItem(items_[itemIndex]) ||
            _wcsicmp(items_[itemIndex].desktopIconClsid.c_str(),
                kDesktopIconClsidRecycleBin) == 0)
            continue;
        candidates.push_back({ itemIndex, usage });
    }

    std::stable_sort(candidates.begin(), candidates.end(),
        [this](const Candidate& a, const Candidate& b) {
            if (a.usage.launchCount != b.usage.launchCount)
                return a.usage.launchCount > b.usage.launchCount;
            if (a.usage.lastUsed != b.usage.lastUsed)
                return a.usage.lastUsed > b.usage.lastUsed;
            return ToUpperInvariant(items_[a.itemIndex].name) <
                ToUpperInvariant(items_[b.itemIndex].name);
        });

    const size_t limit = static_cast<size_t>(
        std::clamp(dockSettings_.frequentItemCount, 1, 8));
    for (const Candidate& candidate : candidates)
    {
        const DockAppIdentity identity = ResolveDockAppIdentity(candidate.itemIndex);
        const bool isShownAsRunning =
            std::any_of(dockUnpinnedRunningApps_.begin(),
            dockUnpinnedRunningApps_.end(), [&](const DockRunningAppInfo& running) {
                switch (identity.kind)
                {
                case DockAppIdentityKind::Executable:
                    return !identity.executablePath.empty() &&
                        identity.executablePath == running.executablePath;
                case DockAppIdentityKind::Applications:
                    return !identity.appUserModelId.empty() &&
                        identity.appUserModelId == running.appUserModelId;
                case DockAppIdentityKind::Steam:
                    return (!identity.appUserModelId.empty() &&
                            identity.appUserModelId == running.appUserModelId) ||
                        IsDockPathInsideDirectory(running.executablePath,
                            identity.steamInstallDirectory);
                default:
                    return false;
                }
            });
        if (isShownAsRunning) continue;
        result.push_back(candidate.itemIndex);
        if (result.size() >= limit) break;
    }
    return result;
}

inline bool DesktopApp::StartDockLaunchBounce(size_t itemIndex)
{
    if (!hwnd_ || !IsWindow(hwnd_) ||
        itemIndex >= items_.size() ||
        !generalSettings_.dockEnabled ||
        !snowdesktop::dock_launch_animation::
            SystemAnimationsEnabled())
        return false;

    const std::wstring key =
        DockItemWindowKey(items_[itemIndex]);
    if (key.empty())
        return false;

    const bool fixed =
        std::any_of(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& entry) {
                return entry.type ==
                        DockEntryType::DesktopItem &&
                    ToUpperInvariant(entry.reference) == key;
            });
    bool frequent = false;
    if (!fixed)
    {
        const std::vector<size_t> frequentItems =
            GetFrequentDockItemIndices();
        frequent = std::find(
            frequentItems.begin(), frequentItems.end(),
            itemIndex) != frequentItems.end();
    }
    if (!fixed && !frequent)
        return false;

    dockLaunchBounces_[key] = {
        snowdesktop::dock_launch_animation::
            MonotonicTimeMilliseconds(),
        false,
        nullptr
    };
    if (!SetCoalescableTimer(
            hwnd_, kDockLaunchBounceTimerId,
            snowdesktop::dock_launch_animation::
                kFrameIntervalMs, nullptr,
            TIMERV_NO_COALESCING))
    {
        dockLaunchBounces_.erase(key);
        return false;
    }
    InvalidateDockRects(FALSE);
    return true;
}

inline float DesktopApp::GetDockLaunchBounceOffset(
    size_t itemIndex, int iconSize) const
{
    if (itemIndex >= items_.size())
        return 0.0f;
    const auto found = dockLaunchBounces_.find(
        DockItemWindowKey(items_[itemIndex]));
    if (found == dockLaunchBounces_.end())
        return 0.0f;
    return snowdesktop::dock_launch_animation::
        OffsetPixels(
            snowdesktop::dock_launch_animation::
                MonotonicTimeMilliseconds() -
                found->second.startTimeMs,
            iconSize);
}

inline void DesktopApp::OnDockLaunchBounceTimer()
{
    if (dockLaunchBounces_.empty())
    {
        if (hwnd_)
            KillTimer(hwnd_, kDockLaunchBounceTimerId);
        return;
    }

    const double now =
        snowdesktop::dock_launch_animation::
            MonotonicTimeMilliseconds();

    for (auto bounce = dockLaunchBounces_.begin();
        bounce != dockLaunchBounces_.end();)
    {
        const size_t itemIndex =
            FindItemIndexByKey(bounce->first);
        const double elapsed =
            now - bounce->second.startTimeMs;
        if (itemIndex >= items_.size() ||
            elapsed >=
                static_cast<double>(
                    snowdesktop::dock_launch_animation::
                        kMaximumDurationMs))
        {
            bounce = dockLaunchBounces_.erase(bounce);
            continue;
        }

        if (elapsed >=
                static_cast<double>(
                    snowdesktop::dock_launch_animation::
                        kMinimumDurationMs))
        {
            const bool knownRunning =
                GetDockWindowVisualState(itemIndex) !=
                    DockWindowVisualState::Closed;
            HWND foreground = GetAncestor(
                GetForegroundWindow(), GA_ROOT);
            bool launchedWindowIsForeground = false;
            if (!knownRunning &&
                foreground &&
                foreground !=
                    bounce->second.observedForeground)
            {
                bounce->second.observedForeground =
                    foreground;
                launchedWindowIsForeground =
                    DockWindowMatchesAppIdentity(
                        foreground,
                        ResolveDockAppIdentity(
                            itemIndex));
            }
            if (knownRunning ||
                launchedWindowIsForeground)
            {
                bounce->second.stopRequested = true;
            }
        }
        if (bounce->second.stopRequested &&
            snowdesktop::dock_launch_animation::
                IsRestingPoint(elapsed))
        {
            bounce = dockLaunchBounces_.erase(bounce);
            continue;
        }
        ++bounce;
    }

    InvalidateDockRects(FALSE);
    if (dockLaunchBounces_.empty() && hwnd_)
        KillTimer(hwnd_, kDockLaunchBounceTimerId);
}

inline bool DesktopApp::LaunchDesktopItem(
    size_t itemIndex, bool animateDockLaunch)
{
    if (itemIndex >= items_.size() || items_[itemIndex].parsingName.empty())
        return false;
    if (animateDockLaunch)
    {
        const DockAppIdentity identity =
            ResolveDockAppIdentity(itemIndex);
        if (snowdesktop::dock_window_rules::
                ShouldSuppressDockWindowCommand(
                    IsDockAppClosePending(identity)))
            return false;
    }
    if (dockWindowTransition_ &&
        dockWindowTransition_->IsActive())
    {
        dockWindowTransition_->Cancel();
    }
    if (animateDockLaunch)
        DismissDockWindowPreviewUntilLeave();
    const bool wasClosed =
        GetDockWindowVisualState(itemIndex) ==
            DockWindowVisualState::Closed;
    HINSTANCE result = ShellExecuteW(hwnd_, L"open", items_[itemIndex].parsingName.c_str(),
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
        return false;
    RecordDockItemUsage(itemIndex);
    if (animateDockLaunch && wasClosed)
        StartDockLaunchBounce(itemIndex);
    return true;
}

inline DockAppIdentity DesktopApp::ResolveDockAppIdentity(size_t itemIndex)
{
    if (itemIndex >= items_.size()) return {};
    const DesktopItem& item = items_[itemIndex];
    const std::wstring key = DockItemWindowKey(item);
    if (key.empty() || item.parsingName.empty() || !item.desktopIconClsid.empty())
        return {};

    if (const auto cached = dockAppIdentityCache_.find(key);
        cached != dockAppIdentityCache_.end() &&
        cached->second.sourceParsingName == item.parsingName)
        return cached->second;

    DockAppIdentity identity;
    identity.sourceParsingName = item.parsingName;
    const wchar_t* extension = PathFindExtensionW(item.parsingName.c_str());
    if (extension && _wcsicmp(extension, L".exe") == 0)
    {
        identity.kind = DockAppIdentityKind::Executable;
        identity.executablePath = NormalizeDockExecutablePath(item.parsingName);
    }
    else if (extension && _wcsicmp(extension, L".url") == 0)
    {
        identity.steamAppId = ParseDockSteamAppId(item.parsingName);
        if (!identity.steamAppId.empty())
        {
            identity.kind = DockAppIdentityKind::Steam;
            identity.appUserModelId = L"STEAM://RUNGAMEID/" + identity.steamAppId;
            identity.steamInstallDirectory =
                FindDockSteamAppInstallDirectory(identity.steamAppId);
        }
    }
    else if (extension && _wcsicmp(extension, L".lnk") == 0)
    {
        ComPtr<IShellLinkW> shellLink;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&shellLink))) && shellLink)
        {
            ComPtr<IPersistFile> persistFile;
            if (SUCCEEDED(shellLink.As(&persistFile)) &&
                SUCCEEDED(persistFile->Load(item.parsingName.c_str(), STGM_READ)))
            {
                wchar_t target[32768]{};
                if (SUCCEEDED(shellLink->GetPath(target,
                        static_cast<int>(std::size(target)), nullptr, 0)) && target[0])
                {
                    const wchar_t* targetExtension = PathFindExtensionW(target);
                    if (targetExtension && _wcsicmp(targetExtension, L".exe") == 0)
                    {
                        identity.kind = DockAppIdentityKind::Executable;
                        identity.executablePath = NormalizeDockExecutablePath(target);
                    }
                }

                // 普通 EXE 快捷方式使用独立的进程路径匹配，不读取 AUMID。
                // 只有无法解析出 EXE 的虚拟 Applications 项才进入下方分支。
                if (identity.kind != DockAppIdentityKind::Executable)
                {
                    ComPtr<IPropertyStore> propertyStore;
                    std::wstring targetParsingPath;
                    if (SUCCEEDED(shellLink.As(&propertyStore)))
                    {
                        identity.appUserModelId = ReadDockAppUserModelId(propertyStore.Get());
                        targetParsingPath = ReadDockStringProperty(
                            propertyStore.Get(), PKEY_Link_TargetParsingPath);
                    }
                    if (identity.appUserModelId.empty())
                        identity.appUserModelId = ToUpperInvariant(ReadDockShellItemStringProperty(
                            item.parsingName, PKEY_AppUserModel_ID));
                    if (targetParsingPath.empty())
                        targetParsingPath = ReadDockShellItemStringProperty(
                            item.parsingName, PKEY_Link_TargetParsingPath);

                    const bool looksLikeAppUserModelId = !targetParsingPath.empty() &&
                        targetParsingPath.find(L'\\') == std::wstring::npos &&
                        targetParsingPath.find(L'/') == std::wstring::npos &&
                        targetParsingPath.find(L':') == std::wstring::npos;
                    if (!target[0] && looksLikeAppUserModelId)
                    {
                        identity.kind = DockAppIdentityKind::Applications;
                        identity.appUserModelId = ToUpperInvariant(targetParsingPath);
                    }
                }

                if (identity.kind == DockAppIdentityKind::None)
                {
                    PIDLIST_ABSOLUTE targetPidl = nullptr;
                    if (SUCCEEDED(shellLink->GetIDList(&targetPidl)) && targetPidl)
                    {
                        PWSTR parsingName = nullptr;
                        if (SUCCEEDED(SHGetNameFromIDList(targetPidl,
                                SIGDN_DESKTOPABSOLUTEPARSING, &parsingName)) && parsingName)
                        {
                            const std::wstring targetName(parsingName);
                            const std::wstring upper = ToUpperInvariant(targetName);
                            const size_t appsFolder = upper.find(L"APPSFOLDER\\");
                            if (appsFolder != std::wstring::npos)
                            {
                                identity.kind = DockAppIdentityKind::Applications;
                                identity.appUserModelId = ToUpperInvariant(targetName.substr(
                                    appsFolder + std::wstring(L"APPSFOLDER\\").size()));
                            }
                            else if (IsApplicationsShellLinkTarget(shellLink.Get()))
                            {
                                identity.kind = DockAppIdentityKind::Applications;
                                identity.appUserModelId = upper;
                            }
                        }
                        if (parsingName) CoTaskMemFree(parsingName);
                        CoTaskMemFree(targetPidl);
                    }
                }
            }
        }
    }

    dockAppIdentityCache_[key] = identity;
    return identity;
}

inline DockWindowVisualState DesktopApp::GetDockWindowVisualState(size_t itemIndex) const
{
    if (itemIndex >= items_.size()) return DockWindowVisualState::Closed;
    const auto found = dockRunningWindows_.find(DockItemWindowKey(items_[itemIndex]));
    if (found == dockRunningWindows_.end() || !found->second.running)
        return DockWindowVisualState::Closed;
    if (found->second.window && IsWindow(found->second.window))
    {
        if (IsIconic(found->second.window))
            return DockWindowVisualState::Minimized;
        // A click on a non-maximized window can move foreground ownership to
        // the desktop/Dock before this handler runs. Keep using the indicator
        // state captured by the window refresh instead of reclassifying that
        // click as Activate.
        if (found->second.foreground)
            return DockWindowVisualState::Foreground;
    }
    return DockWindowVisualState::Running;
}

inline void DesktopApp::RefreshDockRunningWindows(
    bool invalidateChanged, HWND preferredWindow)
{
    PruneDockPendingCloseWindows();
    struct DockWindowTarget
    {
        std::wstring key;
        DockAppIdentity identity;
        DockWindowInfo best;
        int score = -1;
    };
    struct RunningWindowCandidate
    {
        std::wstring identityKey;
        std::wstring title;
        std::wstring executablePath;
        std::wstring appUserModelId;
        HWND window = nullptr;
        bool minimized = false;
        bool foreground = false;
        int score = -1;
    };

    std::unordered_set<size_t> itemIndices;
    for (const DockEntry& entry : dockEntries_)
    {
        if (entry.type != DockEntryType::DesktopItem) continue;
        const size_t itemIndex = FindItemIndexByKey(entry.reference);
        if (itemIndex < items_.size()) itemIndices.insert(itemIndex);
    }
    for (const size_t itemIndex : GetFrequentDockItemIndices())
        if (itemIndex < items_.size()) itemIndices.insert(itemIndex);

    std::vector<DockWindowTarget> targets;
    targets.reserve(itemIndices.size());
    for (const size_t itemIndex : itemIndices)
    {
        DockAppIdentity identity = ResolveDockAppIdentity(itemIndex);
        if (identity.kind == DockAppIdentityKind::None)
            continue;
        targets.push_back({ DockItemWindowKey(items_[itemIndex]), std::move(identity) });
    }

    std::vector<DockAppIdentity> fixedIdentities;
    for (const DockEntry& entry : dockEntries_)
    {
        if (entry.type != DockEntryType::DesktopItem) continue;
        const size_t itemIndex = FindItemIndexByKey(entry.reference);
        if (itemIndex >= items_.size()) continue;
        DockAppIdentity identity = ResolveDockAppIdentity(itemIndex);
        if (identity.kind != DockAppIdentityKind::None)
            fixedIdentities.push_back(std::move(identity));
    }
    std::vector<RunningWindowCandidate> runningCandidates;
    std::unordered_map<std::wstring, size_t> runningCandidateIndices;

    const HWND preferredRoot = preferredWindow && IsWindow(preferredWindow)
        ? GetAncestor(preferredWindow, GA_ROOT) : nullptr;
    const HWND actualForeground = GetAncestor(GetForegroundWindow(), GA_ROOT);
    const HWND scoringForeground = preferredRoot ? preferredRoot : actualForeground;
    struct EnumContext
    {
        std::vector<DockWindowTarget>* targets;
        HWND scoringForeground;
        HWND actualForeground;
        const std::vector<DockAppIdentity>* fixedIdentities;
        std::vector<RunningWindowCandidate>* runningCandidates;
        std::unordered_map<std::wstring, size_t>* runningCandidateIndices;
        const std::unordered_map<HWND, ULONGLONG>*
            pendingCloseWindows;
        std::unordered_map<DWORD, std::wstring> processPaths;
    } context{ &targets, scoringForeground, actualForeground, &fixedIdentities,
        &runningCandidates, &runningCandidateIndices,
        &dockPendingCloseWindows_ };

    if (generalSettings_.dockEnabled)
    {
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto* context = reinterpret_cast<EnumContext*>(parameter);
            if (!IsDockTaskWindow(window) ||
                (context->pendingCloseWindows &&
                 context->pendingCloseWindows->contains(window)))
                return TRUE;

            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (!processId || processId == GetCurrentProcessId()) return TRUE;
            auto [pathIt, inserted] = context->processPaths.try_emplace(processId);
            if (inserted)
                pathIt->second = QueryDockWindowExecutablePath(window);
            const std::wstring appUserModelId = QueryDockWindowAppUserModelId(window);

            DWORD cloaked = 0;
            const bool isCloaked = SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED,
                &cloaked, sizeof(cloaked))) && cloaked != 0;
            int score = DockWindowsShareActivationGroup(
                window, context->scoringForeground) ? 1000 : 0;
            if (!isCloaked) score += 100;
            if (!IsIconic(window)) score += 20;
            if (!GetWindow(window, GW_OWNER)) score += 10;

            for (DockWindowTarget& target : *context->targets)
            {
                const bool executableMatches = !target.identity.executablePath.empty() &&
                    pathIt->second == target.identity.executablePath;
                const bool appIdMatches = !target.identity.appUserModelId.empty() &&
                    appUserModelId == target.identity.appUserModelId;
                const bool steamPathMatches =
                    target.identity.kind == DockAppIdentityKind::Steam &&
                    IsDockPathInsideDirectory(pathIt->second,
                        target.identity.steamInstallDirectory);
                bool identityMatches = false;
                switch (target.identity.kind)
                {
                case DockAppIdentityKind::Executable:
                    identityMatches = executableMatches;
                    break;
                case DockAppIdentityKind::Applications:
                    identityMatches = appIdMatches;
                    break;
                case DockAppIdentityKind::Steam:
                    identityMatches = appIdMatches || steamPathMatches;
                    break;
                default:
                    break;
                }
                if (!identityMatches || score <= target.score)
                    continue;
                target.best = { window, IsIconic(window) != FALSE, true,
                    DockWindowsShareActivationGroup(window, context->actualForeground) };
                target.score = score;
            }

            if (isCloaked || pathIt->second.empty()) return TRUE;
            bool fixed = false;
            for (const DockAppIdentity& identity : *context->fixedIdentities)
            {
                const bool executableMatches = !identity.executablePath.empty() &&
                    pathIt->second == identity.executablePath;
                const bool appIdMatches = !identity.appUserModelId.empty() &&
                    appUserModelId == identity.appUserModelId;
                const bool steamPathMatches = identity.kind == DockAppIdentityKind::Steam &&
                    IsDockPathInsideDirectory(pathIt->second,
                        identity.steamInstallDirectory);
                fixed = identity.kind == DockAppIdentityKind::Executable
                    ? executableMatches
                    : (identity.kind == DockAppIdentityKind::Applications
                        ? appIdMatches
                        : (identity.kind == DockAppIdentityKind::Steam &&
                            (appIdMatches || steamPathMatches)));
                if (fixed) break;
            }
            if (fixed) return TRUE;

            const std::wstring identityKey = !appUserModelId.empty()
                ? L"AUMID:" + appUserModelId : L"EXE:" + pathIt->second;
            wchar_t titleBuffer[512]{};
            GetWindowTextW(window, titleBuffer, static_cast<int>(std::size(titleBuffer)));
            std::wstring title = titleBuffer;
            if (title.empty())
                title = PathFindFileNameW(pathIt->second.c_str());

            auto [candidateIt, candidateInserted] =
                context->runningCandidateIndices->try_emplace(
                    identityKey, context->runningCandidates->size());
            if (candidateInserted)
            {
                context->runningCandidates->push_back({ identityKey, std::move(title),
                    pathIt->second, appUserModelId, window, IsIconic(window) != FALSE,
                    DockWindowsShareActivationGroup(window, context->actualForeground), score });
            }
            else
            {
                RunningWindowCandidate& candidate =
                    (*context->runningCandidates)[candidateIt->second];
                if (score > candidate.score)
                {
                    candidate.title = std::move(title);
                    candidate.window = window;
                    candidate.minimized = IsIconic(window) != FALSE;
                    candidate.foreground = DockWindowsShareActivationGroup(
                        window, context->actualForeground);
                    candidate.score = score;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
    }

    std::unordered_map<std::wstring, DockWindowInfo> updated;
    for (const DockWindowTarget& target : targets)
    {
        if (target.best.window)
            updated[target.key] = target.best;
        else if (target.identity.kind == DockAppIdentityKind::Steam &&
            IsDockSteamAppRunning(target.identity.steamAppId))
            updated[target.key] = { nullptr, false, true, false };
    }

    bool changed = updated.size() != dockRunningWindows_.size();
    if (!changed)
    {
        for (const auto& [key, state] : updated)
        {
            const auto old = dockRunningWindows_.find(key);
            if (old == dockRunningWindows_.end() || old->second.window != state.window ||
                old->second.minimized != state.minimized ||
                old->second.running != state.running ||
                old->second.foreground != state.foreground)
            {
                changed = true;
                break;
            }
        }
    }
    dockRunningWindows_ = std::move(updated);

    // EnumWindows does not promise a stable order. Keep surviving applications
    // in their existing Dock positions and append only genuinely new ones.
    if (runningCandidates.size() > 1 && !dockUnpinnedRunningApps_.empty())
    {
        std::vector<RunningWindowCandidate> stableCandidates;
        stableCandidates.reserve(runningCandidates.size());
        std::vector<bool> consumed(runningCandidates.size(), false);
        for (const DockRunningAppInfo& existing : dockUnpinnedRunningApps_)
        {
            const auto found = runningCandidateIndices.find(existing.identityKey);
            if (found == runningCandidateIndices.end() ||
                found->second >= runningCandidates.size() || consumed[found->second])
                continue;
            consumed[found->second] = true;
            stableCandidates.push_back(std::move(runningCandidates[found->second]));
        }
        for (size_t i = 0; i < runningCandidates.size(); ++i)
        {
            if (!consumed[i])
                stableCandidates.push_back(std::move(runningCandidates[i]));
        }
        runningCandidates = std::move(stableCandidates);
    }

    std::vector<DockRunningAppInfo> runningApps;
    runningApps.reserve(runningCandidates.size());
    std::vector<bool> reused(dockUnpinnedRunningApps_.size(), false);
    for (RunningWindowCandidate& candidate : runningCandidates)
    {
        DockRunningAppInfo info;
        info.identityKey = std::move(candidate.identityKey);
        info.title = std::move(candidate.title);
        info.executablePath = std::move(candidate.executablePath);
        info.appUserModelId = std::move(candidate.appUserModelId);
        info.window = candidate.window;
        info.minimized = candidate.minimized;
        info.foreground = candidate.foreground;
        for (size_t i = 0; i < dockUnpinnedRunningApps_.size(); ++i)
        {
            DockRunningAppInfo& old = dockUnpinnedRunningApps_[i];
            if (reused[i] || old.identityKey != info.identityKey) continue;
            info.iconBitmap = old.iconBitmap;
            info.iconBitmapSize = old.iconBitmapSize;
            info.selected = old.selected;
            old.iconBitmap = nullptr;
            reused[i] = true;
            break;
        }
        if (!info.iconBitmap)
            info.iconBitmap = CreateDockWindowIconBitmap(
                info.window, info.executablePath, info.appUserModelId,
                info.iconBitmapSize);
        runningApps.push_back(std::move(info));
    }

    bool runningLayoutChanged = runningApps.size() != dockUnpinnedRunningApps_.size();
    bool runningVisualChanged = runningLayoutChanged;
    if (!runningLayoutChanged)
    {
        for (size_t i = 0; i < runningApps.size(); ++i)
        {
            const DockRunningAppInfo& old = dockUnpinnedRunningApps_[i];
            const DockRunningAppInfo& current = runningApps[i];
            if (old.identityKey != current.identityKey)
                runningLayoutChanged = true;
            if (old.window != current.window || old.minimized != current.minimized ||
                old.foreground != current.foreground || old.title != current.title)
                runningVisualChanged = true;
        }
    }
    for (DockRunningAppInfo& old : dockUnpinnedRunningApps_)
    {
        if (!old.iconBitmap) continue;
        EraseD2DIconCacheForBitmap(old.iconBitmap);
        DeleteObject(old.iconBitmap);
        old.iconBitmap = nullptr;
    }
    dockUnpinnedRunningApps_ = std::move(runningApps);

    if (runningLayoutChanged)
    {
        InvalidateDockContainers();
        InvalidateDragStaticScene();
    }

    if ((changed || runningVisualChanged) && invalidateChanged && hwnd_)
    {
        InvalidateRect(hwnd_, nullptr, runningLayoutChanged ? TRUE : FALSE);
    }
    dockRunningWindowsForegroundTick_ =
        dockForegroundChangedTick_.load();
    dockRunningWindowsStateTick_ =
        dockWindowListChangedTick_.load();
    dockRunningWindowsRefreshTick_ = GetTickCount();
}

inline bool DesktopApp::ActivateOrToggleDockItem(
    size_t itemIndex,
    std::optional<snowdesktop::dock_window_rules::DockClickAction>
        pressedAction,
    HWND pressedTarget,
    std::optional<RECT> pressedAnchorScreen)
{
    DismissDockWindowPreviewUntilLeave();
    if (itemIndex >= items_.size()) return false;
    const DockAppIdentity requestedIdentity =
        ResolveDockAppIdentity(itemIndex);
    if (snowdesktop::dock_window_rules::
            ShouldSuppressDockWindowCommand(
                IsDockAppClosePending(requestedIdentity)))
        return true;

    using snowdesktop::dock_window_rules::DockClickAction;
    DockClickAction action =
        pressedAction.value_or(DockClickAction::None);
    if (action == DockClickAction::Launch)
        return LaunchDesktopItem(itemIndex);

    HWND preferredWindow = nullptr;
    if (pressedTarget && IsWindow(pressedTarget))
    {
        preferredWindow = GetAncestor(pressedTarget, GA_ROOT);
        if (!preferredWindow)
            preferredWindow = pressedTarget;
    }
    if (!preferredWindow)
    {
        const DockAppIdentity identity =
            ResolveDockAppIdentity(itemIndex);
        if (identity.kind == DockAppIdentityKind::None)
            return LaunchDesktopItem(itemIndex);
    }

    const std::wstring key = DockItemWindowKey(items_[itemIndex]);
    auto found = dockRunningWindows_.find(key);
    if (preferredWindow)
    {
        if (found == dockRunningWindows_.end())
        {
            found = dockRunningWindows_.emplace(
                key, DockWindowInfo{
                    preferredWindow,
                    IsIconic(preferredWindow) != FALSE,
                    true,
                    false }).first;
        }
        else
        {
            found->second.window = preferredWindow;
            found->second.running = true;
        }
    }
    else if (found == dockRunningWindows_.end() ||
        !IsWindow(found->second.window))
    {
        // Button-down already records the exact window for normal Dock
        // clicks. Only fall back to the expensive all-window scan when that
        // cached target is genuinely unavailable or stale.
        RefreshDockRunningWindows(false);
        found = dockRunningWindows_.find(key);
    }
    if (found == dockRunningWindows_.end() || !IsWindow(found->second.window))
    {
        if (action == DockClickAction::Minimize)
            return false;
        return LaunchDesktopItem(itemIndex);
    }

    HWND target = preferredWindow
        ? preferredWindow : found->second.window;
    if (dockWindowTransition_ &&
        dockWindowTransition_->IsActive() &&
        !dockWindowTransition_->IsActiveFor(
            target))
    {
        dockWindowTransition_->Cancel();
    }
    found->second.window = target;
    found->second.minimized = IsIconic(target) != FALSE;
    if (action == DockClickAction::None)
    {
        action =
            snowdesktop::dock_window_rules::ResolveDockClickAction(
                found->second.running,
                found->second.minimized,
                found->second.foreground);
    }
    const bool transitionActiveForTarget =
        dockWindowTransition_ &&
        dockWindowTransition_->IsActiveFor(
            target);
    const auto activeTransitionDirection =
        transitionActiveForTarget
        ? dockWindowTransition_->GetDirection()
        : DockWindowTransitionDirection::Minimize;
    if (transitionActiveForTarget)
    {
        action = activeTransitionDirection ==
                DockWindowTransitionDirection::Minimize
            ? DockClickAction::Restore
            : DockClickAction::Minimize;
    }

    // The action comes from the indicator under the pointer at button-down.
    // Do not infer it again from GetForegroundWindow() during button-up.
    if (action == DockClickAction::Minimize)
    {
        const bool reverseRestore =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Restore;
        if ((!IsIconic(target) ||
                reverseRestore) &&
            dockWindowTransition_ &&
            pressedAnchorScreen)
        {
            dockWindowTransition_->StartMinimize(
                target, *pressedAnchorScreen);
        }
        if (!IsIconic(target) ||
            reverseRestore)
        {
            RequestDockWindowMinimize(target);
        }
        found->second.minimized = true;
        found->second.foreground = false;
    }
    else
    {
        const bool minimized = IsIconic(target) != FALSE;
        const bool reverseMinimize =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Minimize;
        if (action == DockClickAction::Restore &&
            (minimized || reverseMinimize) &&
            dockWindowTransition_ &&
            pressedAnchorScreen &&
            dockWindowTransition_->StartRestore(
                target, *pressedAnchorScreen,
                [this](HWND restoreTarget) {
                    ActivateDockWindowFromPreview(
                        restoreTarget);
                }))
        {
            InvalidateDockRects();
            return true;
        }
        BOOL showAccepted = FALSE;
        if (minimized)
        {
            showAccepted = ShowWindowAsync(
                target,
                DockRestoreShowCommand(target));
        }
        else
        {
            showAccepted =
                ShowWindowAsync(target, SW_SHOW);
        }
        HWND activationTarget = GetLastActivePopup(target);
        if (!activationTarget || !IsWindow(activationTarget))
            activationTarget = target;
        if (snowdesktop::dock_window_rules::
                NeedsDockWindowSwitchFallback(
                    minimized,
                    showAccepted != FALSE))
        {
            SwitchToThisWindow(target, TRUE);
            if (activationTarget != target)
                SwitchToThisWindow(
                    activationTarget, TRUE);
        }
        BringWindowToTop(activationTarget);
        SetForegroundWindow(activationTarget);
        found->second.minimized = false;
        found->second.foreground = true;
    }

    InvalidateDockRects();
    return true;
}

inline bool DesktopApp::ActivateOrToggleDockWindow(
    HWND window,
    std::optional<snowdesktop::dock_window_rules::DockClickAction>
        pressedAction,
    HWND pressedTarget,
    std::optional<RECT> pressedAnchorScreen)
{
    DismissDockWindowPreviewUntilLeave();
    HWND requestedTarget =
        pressedTarget && IsWindow(pressedTarget)
            ? pressedTarget : window;
    if (!requestedTarget || !IsWindow(requestedTarget))
        return false;
    if (snowdesktop::dock_window_rules::
            ShouldSuppressDockWindowCommand(
                IsDockWindowClosePending(requestedTarget)))
        return true;
    HWND target = GetAncestor(requestedTarget, GA_ROOT);
    if (!target) target = requestedTarget;
    if (dockWindowTransition_ &&
        dockWindowTransition_->IsActive() &&
        !dockWindowTransition_->IsActiveFor(
            target))
    {
        dockWindowTransition_->Cancel();
    }

    using snowdesktop::dock_window_rules::DockClickAction;
    const bool minimized = IsIconic(target) != FALSE;
    DockClickAction action =
        pressedAction.value_or(DockClickAction::None);
    if (action == DockClickAction::None)
    {
        const HWND foreground = GetForegroundWindow();
        action =
            snowdesktop::dock_window_rules::ResolveDockClickAction(
                true, minimized,
                DockWindowsShareApplicationIdentity(
                    target, foreground));
    }
    const bool transitionActiveForTarget =
        dockWindowTransition_ &&
        dockWindowTransition_->IsActiveFor(
            target);
    const auto activeTransitionDirection =
        transitionActiveForTarget
        ? dockWindowTransition_->GetDirection()
        : DockWindowTransitionDirection::Minimize;
    if (transitionActiveForTarget)
    {
        action = activeTransitionDirection ==
                DockWindowTransitionDirection::Minimize
            ? DockClickAction::Restore
            : DockClickAction::Minimize;
    }
    bool nowMinimized = false;
    bool nowForeground = false;
    if (action == DockClickAction::Minimize)
    {
        const bool reverseRestore =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Restore;
        if ((!minimized || reverseRestore) &&
            dockWindowTransition_ &&
            pressedAnchorScreen)
        {
            dockWindowTransition_->StartMinimize(
                target, *pressedAnchorScreen);
        }
        if (!minimized ||
            reverseRestore)
        {
            RequestDockWindowMinimize(target);
        }
        nowMinimized = true;
    }
    else
    {
        const bool reverseMinimize =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Minimize;
        if (action == DockClickAction::Restore &&
            (minimized || reverseMinimize) &&
            dockWindowTransition_ &&
            pressedAnchorScreen &&
            dockWindowTransition_->StartRestore(
                target, *pressedAnchorScreen,
                [this](HWND restoreTarget) {
                    ActivateDockWindowFromPreview(
                        restoreTarget);
                }))
        {
            InvalidateDockRects();
            return true;
        }
        BOOL showAccepted = FALSE;
        if (minimized)
        {
            showAccepted = ShowWindowAsync(
                target,
                DockRestoreShowCommand(target));
        }
        else
        {
            showAccepted =
                ShowWindowAsync(target, SW_SHOW);
        }
        HWND activationTarget = GetLastActivePopup(target);
        if (!activationTarget || !IsWindow(activationTarget))
            activationTarget = target;
        if (snowdesktop::dock_window_rules::
                NeedsDockWindowSwitchFallback(
                    minimized,
                    showAccepted != FALSE))
        {
            SwitchToThisWindow(target, TRUE);
            if (activationTarget != target)
                SwitchToThisWindow(
                    activationTarget, TRUE);
        }
        BringWindowToTop(activationTarget);
        SetForegroundWindow(activationTarget);
        nowForeground = true;
    }

    for (DockRunningAppInfo& app : dockUnpinnedRunningApps_)
    {
        const bool matchesTarget =
            DockWindowsShareApplicationIdentity(
                app.window, target);
        if (nowForeground) app.foreground = matchesTarget;
        if (matchesTarget)
        {
            app.minimized = nowMinimized;
            app.foreground = nowForeground;
        }
    }
    InvalidateDockRects();
    return true;
}

inline void DesktopApp::ActivateDockWindowFromPreview(HWND window)
{
    DismissDockWindowPreviewUntilLeave();
    if (!window || !IsWindow(window))
        return;
    if (snowdesktop::dock_window_rules::
            ShouldSuppressDockWindowCommand(
                IsDockWindowClosePending(window)))
        return;
    HWND target = GetAncestor(window, GA_ROOT);
    if (!target)
        target = window;

    const bool minimized = IsIconic(target) != FALSE;
    BOOL showAccepted = FALSE;
    if (minimized)
    {
        showAccepted = ShowWindowAsync(
            target,
            DockRestoreShowCommand(target));
    }
    else
    {
        showAccepted =
            ShowWindowAsync(target, SW_SHOW);
    }
    HWND activationTarget = GetLastActivePopup(target);
    if (!activationTarget || !IsWindow(activationTarget))
        activationTarget = target;
    if (snowdesktop::dock_window_rules::
            NeedsDockWindowSwitchFallback(
                minimized,
                showAccepted != FALSE))
    {
        SwitchToThisWindow(target, TRUE);
        if (activationTarget != target)
            SwitchToThisWindow(activationTarget, TRUE);
    }
    if (floatingDockVisible_)
        CloseFloatingDock();
    BringWindowToTop(activationTarget);
    SetForegroundWindow(activationTarget);

    // ShowWindowAsync has not necessarily updated IsIconic yet. Update the
    // known target optimistically instead of doing a synchronous EnumWindows
    // scan that can both block the animation handoff and write the old state
    // straight back into the cache.
    for (auto& [key, state] : dockRunningWindows_)
    {
        (void)key;
        if (!state.window || !IsWindow(state.window))
            continue;
        const bool matchesTarget =
            state.window == target ||
            DockWindowsShareApplicationIdentity(
                state.window, target);
        state.foreground = matchesTarget;
        if (matchesTarget)
        {
            state.window = target;
            state.running = true;
            state.minimized = false;
        }
    }
    for (DockRunningAppInfo& app :
         dockUnpinnedRunningApps_)
    {
        if (!app.window || !IsWindow(app.window))
            continue;
        const bool matchesTarget =
            app.window == target ||
            DockWindowsShareApplicationIdentity(
                app.window, target);
        app.foreground = matchesTarget;
        if (matchesTarget)
        {
            app.window = target;
            app.minimized = false;
        }
    }
    InvalidateDockRects();
}

inline DockContainer* DesktopApp::GetDockContainer() const
{
    for (const auto& container : containers_)
        if (auto* dock = dynamic_cast<DockContainer*>(container.get())) return dock;
    return nullptr;
}

inline DockContainer* DesktopApp::GetDockContainerAtPoint(POINT point) const
{
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        if (desktopIconsHidden_ &&
            !dockSettings_.keepWhenDesktopHidden &&
            !(floatingDockVisible_ &&
                dock == floatingDockContainer_))
            continue;
        if (dock->ContainsInteractivePoint(point)) return dock;
    }
    return nullptr;
}

inline void DesktopApp::InvalidateDockContainers()
{
    for (const auto& container : containers_)
    {
        if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
            dock->InvalidateSlots();
    }
}

inline void DesktopApp::InvalidateDockRects(BOOL erase) const
{
    if (floatingDockVisible_)
        InvalidateFloatingDockWindow(true);
    if (!hwnd_) return;
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock ||
            !snowdesktop::floating_dock_rules::
                ShouldRenderDesktopDock(
                    floatingDockVisible_,
                    dock ==
                        floatingDockContainer_))
            continue;
        const RECT bounds = dock->GetInteractiveBounds();
        InvalidateRect(hwnd_, &bounds, erase);
    }
}

inline void DesktopApp::ClearDockBackdropForDragTransition(
    POINT previousPointer, POINT currentPointer)
{
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;

        const RECT previousPanel =
            dock->GetVisualPanelBounds(previousPointer);
        const RECT currentPanel =
            dock->GetVisualPanelBounds(currentPointer);
        desktopBackdropCompositor_.RemovePanel(previousPanel);
        if (!EqualRect(&previousPanel, &currentPanel))
            desktopBackdropCompositor_.RemovePanel(currentPanel);
    }
}

inline int DesktopApp::GetGridPageItemIconSize(const GridPage& page) const
{
    const int pitchX = page.cellWidth + (page.columns > 1 ? page.gapX : 0);
    const int pitchY = page.cellHeight + (page.rows > 1 ? page.gapY : 0);
    const float layoutScale = std::max(0.1f, std::min(
        static_cast<float>(std::max(1, pitchX)) / static_cast<float>(kCellWidth),
        static_cast<float>(std::max(1, pitchY)) / static_cast<float>(kMinCellHeight)));
    const int inset = std::max(1, static_cast<int>(std::round(2.0f * layoutScale)));
    if (page.cellHeight < static_cast<int>(std::round(50.0f * layoutScale)))
    {
        return std::max(1, std::min({
            static_cast<int>(std::round(32.0f * layoutScale)),
            std::max(1, page.cellWidth - inset * 2),
            std::max(1, page.cellHeight - inset * 2) }));
    }
    const float lineHeight = itemFontSize_ * 7.0f / 6.0f * layoutScale;
    const int textHeight = std::max(1,
        static_cast<int>(std::floor(lineHeight * 2.0f)) - 1);
    return std::max(1, std::min(
        std::max(1, page.cellWidth - inset * 2),
        std::max(1, page.cellHeight - textHeight - inset * 2)));
}

inline void DesktopApp::ApplyDockWorkAreaReservation()
{
    if (dockWorkAreaReservationApplied_)
    {
        for (const RECT& dockArea : dockAreas_)
        {
            for (auto& page : gridPages_)
            {
                RECT intersect;
                if (!IntersectRect(
                        &intersect, &dockArea,
                        &page.bounds))
                    continue;
                int reserved;
                switch (dockWorkAreaReservationPosition_)
                {
                case DockPosition::Top:
                    reserved = dockArea.bottom -
                        dockArea.top;
                    page.workArea.top = std::max(
                        page.bounds.top,
                        page.workArea.top - reserved);
                    break;
                case DockPosition::Bottom:
                    reserved = dockArea.bottom -
                        dockArea.top;
                    page.workArea.bottom = std::min(
                        page.bounds.bottom,
                        page.workArea.bottom + reserved);
                    break;
                case DockPosition::Left:
                    reserved = dockArea.right -
                        dockArea.left;
                    page.workArea.left = std::max(
                        page.bounds.left,
                        page.workArea.left - reserved);
                    break;
                case DockPosition::Right:
                    reserved = dockArea.right -
                        dockArea.left;
                    page.workArea.right = std::min(
                        page.bounds.right,
                        page.workArea.right + reserved);
                    break;
                }
                break;
            }
        }
    }

    dockAreas_.clear();
    dockWorkAreaReservationApplied_ = false;
    if (!generalSettings_.dockEnabled || gridPages_.empty()) return;

    std::vector<size_t> targetPages = BuildMonitorRenderOrder();
    if (targetPages.empty())
    {
        targetPages.resize(gridPages_.size());
        std::iota(targetPages.begin(), targetPages.end(), size_t{ 0 });
    }
    if (targetPages.size() > 1)
    {
        switch (dockSettings_.monitorScope)
        {
        case DockMonitorScope::Last:
            targetPages.erase(targetPages.begin(), targetPages.end() - 1);
            break;
        case DockMonitorScope::First:
            targetPages.erase(targetPages.begin() + 1, targetPages.end());
            break;
        case DockMonitorScope::All:
        default:
            break;
        }
    }

    const bool vertical = dockSettings_.position == DockPosition::Left ||
        dockSettings_.position == DockPosition::Right;

    for (size_t pageIndex : targetPages)
    {
        if (pageIndex >= gridPages_.size()) continue;
        GridPage& targetPage = gridPages_[pageIndex];
        const RECT originalWorkArea = targetPage.workArea;
        const int width = std::max(1, static_cast<int>(
            originalWorkArea.right - originalWorkArea.left));
        const int height = std::max(1, static_cast<int>(
            originalWorkArea.bottom - originalWorkArea.top));
        const int edgeExtent = vertical ? width : height;
        auto reserveEdge = [&](GridPage& page, int reserved, RECT* dockArea) {
            page.workArea = originalWorkArea;
            RECT area{};
            switch (dockSettings_.position)
            {
            case DockPosition::Top:
                area = RECT{ originalWorkArea.left, originalWorkArea.top,
                    originalWorkArea.right, originalWorkArea.top + reserved };
                page.workArea.top = area.bottom;
                break;
            case DockPosition::Left:
                area = RECT{ originalWorkArea.left, originalWorkArea.top,
                    originalWorkArea.left + reserved, originalWorkArea.bottom };
                page.workArea.left = area.right;
                break;
            case DockPosition::Right:
                area = RECT{ originalWorkArea.right - reserved, originalWorkArea.top,
                    originalWorkArea.right, originalWorkArea.bottom };
                page.workArea.right = area.left;
                break;
            case DockPosition::Bottom:
            default:
                area = RECT{ originalWorkArea.left, originalWorkArea.bottom - reserved,
                    originalWorkArea.right, originalWorkArea.bottom };
                page.workArea.bottom = area.top;
                break;
            }
            if (dockArea) *dockArea = area;
        };

        // Match each Dock copy to the icon grid of its own display. This also
        // keeps mixed-resolution monitors from inheriting another screen's size.
        GridPage bestPage = targetPage;
        int bestReserved = 1;
        int bestError = INT_MAX;
        for (int reserved = 1; reserved < edgeExtent; ++reserved)
        {
            GridPage candidate = targetPage;
            reserveEdge(candidate, reserved, nullptr);
            ApplyIconSpacingToPage(candidate);
            const int componentMargin = vertical
                ? candidate.marginX : candidate.marginY;
            const float dockScale = ClampDockScale(dockSettings_.thicknessScale);
            const int scaledIconSize = std::max(1, static_cast<int>(std::round(
                GetGridPageItemIconSize(candidate) * dockScale)));
            const int scaledSpacing = std::max(1, static_cast<int>(std::round(
                kDockSpacing * dockScale)));
            const int edgeDistance =
                std::max(
                    scaledSpacing, componentMargin);
            const int innerGap =
                edgeDistance - componentMargin;
            const int panelThickness = scaledIconSize + scaledSpacing * 2;
            const int desiredReservation = dockSettings_.edgeAttached
                ? panelThickness + innerGap
                : panelThickness + edgeDistance + innerGap;
            const int error = std::abs(desiredReservation - reserved);
            if (error < bestError)
            {
                bestError = error;
                bestReserved = reserved;
                bestPage = candidate;
                if (error == 0) break;
            }
        }

        targetPage = bestPage;
        RECT dockArea{};
        reserveEdge(targetPage, bestReserved, &dockArea);
        ApplyIconSpacingToPage(targetPage);
        if (!IsRectEmptyRect(dockArea)) dockAreas_.push_back(dockArea);
    }
    dockWorkAreaReservationApplied_ =
        !dockAreas_.empty();
    if (dockWorkAreaReservationApplied_)
        dockWorkAreaReservationPosition_ =
            dockSettings_.position;
}

inline void DesktopApp::CommitDockDrop(const std::vector<Item*>& sourceItems,
    Container* origin, DockContainer* targetDock, size_t insertIndex, int mods)
{
    if (!targetDock || sourceItems.empty()) return;

    if (dynamic_cast<DockContainer*>(origin))
    {
        std::vector<size_t> indices;
        for (Item* item : sourceItems)
            if (auto* dockItem = dynamic_cast<DockEntryItem*>(item))
            {
                const size_t index = dockItem->GetEntryIndex();
                if (index < dockEntries_.size() &&
                    !IsRecycleBinDockEntry(dockEntries_[index]))
                    indices.push_back(index);
            }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        if (indices.empty()) return;

        std::vector<DockEntry> moving;
        for (size_t index : indices)
            if (index < dockEntries_.size()) moving.push_back(dockEntries_[index]);
        const bool movingFolders = std::all_of(
            moving.begin(), moving.end(),
            [this](const DockEntry& entry) {
                return IsFolderDockEntry(entry);
            });
        const bool movingMain = std::all_of(
            moving.begin(), moving.end(),
            [this](const DockEntry& entry) {
                return !IsFolderDockEntry(entry);
            });
        if (!movingFolders && !movingMain)
        {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        {
            if (*it < insertIndex) --insertIndex;
            dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        const size_t mainEnd = DockMainEntryCount();
        const size_t folderEnd =
            mainEnd + DockFolderEntryCount();
        insertIndex = movingFolders
            ? std::clamp(insertIndex, mainEnd, folderEnd)
            : std::min(insertIndex, mainEnd);
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            moving.begin(), moving.end());
        NormalizeDockRecycleBinPosition();
        InvalidateDockContainers();
        return;
    }

    const bool folderEntriesOnly =
        std::all_of(
            sourceItems.begin(), sourceItems.end(),
            [](Item* source) {
                return dynamic_cast<FolderEntryIcon*>(
                    source) != nullptr;
            });
    if (folderEntriesOnly)
    {
        DragSourceList sourceList =
            BuildDragSourceList(sourceItems, origin);
        const auto existingKeys =
            SnapshotDesktopKeys();
        DropPreviewList preview =
            BuildDropPreviewList(
                sourceList, GetDesktopGrid(),
                nullptr, HitRegion::Empty,
                mods, dragSession_.CurrentPoint());
        preview.action = DropAction::Link;
        if (ExecuteDropPipeline(
                sourceList, preview))
        {
            AddExternalItemsToDock(
                NewDesktopKeysSince(existingKeys),
                insertIndex);
            SaveLayoutSlots();
        }
        return;
    }

    const bool keepSource = (mods & MK_CONTROL) != 0;
    std::vector<DockEntry> additions;
    for (Item* source : sourceItems)
    {
        if (auto* icon = dynamic_cast<DesktopIcon*>(source))
        {
            DesktopItem* item = icon->GetDesktopItem();
            if (!item || item->layoutKey.empty()) continue;
            additions.push_back({ DockEntryType::DesktopItem,
                ToUpperInvariant(item->layoutKey), keepSource });
            continue;
        }

        auto* widget = dynamic_cast<Widget*>(source);
        DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
        if (data && data->type == DesktopWidgetType::Collection)
            additions.push_back({ DockEntryType::Collection, data->id, false });
        else if (data && data->type == DesktopWidgetType::FolderMapping)
            additions.push_back({ DockEntryType::FolderMapping, data->id, false });
        else if (auto* groupEntry =
                     dynamic_cast<FileGroupEntryItem*>(source))
        {
            const size_t widgetIndex =
                FindWidgetIndexById(
                    groupEntry->GetChildWidgetId());
            if (widgetIndex < widgets_.size() &&
                widgets_[widgetIndex].type ==
                    DesktopWidgetType::FolderMapping)
            {
                additions.push_back({
                    DockEntryType::FolderMapping,
                    widgets_[widgetIndex].id,
                    false });
            }
        }
    }
    if (additions.empty()) return;

    size_t genuinelyNew = 0;
    for (const DockEntry& addition : additions)
    {
        bool exists = std::any_of(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& current) {
                return current.type == addition.type &&
                    ToUpperInvariant(current.reference) == ToUpperInvariant(addition.reference);
            });
        if (!exists) ++genuinelyNew;
    }
    if (!targetDock->HasCapacity(genuinelyNew))
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    for (const DockEntry& addition : additions)
    {
        auto existing = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& current) {
                return current.type == addition.type &&
                    ToUpperInvariant(current.reference) == ToUpperInvariant(addition.reference);
            });
        if (existing != dockEntries_.end())
        {
            const bool becomingExclusive = existing->keepOnDesktop && !addition.keepOnDesktop;
            existing->keepOnDesktop = addition.keepOnDesktop;
            if (addition.type == DockEntryType::Collection ||
                addition.type == DockEntryType::FolderMapping)
            {
                existing->keepOnDesktop = false;
                size_t widgetIndex = FindWidgetIndexById(addition.reference);
                if (widgetIndex < widgets_.size())
                {
                    if (addition.type == DockEntryType::FolderMapping)
                    {
                        for (auto& group : widgets_)
                        {
                            if (group.type != DesktopWidgetType::FileGroup) continue;
                            std::erase(group.childWidgetIds, addition.reference);
                            group.activeCategoryId =
                                snowdesktop::collection_group_rules::
                                    ResolveActiveItem(
                                        group.childWidgetIds,
                                        group.activeCategoryId);
                        }
                    }
                    widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
                }
                continue;
            }
            if (becomingExclusive)
            {
                RemoveDesktopKeysFromWidgets({ addition.reference });
                size_t itemIndex = FindItemIndexByKey(addition.reference);
                if (itemIndex < items_.size())
                    items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
            }
            continue;
        }
        auto recycleBin = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [this](const DockEntry& entry) { return IsRecycleBinDockEntry(entry); });
        const size_t sortableEnd = static_cast<size_t>(
            std::distance(dockEntries_.begin(), recycleBin));
        insertIndex = IsRecycleBinDockEntry(addition)
            ? dockEntries_.size()
            : std::min(insertIndex, sortableEnd);
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex), addition);
        if (!IsRecycleBinDockEntry(addition))
            ++insertIndex;

        if (addition.keepOnDesktop && addition.type == DockEntryType::DesktopItem) continue;
        if (addition.type == DockEntryType::DesktopItem)
        {
            RemoveDesktopKeysFromWidgets({ addition.reference });
            size_t itemIndex = FindItemIndexByKey(addition.reference);
            if (itemIndex < items_.size()) items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(addition.reference);
            if (widgetIndex < widgets_.size())
            {
                if (addition.type == DockEntryType::FolderMapping)
                {
                    for (auto& group : widgets_)
                    {
                        if (group.type != DesktopWidgetType::FileGroup) continue;
                        std::erase(group.childWidgetIds, addition.reference);
                        group.activeCategoryId =
                            snowdesktop::collection_group_rules::
                                ResolveActiveItem(
                                    group.childWidgetIds,
                                    group.activeCategoryId);
                    }
                }
                widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
            }
        }
    }
    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    InvalidateDockContainers();
    InvalidateDragStaticScene();
}

inline void DesktopApp::AddExternalItemsToDock(
    const std::vector<std::wstring>& newKeys, size_t insertIndex)
{
    bool hasDock = false;
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        hasDock = true;
        if (!dock->HasCapacity(newKeys.size())) return;
    }
    if (!hasDock) return;
    for (const std::wstring& key : newKeys)
    {
        const std::wstring upper = ToUpperInvariant(key);
        if (upper.empty() ||
            snowdesktop::
                shell_item_visibility::
                    IsAlwaysHidden(upper))
            continue;
        bool exists = std::any_of(dockEntries_.begin(), dockEntries_.end(),
            [&](const DockEntry& entry) {
                return entry.type == DockEntryType::DesktopItem &&
                    ToUpperInvariant(entry.reference) == upper;
            });
        if (exists) continue;
        auto recycleBin = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [this](const DockEntry& entry) { return IsRecycleBinDockEntry(entry); });
        insertIndex = std::min(insertIndex,
            static_cast<size_t>(std::distance(dockEntries_.begin(), recycleBin)));
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            DockEntry{ DockEntryType::DesktopItem, upper, false });
        ++insertIndex;
        size_t itemIndex = FindItemIndexByKey(upper);
        if (itemIndex < items_.size()) items_[itemIndex].gridCell = { kDockPageId, 0, 0 };
    }
    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    RebuildContainersAndItems();
    LayoutItems();
}

inline bool DesktopApp::FindDockReturnCell(
    std::unordered_set<std::wstring>& usedSlots,
    const std::wstring& preferredPageId, int startSlot, GridSpan span,
    GridCell& result)
{
    span.columns = std::max(1, span.columns);
    span.rows = std::max(1, span.rows);
    if (TryFindFreeCell(span, usedSlots, result, preferredPageId, startSlot))
        return true;

    // Try saved pages that are currently off-screen before allocating a new one.
    for (const std::wstring& pageId : savedPageIds_)
    {
        auto columnsIt = savedPageColumns_.find(pageId);
        auto rowsIt = savedPageRows_.find(pageId);
        if (columnsIt == savedPageColumns_.end() || rowsIt == savedPageRows_.end()) continue;
        const int columns = std::max(1, columnsIt->second);
        const int rows = std::max(1, rowsIt->second);
        for (int slot = 0; slot < columns * rows; ++slot)
        {
            GridCell candidate{ pageId, slot / rows, slot % rows };
            if (candidate.column + span.columns <= columns &&
                candidate.row + span.rows <= rows &&
                !AreGridSlotsMarked(usedSlots, candidate, span))
            {
                result = candidate;
                return true;
            }
        }
    }

    if (gridPages_.empty()) return false;
    const std::vector<size_t> order = BuildMonitorRenderOrder();
    const GridPage& reference = gridPages_[order.empty() ? 0 : order.back()];
    const std::wstring pageId = GeneratePageId();
    RememberSavedPageId(pageId);
    savedPageColumns_[pageId] = std::max(span.columns, reference.columns);
    savedPageRows_[pageId] = std::max(span.rows, reference.rows);
    result = { pageId, 0, 0 };
    return true;
}

inline bool DesktopApp::DropItemsIntoDockCollection(
    const std::vector<Item*>& sourceItems, Container* origin,
    DockEntryItem* targetItem, int mods)
{
    if (!targetItem || targetItem->GetEntryType() != DockEntryType::Collection)
        return false;
    size_t widgetIndex = FindWidgetIndexById(targetItem->GetReference());
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        return false;

    Collection collection(&widgets_[widgetIndex], this);
    DragSourceList sourceList = BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = BuildDropPreviewList(sourceList, &collection,
        nullptr, HitRegion::SortAfter, mods, dragSession_.CurrentPoint());
    return ExecuteDropPipeline(sourceList, preview);
}

inline void DesktopApp::MoveDockItemsToDesktop(
    const std::vector<Item*>& sourceItems, GridCell targetCell)
{
    std::vector<size_t> indices;
    for (Item* source : sourceItems)
        if (auto* item = dynamic_cast<DockEntryItem*>(source))
            indices.push_back(item->GetEntryIndex());
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.empty()) return;

    std::vector<std::pair<size_t, DockEntry>> moving;
    for (size_t index : indices)
    {
        if (index < dockEntries_.size())
            moving.emplace_back(index, dockEntries_[index]);
    }

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& widget : widgets_)
        if (!IsGroupedWidget(widget) &&
            widget.gridCell.pageId != kDockPageId)
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);

    const GridPage* targetPage = FindGridPage(gridPages_, targetCell.pageId);
    int startSlot = targetPage ? SlotFromCell(gridPages_, targetCell) : 0;
    std::vector<size_t> restoredIndices;
    for (size_t movingIndex = 0;
        movingIndex < moving.size();
        ++movingIndex)
    {
        const size_t entryIndex =
            moving[movingIndex].first;
        const DockEntry& entry =
            moving[movingIndex].second;
        if (entry.keepOnDesktop) continue;
        GridSpan span{ 1, 1 };
        if (entry.type == DockEntryType::Collection ||
            entry.type == DockEntryType::FolderMapping)
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) span = widgets_[widgetIndex].gridSpan;
        }
        GridCell freeCell;
        if (!FindDockReturnCell(usedSlots, targetCell.pageId, startSlot, span, freeCell))
            continue;
        MarkGridArea(usedSlots, freeCell, span);
        ++startSlot;
        bool restored = false;
        if (entry.type == DockEntryType::DesktopItem)
        {
            size_t itemIndex = FindItemIndexByKey(entry.reference);
            if (itemIndex < items_.size())
            {
                items_[itemIndex].gridCell = freeCell;
                restored = true;
            }
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size())
            {
                widgets_[widgetIndex].gridCell = freeCell;
                restored = true;
            }
        }
        if (restored)
            restoredIndices.push_back(entryIndex);
    }

    for (auto it = restoredIndices.rbegin();
        it != restoredIndices.rend(); ++it)
        dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));
    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    RebuildContainersAndItems();
    LayoutItems();
    InvalidateDragStaticScene();
}

inline void DesktopApp::RestoreDockEntriesToDesktop()
{
    if (dockEntries_.empty()) return;
    const GridPage* first = GetFirstPageGridPage();
    const std::wstring preferredPage = first ? first->id : L"";
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& widget : widgets_)
        if (!IsGroupedWidget(widget) &&
            widget.gridCell.pageId != kDockPageId)
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);

    int startSlot = 0;
    for (const DockEntry& entry : dockEntries_)
    {
        if (entry.keepOnDesktop) continue;
        GridSpan span{ 1, 1 };
        if (entry.type == DockEntryType::Collection ||
            entry.type == DockEntryType::FolderMapping)
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) span = widgets_[widgetIndex].gridSpan;
        }
        GridCell cell;
        if (!FindDockReturnCell(usedSlots, preferredPage, startSlot, span, cell)) continue;
        MarkGridArea(usedSlots, cell, span);
        ++startSlot;
        if (entry.type == DockEntryType::DesktopItem)
        {
            size_t itemIndex = FindItemIndexByKey(entry.reference);
            if (itemIndex < items_.size()) items_[itemIndex].gridCell = cell;
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) widgets_[widgetIndex].gridCell = cell;
        }
    }
    dockEntries_.clear();
    RefreshCollectedKeysCache();
}

inline bool DesktopApp::DrawDockControlBackground(
    ID2D1DeviceContext* ctx, RECT rect, int state, bool forceWhiteStyle)
{
    if (!ctx || IsRectEmptyRect(rect)) return false;
    PersonalizationSettings appearance = PersonalizationSettings::DarkPreset();
    if (settingsWindow_)
        appearance = settingsWindow_->GetPersonalization();
    else
        LoadPersonalization(GetPersonalizationPath().c_str(), appearance);

    const float luminance = appearance.widgetBgR * 0.2126f +
        appearance.widgetBgG * 0.7152f + appearance.widgetBgB * 0.0722f;
    const bool lightSurface = !forceWhiteStyle &&
        luminance > 0.58f && appearance.widgetAlpha > 0.10f;
    const bool active = state > 0;
    const D2D1_COLOR_F fill = forceWhiteStyle
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, active ? 0.18f : 0.11f)
        : (active
            ? D2D1::ColorF(0.39f, 0.66f, 1.0f, lightSurface ? 0.20f : 0.25f)
            : (lightSurface
                ? D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.075f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f)));
    const D2D1_COLOR_F border = forceWhiteStyle
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, active ? 0.36f : 0.20f)
        : (active
            ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.88f)
            : (lightSurface
                ? D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.14f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f)));
    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int height = std::max(1, static_cast<int>(rect.bottom - rect.top));
    const float scale = static_cast<float>(std::min(width, height)) / 52.0f;
    DrawBeautifiedIconPlate(ctx, rect, fill, border,
        (active ? 1.6f : 1.0f) * std::max(0.75f, scale));
    return lightSurface;
}

inline void DesktopApp::DrawDockSelectionIndicator(
    ID2D1DeviceContext* ctx, RECT iconRect, bool lightTheme)
{
    if (!ctx || IsRectEmptyRect(iconRect))
        return;

    const float iconSize = static_cast<float>(std::max<LONG>(1, std::min(
        iconRect.right - iconRect.left,
        iconRect.bottom - iconRect.top)));
    const float halfSize = std::clamp(iconSize * 0.06f, 3.0f, 5.0f);
    const float gap = std::max(3.0f, iconSize * 0.06f);
    const float centerX = (iconRect.left + iconRect.right) * 0.5f;
    const float centerY = (iconRect.top + iconRect.bottom) * 0.5f;
    D2D1_POINT_2F tip{};
    D2D1_POINT_2F baseA{};
    D2D1_POINT_2F baseB{};
    switch (dockSettings_.position)
    {
    case DockPosition::Top:
    {
        const float y = static_cast<float>(iconRect.top) - gap;
        tip = D2D1::Point2F(centerX, y + halfSize);
        baseA = D2D1::Point2F(centerX - halfSize, y - halfSize);
        baseB = D2D1::Point2F(centerX + halfSize, y - halfSize);
        break;
    }
    case DockPosition::Left:
    {
        const float x = static_cast<float>(iconRect.left) - gap;
        tip = D2D1::Point2F(x + halfSize, centerY);
        baseA = D2D1::Point2F(x - halfSize, centerY - halfSize);
        baseB = D2D1::Point2F(x - halfSize, centerY + halfSize);
        break;
    }
    case DockPosition::Right:
    {
        const float x = static_cast<float>(iconRect.right) + gap;
        tip = D2D1::Point2F(x - halfSize, centerY);
        baseA = D2D1::Point2F(x + halfSize, centerY - halfSize);
        baseB = D2D1::Point2F(x + halfSize, centerY + halfSize);
        break;
    }
    case DockPosition::Bottom:
    default:
    {
        const float y = static_cast<float>(iconRect.bottom) + gap;
        tip = D2D1::Point2F(centerX, y - halfSize);
        baseA = D2D1::Point2F(centerX - halfSize, y + halfSize);
        baseB = D2D1::Point2F(centerX + halfSize, y + halfSize);
        break;
    }
    }

    ComPtr<ID2D1Factory> factory;
    ctx->GetFactory(&factory);
    if (!factory)
        return;
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
        return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink)) || !sink)
        return;
    sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(baseA);
    sink->AddLine(baseB);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        return;

    ComPtr<ID2D1SolidColorBrush> brush;
    const D2D1_COLOR_F color = lightTheme
        ? D2D1::ColorF(0.08f, 0.42f, 0.94f, 0.96f)
        : D2D1::ColorF(0.35f, 0.68f, 1.0f, 1.0f);
    if (SUCCEEDED(ctx->CreateSolidColorBrush(color, &brush)) && brush)
        ctx->FillGeometry(geometry.Get(), brush.Get());
}

inline void DesktopApp::DrawDockEntry(ID2D1DeviceContext* ctx,
    const DockEntry& entry, RECT rect, int state)
{
    if (!ctx) return;
    const int scaledSpacing = std::max(1, static_cast<int>(std::round(
        kDockSpacing * ClampDockScale(dockSettings_.thicknessScale))));
    const int iconSize = std::max(1, static_cast<int>(std::min(
        rect.right - rect.left, rect.bottom - rect.top)) - scaledSpacing);
    RECT iconRect{
        rect.left + (rect.right - rect.left - iconSize) / 2,
        rect.top + (rect.bottom - rect.top - iconSize) / 2,
        rect.left + (rect.right - rect.left + iconSize) / 2,
        rect.top + (rect.bottom - rect.top + iconSize) / 2
    };
    const RECT indicatorIconRect = iconRect;
    const bool lt = IsLightContentTheme();

    auto drawDesktopItem = [&](const DesktopItem& item, RECT target) {
        RECT bitmapTarget = target;
        const bool recycleBin = _wcsicmp(item.desktopIconClsid.c_str(),
            kDesktopIconClsidRecycleBin) == 0;
        if (recycleBin)
        {
            DrawDockControlBackground(ctx, target, 0, !lt);
            const int shortSide = std::max(1, static_cast<int>(std::min(
                target.right - target.left, target.bottom - target.top)));
            const int inset = std::max(1, static_cast<int>(std::round(shortSide * 0.16f)));
            InflateRect(&bitmapTarget, -inset, -inset);
        }
        const float alpha = item.isCut ? 0.4f : 1.0f;
        if (item.iconState == IconState::Loading)
            DrawPlaceholderIcon(ctx, item.sysIconIndex, bitmapTarget, alpha, !recycleBin);
        else if (ID2D1Bitmap1* bitmap = recycleBin
            ? GetOrCreateD2DBitmap(item.iconBitmap, false)
            : GetOrCreateD2DBitmap(item.iconBitmap))
            ctx->DrawBitmap(bitmap, ToD2DRect(bitmapTarget), alpha,
                D2D1_INTERPOLATION_MODE_LINEAR);
        else
            DrawPlaceholderIcon(ctx, item.sysIconIndex, bitmapTarget, alpha, !recycleBin);
        if (ShouldDrawShortcutArrow(item.isShortcut, item.isApplicationShortcut) &&
            item.iconState != IconState::Loading)
            DrawShortcutArrowOverlay(ctx, bitmapTarget, alpha);
    };

    if (entry.type == DockEntryType::DesktopItem)
    {
        size_t index = FindItemIndexByKey(entry.reference);
        if (index >= items_.size()) return;
        const float launchOffset =
            GetDockLaunchBounceOffset(index, iconSize);
        float launchOffsetX = 0.0f;
        float launchOffsetY = 0.0f;
        switch (dockSettings_.position)
        {
        case DockPosition::Top:
            launchOffsetY = launchOffset;
            break;
        case DockPosition::Left:
            launchOffsetX = launchOffset;
            break;
        case DockPosition::Right:
            launchOffsetX = -launchOffset;
            break;
        case DockPosition::Bottom:
        default:
            launchOffsetY = -launchOffset;
            break;
        }
        D2D1_MATRIX_3X2_F previousTransform{};
        ctx->GetTransform(&previousTransform);
        const bool launchTransformApplied =
            std::abs(launchOffsetX) > 0.001f ||
            std::abs(launchOffsetY) > 0.001f;
        if (launchTransformApplied)
        {
            ctx->SetTransform(
                D2D1::Matrix3x2F::Translation(
                    launchOffsetX,
                    launchOffsetY) *
                previousTransform);
        }
        drawDesktopItem(items_[index], iconRect);
        if (state == 2)
            DrawDockSelectionIndicator(ctx, iconRect, lt);
        if (launchTransformApplied)
            ctx->SetTransform(previousTransform);

        const DockWindowVisualState windowState = GetDockWindowVisualState(index);
        if (windowState != DockWindowVisualState::Closed && state != 2)
        {
            const bool minimized = windowState == DockWindowVisualState::Minimized;
            const bool foreground = windowState == DockWindowVisualState::Foreground;
            const bool verticalDock = dockSettings_.position == DockPosition::Left ||
                dockSettings_.position == DockPosition::Right;
            const float dotRadius = std::max(1.7f, iconSize * 0.038f);
            const float indicatorGap = std::max(3.0f, iconSize * 0.06f);
            D2D1_POINT_2F center{};
            switch (dockSettings_.position)
            {
            case DockPosition::Top:
                center = D2D1::Point2F((rect.left + rect.right) * 0.5f,
                    static_cast<float>(indicatorIconRect.top) - indicatorGap);
                break;
            case DockPosition::Left:
                center = D2D1::Point2F(static_cast<float>(indicatorIconRect.left) - indicatorGap,
                    (rect.top + rect.bottom) * 0.5f);
                break;
            case DockPosition::Right:
                center = D2D1::Point2F(static_cast<float>(indicatorIconRect.right) + indicatorGap,
                    (rect.top + rect.bottom) * 0.5f);
                break;
            case DockPosition::Bottom:
            default:
                center = D2D1::Point2F((rect.left + rect.right) * 0.5f,
                    static_cast<float>(indicatorIconRect.bottom) + indicatorGap);
                break;
            }

            ComPtr<ID2D1SolidColorBrush> indicatorBrush;
            const auto indicator =
                snowdesktop::dock_window_rules::
                    ResolveDockRunningIndicatorColor(
                        lt, foreground, minimized);
            const D2D1_COLOR_F indicatorColor =
                D2D1::ColorF(
                    indicator.red,
                    indicator.green,
                    indicator.blue,
                    indicator.alpha);
            if (SUCCEEDED(ctx->CreateSolidColorBrush(indicatorColor, &indicatorBrush)) &&
                indicatorBrush)
            {
                if (minimized)
                {
                    ctx->FillEllipse(D2D1::Ellipse(
                        center, dotRadius, dotRadius), indicatorBrush.Get());
                }
                else
                {
                    const float longHalf = foreground
                        ? std::max(6.0f, iconSize * 0.14f)
                        : std::max(4.0f, iconSize * 0.09f);
                    const float shortHalf = foreground
                        ? std::max(1.45f, iconSize * 0.031f)
                        : std::max(1.2f, iconSize * 0.026f);
                    const D2D1_RECT_F bar = verticalDock
                        ? D2D1::RectF(center.x - shortHalf, center.y - longHalf,
                            center.x + shortHalf, center.y + longHalf)
                        : D2D1::RectF(center.x - longHalf, center.y - shortHalf,
                            center.x + longHalf, center.y + shortHalf);
                    ctx->FillRoundedRectangle(D2D1::RoundedRect(
                        bar, shortHalf, shortHalf), indicatorBrush.Get());
                }
            }
        }
        return;
    }

    size_t widgetIndex = FindWidgetIndexById(entry.reference);
    if (widgetIndex >= widgets_.size()) return;
    const DesktopWidget& widget = widgets_[widgetIndex];
    if (entry.type == DockEntryType::FolderMapping)
    {
        int sysIconIndex = -1;
        const std::wstring iconCacheKey =
            ToUpperInvariant(entry.reference);
        if (const auto cached =
                dockFolderIconIndexCache_.find(
                    iconCacheKey);
            cached !=
                dockFolderIconIndexCache_.end())
        {
            sysIconIndex = cached->second;
        }
        else
        {
            SHFILEINFOW info{};
            if (!widget.sourceFolderPath.empty() &&
                SHGetFileInfoW(
                    widget.sourceFolderPath.c_str(), 0,
                    &info, sizeof(info),
                    SHGFI_SYSICONINDEX) != 0)
                sysIconIndex = info.iIcon;
            dockFolderIconIndexCache_.emplace(
                iconCacheKey, sysIconIndex);
        }
        DrawPlaceholderIcon(
            ctx, sysIconIndex, iconRect, 1.0f, true);
        if (ShouldDrawShortcutArrow(true, false))
            DrawShortcutArrowOverlay(
                ctx, iconRect, 1.0f);
        if (state == 2)
            DrawDockSelectionIndicator(
                ctx, iconRect, lt);
        return;
    }
    const auto collectionLayout =
        snowdesktop::dock_collection_icon_rules::
            CalculateLayout(iconRect);
    DrawDockControlBackground(
        ctx, collectionLayout.background,
        0, !lt);
    for (size_t i = 0; i < std::min<size_t>(4, widget.itemKeys.size()); ++i)
    {
        size_t itemIndex = FindItemIndexByKey(widget.itemKeys[i]);
        if (itemIndex >= items_.size()) continue;
        int col = static_cast<int>(i % 2);
        int row = static_cast<int>(i / 2);
        const RECT cell =
            snowdesktop::dock_collection_icon_rules::
                CellRect(
                    collectionLayout,
                    col, row);
        drawDesktopItem(items_[itemIndex], cell);
    }
    if (state == 2)
        DrawDockSelectionIndicator(ctx, iconRect, lt);
}

inline void DesktopApp::DrawDockRunningApp(ID2D1DeviceContext* ctx,
    const DockRunningAppInfo& app, RECT rect, int state)
{
    if (!ctx) return;
    const int scaledSpacing = std::max(1, static_cast<int>(std::round(
        kDockSpacing * ClampDockScale(dockSettings_.thicknessScale))));
    const int iconSize = std::max(1, static_cast<int>(std::min(
        rect.right - rect.left, rect.bottom - rect.top)) - scaledSpacing);
    RECT iconRect{
        rect.left + (rect.right - rect.left - iconSize) / 2,
        rect.top + (rect.bottom - rect.top - iconSize) / 2,
        rect.left + (rect.right - rect.left + iconSize) / 2,
        rect.top + (rect.bottom - rect.top + iconSize) / 2
    };
    const bool lt = IsLightContentTheme();
    if (ID2D1Bitmap1* bitmap = GetOrCreateD2DBitmap(app.iconBitmap))
        ctx->DrawBitmap(bitmap, ToD2DRect(iconRect), 1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR);
    else
        DrawPlaceholderIcon(ctx, -1, iconRect, 1.0f, true);
    if (state == 2)
    {
        DrawDockSelectionIndicator(ctx, iconRect, lt);
        return;
    }

    const bool verticalDock = dockSettings_.position == DockPosition::Left ||
        dockSettings_.position == DockPosition::Right;
    const float dotRadius = std::max(1.7f, iconSize * 0.038f);
    const float indicatorGap = std::max(3.0f, iconSize * 0.06f);
    D2D1_POINT_2F center{};
    switch (dockSettings_.position)
    {
    case DockPosition::Top:
        center = D2D1::Point2F((rect.left + rect.right) * 0.5f,
            static_cast<float>(iconRect.top) - indicatorGap);
        break;
    case DockPosition::Left:
        center = D2D1::Point2F(static_cast<float>(iconRect.left) - indicatorGap,
            (rect.top + rect.bottom) * 0.5f);
        break;
    case DockPosition::Right:
        center = D2D1::Point2F(static_cast<float>(iconRect.right) + indicatorGap,
            (rect.top + rect.bottom) * 0.5f);
        break;
    case DockPosition::Bottom:
    default:
        center = D2D1::Point2F((rect.left + rect.right) * 0.5f,
            static_cast<float>(iconRect.bottom) + indicatorGap);
        break;
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    const auto indicator =
        snowdesktop::dock_window_rules::
            ResolveDockRunningIndicatorColor(
                lt, app.foreground, app.minimized);
    const D2D1_COLOR_F color =
        D2D1::ColorF(
            indicator.red,
            indicator.green,
            indicator.blue,
            indicator.alpha);
    if (FAILED(ctx->CreateSolidColorBrush(color, &brush)) || !brush) return;
    if (app.minimized)
    {
        ctx->FillEllipse(D2D1::Ellipse(center, dotRadius, dotRadius), brush.Get());
        return;
    }

    const float longHalf = app.foreground
        ? std::max(6.0f, iconSize * 0.14f)
        : std::max(4.0f, iconSize * 0.09f);
    const float shortHalf = app.foreground
        ? std::max(1.45f, iconSize * 0.031f)
        : std::max(1.2f, iconSize * 0.026f);
    const D2D1_RECT_F bar = verticalDock
        ? D2D1::RectF(center.x - shortHalf, center.y - longHalf,
            center.x + shortHalf, center.y + longHalf)
        : D2D1::RectF(center.x - longHalf, center.y - shortHalf,
            center.x + longHalf, center.y + shortHalf);
    ctx->FillRoundedRectangle(
        D2D1::RoundedRect(bar, shortHalf, shortHalf), brush.Get());
}
