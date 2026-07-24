#pragma once

#include <ctime>
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
    const std::wstring& parsingName, SIZE& bitmapSize)
{
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

    HBITMAP bitmap = GetHighResolutionShellIconBitmap(
        pidl, fallbackIndex, bitmapSize, false);
    CoTaskMemFree(pidl);
    return bitmap;
}

inline HBITMAP CreateDockWindowIconBitmap(
    HWND window, const std::wstring& executablePath,
    const std::wstring& appUserModelId, SIZE& bitmapSize)
{
    // The window icon often describes the current document or Explorer
    // location (for example "This PC") and is commonly only 16/32 px. Resolve
    // the application identity through the Shell first so the running area
    // receives the stable high-resolution application icon.
    if (!appUserModelId.empty())
    {
        if (HBITMAP bitmap = CreateDockShellIconBitmap(
                L"shell:AppsFolder\\" + appUserModelId, bitmapSize))
            return bitmap;
    }
    if (HBITMAP bitmap = CreateDockShellIconBitmap(executablePath, bitmapSize))
        return bitmap;

    HICON icon = nullptr;
    DWORD_PTR iconResult = 0;
    if (SendMessageTimeoutW(window, WM_GETICON, ICON_BIG, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 80, &iconResult))
        icon = reinterpret_cast<HICON>(iconResult);
    if (!icon && SendMessageTimeoutW(window, WM_GETICON, ICON_SMALL, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 80, &iconResult))
        icon = reinterpret_cast<HICON>(iconResult);
    if (!icon)
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
    if (!icon)
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
    if (icon)
        return CreateAlphaBitmapFromIcon(
            icon, kIconBitmapSize, kIconBitmapSize, bitmapSize);

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
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 && (exStyle & WS_EX_APPWINDOW) == 0)
        return false;
    if (GetWindow(window, GW_OWNER) && (exStyle & WS_EX_APPWINDOW) == 0)
        return false;
    return true;
}

inline std::wstring DockItemWindowKey(const DesktopItem& item)
{
    return ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
}

inline bool IsDockDesktopActivationWindow(HWND window)
{
    if (!window) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == GetCurrentProcessId()) return true;
    wchar_t className[64]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return _wcsicmp(className, L"Progman") == 0 ||
        _wcsicmp(className, L"WorkerW") == 0 ||
        _wcsicmp(className, L"Shell_TrayWnd") == 0;
}

inline void CALLBACK DesktopApp::DockForegroundWinEventProc(HWINEVENTHOOK,
    DWORD event, HWND window, LONG, LONG, DWORD, DWORD)
{
    if (event != EVENT_SYSTEM_FOREGROUND || !window) return;
    const HWND previous = dockForegroundWindow_.exchange(window);
    if (previous != window)
    {
        dockPreviousForegroundWindow_.store(previous);
        dockForegroundChangedTick_.store(GetTickCount());
    }
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
}

inline void DesktopApp::StopDockForegroundMonitor()
{
    if (dockForegroundEventHook_)
    {
        UnhookWinEvent(dockForegroundEventHook_);
        dockForegroundEventHook_ = nullptr;
    }
    dockForegroundWindow_.store(nullptr);
    dockPreviousForegroundWindow_.store(nullptr);
    dockForegroundChangedTick_.store(0);
}

inline void DesktopApp::UpdateSystemTaskbarRevealGuard()
{
    if (!generalSettings_.dockEnabled || !dockSettings_.systemTaskbarAutoHide ||
        dockSettings_.edgeAttached ||
        dockSettings_.position != DockPosition::Bottom ||
        !IsSystemTaskbarAutoHideEnabled())
        return;

    APPBARDATA taskbarPosition{};
    taskbarPosition.cbSize = sizeof(taskbarPosition);
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &taskbarPosition) ||
        taskbarPosition.uEdge != ABE_BOTTOM)
        return;

    if (!hwnd_) return;

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
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
        if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) continue;
        if (cursor.x < candidate.left || cursor.x >= candidate.right ||
            cursor.y < candidate.top || cursor.y >= monitorInfo.rcMonitor.bottom)
            continue;
        screen = monitorInfo.rcMonitor;
        foundProtectedDock = true;
        break;
    }
    if (!foundProtectedDock) return;

    constexpr int kRevealGuardPixels = 6;
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
        return entry.type == DockEntryType::Collection && entry.reference == id;
    });
}

