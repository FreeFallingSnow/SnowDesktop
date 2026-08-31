#include "app.h"
#include "dock_platform_helpers.h"

// Dock launch animation, item activation and application identity resolution.

bool DesktopApp::StartDockLaunchBounce(size_t itemIndex)
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
    EnsureUiAnimationFrame();
    InvalidateDockLaunchBounceRects();
    return true;
}

float DesktopApp::GetDockLaunchBounceOffset(
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

void DesktopApp::OnDockLaunchBounceTimer()
{
    if (dockLaunchBounces_.empty())
    {
        return;
    }

    const double now =
        snowdesktop::dock_launch_animation::
            MonotonicTimeMilliseconds();

    // Include the previous frame before removing completed bounces so their
    // last translated pixels are cleared by the same coalesced paint.
    InvalidateDockLaunchBounceRects();

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

    InvalidateDockLaunchBounceRects();
}

void DesktopApp::InvalidateDockLaunchBounceRects()
{
    bool invalidateFloatingDock = false;
    for (const auto& [key, _] : dockLaunchBounces_)
    {
        const size_t itemIndex = FindItemIndexByKey(key);
        if (itemIndex >= items_.size())
            continue;
        for (const auto& container : containers_)
        {
            auto* dock = dynamic_cast<DockContainer*>(
                container.get());
            if (!dock)
                continue;
            RECT dirty = dock->GetDesktopItemVisualRect(
                itemIndex, lastMousePoint_);
            if (IsRectEmptyRect(dirty))
                continue;
            const int shortSide = std::max<LONG>(
                1,
                std::min(
                    dirty.right - dirty.left,
                    dirty.bottom - dirty.top));
            const int padding = std::max(
                4,
                static_cast<int>(std::ceil(
                    static_cast<double>(shortSide) * 0.42)));
            InflateRect(&dirty, padding, padding);
            if (IsDockHostedByPersistentHost(dock))
            {
                invalidateFloatingDock = true;
            }
            else if (hwnd_ && IsWindow(hwnd_))
            {
                InvalidateRect(hwnd_, &dirty, FALSE);
            }
        }
    }
    if (invalidateFloatingDock)
        InvalidateFloatingDockWindow(false);
}

bool DesktopApp::LaunchDesktopItem(
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
    const DesktopItem& item = items_[itemIndex];
    const wchar_t* extension =
        PathFindExtensionW(item.parsingName.c_str());
    const bool useShellItemActivation = item.isShortcut ||
        (extension &&
            (_wcsicmp(extension, L".lnk") == 0 ||
                _wcsicmp(extension, L".url") == 0));
    // Desktop activation is direct user input. Invoke the Shell item's Open
    // command on the UI STA, matching the context-menu path and preserving
    // the input thread's foreground/DDE handoff. A single background launch
    // queue can otherwise leave later document opens behind a blocked legacy
    // handler on Windows 10. Dock launches keep their existing isolation.
    const bool launchAccepted =
        !animateDockLaunch && item.absolutePidl.get()
        ? snowdesktop::ShellLaunchWorker::ExecuteInteractive(
            hwnd_, item.parsingName, item.absolutePidl.get())
        : useShellItemActivation && item.absolutePidl.get()
            ? shellLaunchWorker_.EnqueueShellItem(
                hwnd_, item.parsingName, item.absolutePidl.get())
            : shellLaunchWorker_.Enqueue(
                hwnd_, item.parsingName);
    if (!launchAccepted)
        return false;
    RecordDockItemUsage(itemIndex);
    if (animateDockLaunch && wasClosed)
        StartDockLaunchBounce(itemIndex);
    return true;
}

DockAppIdentity DesktopApp::ResolveDockAppIdentity(size_t itemIndex)
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
                            else if (IsApplicationsShellLinkTarget(
                                    shellLink.Get(), item.parsingName))
                            {
                                identity.kind = DockAppIdentityKind::Applications;
                                if (identity.appUserModelId.empty())
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
