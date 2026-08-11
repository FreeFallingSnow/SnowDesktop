#pragma once

#include <cmath>
#include <ctime>
#include <limits>
#include <numeric>

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
    int requestedSize,
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
        pidl, fallbackIndex, bitmapSize, false, requestedSize, true);
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
    HWND window, SIZE& bitmapSize, int requestedSize)
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
        icon, requestedSize, requestedSize,
        bitmapSize);
}

inline HBITMAP CreateDockWindowIconBitmap(
    HWND window, const std::wstring& executablePath,
    const std::wstring& appUserModelId, SIZE& bitmapSize,
    int requestedSize)
{
    // Prefer stable high-resolution application identity icons. Some classic
    // Win32 hosts (for example Creo's xtop.exe) expose only the generic
    // executable icon through the Shell, so that result is allowed to fall
    // through to the window-provided icon below.
    if (!appUserModelId.empty())
    {
        if (HBITMAP bitmap = CreateDockShellIconBitmap(
                L"shell:AppsFolder\\" + appUserModelId, bitmapSize,
                requestedSize))
            return bitmap;
    }

    int executableIconIndex = -1;
    SIZE executableBitmapSize{};
    HBITMAP executableBitmap =
        CreateDockShellIconBitmap(
            executablePath, executableBitmapSize, requestedSize,
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
            window, windowBitmapSize, requestedSize);
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
    if (!snowdesktop::dock_window_rules::
            IsTaskWindowPresentationEligible(
                IsWindowVisible(window) != FALSE,
                IsIconic(window) != FALSE))
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
 * @brief 判断对目标窗口执行同步激活是否安全。
 *
 * BringWindowToTop / SwitchToThisWindow 通过 SetWindowPos 同步等待目标
 * 进程的窗口线程处理；目标进程挂起（例如求解器无法泵消息）时该调用会在
 * 内核 NtUserSetWindowPos 上无限阻塞 UI 线程，导致整个软件卡死。此时只能
 * 依赖异步的 ShowWindowAsync 与不等待目标线程的 SetForegroundWindow。
 */
inline bool ShouldSkipSynchronousWindowActivation(HWND window)
{
    if (!window || !IsWindow(window))
        return true;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root)
        root = window;
    return IsHungAppWindow(root) != FALSE;
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
    const std::wstring& identityKey)
{
    if (identityKey.empty())
        return {};
    return identityKey;
}

inline UINT QueryDockWindowPreviewHoverTime()
{
    UINT hoverTime = kDockWindowPreviewHoverFallbackMs;
    if (!SystemParametersInfoW(
            SPI_GETMOUSEHOVERTIME, 0, &hoverTime, 0))
        hoverTime = kDockWindowPreviewHoverFallbackMs;
    return std::max<UINT>(1, hoverTime);
}