inline bool DesktopApp::IsRecycleBinDockEntry(const DockEntry& entry) const
{
    return entry.type == DockEntryType::DesktopItem &&
        _wcsicmp(entry.reference.c_str(), kDesktopIconClsidRecycleBin) == 0;
}

inline void DesktopApp::NormalizeDockRecycleBinPosition()
{
    std::stable_partition(dockEntries_.begin(), dockEntries_.end(),
        [this](const DockEntry& entry) { return !IsRecycleBinDockEntry(entry); });
}

inline void DesktopApp::LoadDockUsageStats()
{
    dockUsageStats_.clear();
    std::ifstream file(GetDataFilePath(L"SnowDesktop.dock-usage.json"), std::ios::binary);
    if (!file) return;
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string text = stream.str();

    const size_t entriesName = text.find("\"entries\"");
    const size_t arrayStart = entriesName == std::string::npos
        ? std::string::npos : text.find('[', entriesName);
    const size_t arrayEnd = arrayStart == std::string::npos
        ? std::string::npos : FindJsonArrayEnd(text, arrayStart);
    size_t position = arrayStart == std::string::npos ? 0 : arrayStart + 1;
    while (arrayEnd != std::string::npos &&
        (position = text.find('{', position)) != std::string::npos && position < arrayEnd)
    {
        const size_t objectEnd = FindJsonObjectEnd(text, position);
        if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;
        const std::string object = text.substr(position, objectEnd - position + 1);
        std::string keyUtf8;
        int launchCount = 0;
        int lastUsed = 0;
        if (ReadJsonStringField(object, "key", keyUtf8) &&
            ReadJsonIntField(object, "launchCount", launchCount) && launchCount > 0)
        {
            ReadJsonIntField(object, "lastUsed", lastUsed);
            const std::wstring key = ToUpperInvariant(Utf8ToWide(keyUtf8));
            if (!key.empty())
                dockUsageStats_[key] = { launchCount, std::max(0, lastUsed) };
        }
        position = objectEnd + 1;
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
        const bool isShownAsRunning = dockSettings_.showRunningApps &&
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

inline bool DesktopApp::LaunchDesktopItem(size_t itemIndex)
{
    if (itemIndex >= items_.size() || items_[itemIndex].parsingName.empty())
        return false;
    HINSTANCE result = ShellExecuteW(hwnd_, L"open", items_[itemIndex].parsingName.c_str(),
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
        return false;
    RecordDockItemUsage(itemIndex);
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
        if (found->second.minimized)
            return DockWindowVisualState::Minimized;
        if (found->second.foreground)
            return DockWindowVisualState::Foreground;
    }
    return DockWindowVisualState::Running;
}

inline void DesktopApp::RefreshDockRunningWindows(
    bool invalidateChanged, HWND preferredWindow)
{
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
        std::unordered_map<DWORD, std::wstring> processPaths;
    } context{ &targets, scoringForeground, actualForeground, &fixedIdentities,
        &runningCandidates, &runningCandidateIndices };

    if (generalSettings_.dockEnabled)
    {
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto* context = reinterpret_cast<EnumContext*>(parameter);
            if (!IsDockTaskWindow(window)) return TRUE;

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

    if (!dockSettings_.showRunningApps)
        runningCandidates.clear();

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
}

inline bool DesktopApp::ActivateOrToggleDockItem(size_t itemIndex)
{
    if (itemIndex >= items_.size()) return false;
    const HWND pressedForeground = dockPressedForegroundWindow_;
    dockPressedForegroundWindow_ = nullptr;
    dockPressedForegroundTick_ = 0;
    const DockAppIdentity identity = ResolveDockAppIdentity(itemIndex);
    if (identity.kind == DockAppIdentityKind::None)
        return LaunchDesktopItem(itemIndex);

    const HWND foreground = GetForegroundWindow();
    const HWND observedForeground = dockForegroundWindow_.load();
    const HWND previousForeground = dockPreviousForegroundWindow_.load();
    const DWORD foregroundElapsed = GetTickCount() - dockForegroundChangedTick_.load();
    const DWORD activationGrace = std::max<DWORD>(1000, GetDoubleClickTime() * 2);
    HWND preferredWindow = nullptr;
    auto choosePreferredWindow = [&](HWND candidate) {
        if (!preferredWindow && DockWindowMatchesAppIdentity(candidate, identity))
            preferredWindow = GetAncestor(candidate, GA_ROOT);
    };
    choosePreferredWindow(pressedForeground);
    choosePreferredWindow(foreground);
    choosePreferredWindow(observedForeground);
    if (foregroundElapsed <= activationGrace)
        choosePreferredWindow(previousForeground);

    // Refresh must rank the window that was in front before the Dock click highest.
    // Otherwise a multi-window application may activate a sibling window first and
    // only minimize on the next click.
    RefreshDockRunningWindows(false, preferredWindow);
    const std::wstring key = DockItemWindowKey(items_[itemIndex]);
    auto found = dockRunningWindows_.find(key);
    if (found == dockRunningWindows_.end() || !IsWindow(found->second.window))
        return LaunchDesktopItem(itemIndex);

    HWND target = found->second.window;
    if (preferredWindow && IsWindow(preferredWindow))
    {
        target = preferredWindow;
        found->second.window = target;
        found->second.minimized = IsIconic(target) != FALSE;
    }

    const bool targetWasForeground = found->second.foreground ||
        DockWindowsShareActivationGroup(target, pressedForeground) ||
        DockWindowsShareActivationGroup(target, foreground) ||
        DockWindowsShareActivationGroup(target, observedForeground) ||
        (foregroundElapsed <= activationGrace &&
            DockWindowsShareActivationGroup(target, previousForeground));

    // Match Windows taskbar behavior: a background window is activated first;
    // clicking it again while it is foreground minimizes it.
    if (targetWasForeground && !IsIconic(target))
    {
        PostMessageW(target, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        ShowWindowAsync(target, SW_MINIMIZE);
        found->second.minimized = true;
        found->second.foreground = false;
    }
    else
    {
        const bool minimized = IsIconic(target) != FALSE;
        if (minimized)
        {
            OpenIcon(target);
            PostMessageW(target, WM_SYSCOMMAND, SC_RESTORE, 0);
            ShowWindowAsync(target, SW_RESTORE);
        }
        else
        {
            ShowWindowAsync(target, SW_SHOW);
        }
        const HWND activationTarget = GetLastActivePopup(target);
        SwitchToThisWindow(target, TRUE);
        if (activationTarget != target)
            SwitchToThisWindow(activationTarget, TRUE);
        BringWindowToTop(activationTarget);
        SetForegroundWindow(activationTarget);
        found->second.minimized = false;
        found->second.foreground = true;
    }

    InvalidateDockRects();
    return true;
}

inline bool DesktopApp::ActivateOrToggleDockWindow(HWND window)
{
    if (!window || !IsWindow(window)) return false;
    HWND target = GetAncestor(window, GA_ROOT);
    if (!target) target = window;

    const HWND pressedForeground = dockPressedForegroundWindow_;
    dockPressedForegroundWindow_ = nullptr;
    dockPressedForegroundTick_ = 0;
    const HWND foreground = GetForegroundWindow();
    const HWND observedForeground = dockForegroundWindow_.load();
    const HWND previousForeground = dockPreviousForegroundWindow_.load();
    const DWORD elapsed = GetTickCount() - dockForegroundChangedTick_.load();
    const DWORD activationGrace = std::max<DWORD>(1000, GetDoubleClickTime() * 2);
    const bool wasForeground =
        DockWindowsShareActivationGroup(target, pressedForeground) ||
        DockWindowsShareActivationGroup(target, foreground) ||
        DockWindowsShareActivationGroup(target, observedForeground) ||
        (elapsed <= activationGrace &&
            DockWindowsShareActivationGroup(target, previousForeground));

    const bool minimized = IsIconic(target) != FALSE;
    bool nowMinimized = false;
    bool nowForeground = false;
    if (wasForeground && !minimized)
    {
        PostMessageW(target, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        ShowWindowAsync(target, SW_MINIMIZE);
        nowMinimized = true;
    }
    else
    {
        if (minimized)
        {
            OpenIcon(target);
            PostMessageW(target, WM_SYSCOMMAND, SC_RESTORE, 0);
            ShowWindowAsync(target, SW_RESTORE);
        }
        else
        {
            ShowWindowAsync(target, SW_SHOW);
        }
        const HWND activationTarget = GetLastActivePopup(target);
        SwitchToThisWindow(target, TRUE);
        if (activationTarget != target)
            SwitchToThisWindow(activationTarget, TRUE);
        BringWindowToTop(activationTarget);
        SetForegroundWindow(activationTarget);
        nowForeground = true;
    }

    for (DockRunningAppInfo& app : dockUnpinnedRunningApps_)
    {
        const bool matchesTarget = DockWindowsShareActivationGroup(app.window, target);
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
        const RECT bounds = dock->GetBounds();
        if (PtInRect(&bounds, point)) return dock;
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
    if (!hwnd_) return;
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        const RECT bounds = dock->GetBounds();
        InvalidateRect(hwnd_, &bounds, erase);
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
    for (const RECT& dockArea : dockAreas_)
    {
        for (auto& page : gridPages_)
        {
            RECT intersect;
            if (!IntersectRect(&intersect, &dockArea, &page.bounds)) continue;
            int reserved;
            switch (dockSettings_.position)
            {
            case DockPosition::Top:
                reserved = dockArea.bottom - dockArea.top;
                page.workArea.top = std::max(page.bounds.top, page.workArea.top - reserved);
                break;
            case DockPosition::Bottom:
                reserved = dockArea.bottom - dockArea.top;
                page.workArea.bottom = std::min(page.bounds.bottom, page.workArea.bottom + reserved);
                break;
            case DockPosition::Left:
                reserved = dockArea.right - dockArea.left;
                page.workArea.left = std::max(page.bounds.left, page.workArea.left - reserved);
                break;
            case DockPosition::Right:
                reserved = dockArea.right - dockArea.left;
                page.workArea.right = std::min(page.bounds.right, page.workArea.right + reserved);
                break;
            }
            break;
        }
    }

    dockAreas_.clear();
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
            const int edgeDistance = std::max(scaledSpacing, componentMargin);
            const int innerGap = edgeDistance - componentMargin;
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
        for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        {
            if (*it < insertIndex) --insertIndex;
            dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        auto recycleBin = std::find_if(dockEntries_.begin(), dockEntries_.end(),
            [this](const DockEntry& entry) { return IsRecycleBinDockEntry(entry); });
        insertIndex = std::min(insertIndex,
            static_cast<size_t>(std::distance(dockEntries_.begin(), recycleBin)));
        dockEntries_.insert(dockEntries_.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            moving.begin(), moving.end());
        NormalizeDockRecycleBinPosition();
        InvalidateDockContainers();
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
            if (addition.type == DockEntryType::Collection)
            {
                existing->keepOnDesktop = false;
                size_t widgetIndex = FindWidgetIndexById(addition.reference);
                if (widgetIndex < widgets_.size())
                    widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
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
            if (widgetIndex < widgets_.size()) widgets_[widgetIndex].gridCell = { kDockPageId, 0, 0 };
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
        if (upper.empty()) continue;
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

    std::vector<DockEntry> moving;
    for (size_t index : indices)
        if (index < dockEntries_.size()) moving.push_back(dockEntries_[index]);

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& widget : widgets_)
        if (widget.gridCell.pageId != kDockPageId)
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);

    const GridPage* targetPage = FindGridPage(gridPages_, targetCell.pageId);
    int startSlot = targetPage ? SlotFromCell(gridPages_, targetCell) : 0;
    for (const DockEntry& entry : moving)
    {
        if (entry.keepOnDesktop) continue;
        GridSpan span{ 1, 1 };
        if (entry.type == DockEntryType::Collection)
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) span = widgets_[widgetIndex].gridSpan;
        }
        GridCell freeCell;
        if (!FindDockReturnCell(usedSlots, targetCell.pageId, startSlot, span, freeCell))
            continue;
        MarkGridArea(usedSlots, freeCell, span);
        ++startSlot;
        if (entry.type == DockEntryType::DesktopItem)
        {
            size_t itemIndex = FindItemIndexByKey(entry.reference);
            if (itemIndex < items_.size()) items_[itemIndex].gridCell = freeCell;
        }
        else
        {
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex < widgets_.size()) widgets_[widgetIndex].gridCell = freeCell;
        }
    }

    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        dockEntries_.erase(dockEntries_.begin() + static_cast<std::ptrdiff_t>(*it));
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
        if (widget.gridCell.pageId != kDockPageId)
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);

    int startSlot = 0;
    for (const DockEntry& entry : dockEntries_)
    {
        if (entry.keepOnDesktop) continue;
        GridSpan span{ 1, 1 };
        if (entry.type == DockEntryType::Collection)
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
    bool recycleBinEntry = false;
    if (entry.type == DockEntryType::DesktopItem)
    {
        const size_t itemIndex = FindItemIndexByKey(entry.reference);
        recycleBinEntry = itemIndex < items_.size() &&
            _wcsicmp(items_[itemIndex].desktopIconClsid.c_str(),
                kDesktopIconClsidRecycleBin) == 0;
    }

    const bool lt = IsLightContentTheme();

    // Hover/selected feedback belongs to the whole Dock slot, not just the bitmap.
    // Keep a small inset so adjacent items remain visually separated.
    if (state > 0 && !recycleBinEntry)
    {
        RECT feedbackRect = rect;
        InflateRect(&feedbackRect, -2, -2);
        DrawD2DRoundedRectangle(ctx, feedbackRect, 12.0f,
            state == 2
                ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.28f)
                : (lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f)),
            state == 2
                ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.82f)
                : (lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f)),
            state == 2 ? 1.6f : 1.0f);
    }

    auto drawDesktopItem = [&](const DesktopItem& item, RECT target, int visualState) {
        RECT bitmapTarget = target;
        const bool recycleBin = _wcsicmp(item.desktopIconClsid.c_str(),
            kDesktopIconClsidRecycleBin) == 0;
        if (recycleBin)
        {
            DrawDockControlBackground(ctx, target, visualState, !lt);
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
        drawDesktopItem(items_[index], iconRect, state);

        const DockWindowVisualState windowState = GetDockWindowVisualState(index);
        if (windowState != DockWindowVisualState::Closed)
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

            ComPtr<ID2D1SolidColorBrush> indicatorBrush;
            const D2D1_COLOR_F indicatorColor = lt
                ? (foreground
                    ? D2D1::ColorF(0.14f, 0.16f, 0.22f, 1.0f)
                    : D2D1::ColorF(0.24f, 0.26f, 0.32f, minimized ? 0.82f : 0.90f))
                : (foreground
                    ? D2D1::ColorF(0.86f, 0.88f, 0.92f, 1.0f)
                    : D2D1::ColorF(0.72f, 0.75f, 0.80f, minimized ? 0.82f : 0.90f));
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
    const int innerSize = std::max(1, static_cast<int>(
        std::min(iconRect.right - iconRect.left, iconRect.bottom - iconRect.top)));
    const int collectionGap = std::clamp(static_cast<int>(std::round(
        innerSize * 0.04f)), 2, 4);
    const int smallIconSize = std::max(1, (innerSize - collectionGap) / 2);
    const int groupSize = smallIconSize * 2 + collectionGap;
    const int groupLeft = iconRect.left + (innerSize - groupSize) / 2;
    const int groupTop = iconRect.top + (innerSize - groupSize) / 2;
    for (size_t i = 0; i < std::min<size_t>(4, widget.itemKeys.size()); ++i)
    {
        size_t itemIndex = FindItemIndexByKey(widget.itemKeys[i]);
        if (itemIndex >= items_.size()) continue;
        int col = static_cast<int>(i % 2);
        int row = static_cast<int>(i / 2);
        const int left = groupLeft + col * (smallIconSize + collectionGap);
        const int top = groupTop + row * (smallIconSize + collectionGap);
        RECT cell{ left, top, left + smallIconSize, top + smallIconSize };
        drawDesktopItem(items_[itemIndex], cell, 0);
    }
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
    if (state > 0)
    {
        RECT feedbackRect = rect;
        InflateRect(&feedbackRect, -2, -2);
        DrawD2DRoundedRectangle(ctx, feedbackRect, 12.0f,
            state == 2
                ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.28f)
                : (lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f)),
            state == 2
                ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.82f)
                : (lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f)),
            state == 2 ? 1.6f : 1.0f);
    }
    if (ID2D1Bitmap1* bitmap = GetOrCreateD2DBitmap(app.iconBitmap))
        ctx->DrawBitmap(bitmap, ToD2DRect(iconRect), 1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR);
    else
        DrawPlaceholderIcon(ctx, -1, iconRect, 1.0f, true);

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
    const D2D1_COLOR_F color = lt
        ? (app.foreground
            ? D2D1::ColorF(0.14f, 0.16f, 0.22f, 1.0f)
            : D2D1::ColorF(0.24f, 0.26f, 0.32f, app.minimized ? 0.82f : 0.90f))
        : (app.foreground
            ? D2D1::ColorF(0.86f, 0.88f, 0.92f, 1.0f)
            : D2D1::ColorF(0.72f, 0.75f, 0.80f, app.minimized ? 0.82f : 0.90f));
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
