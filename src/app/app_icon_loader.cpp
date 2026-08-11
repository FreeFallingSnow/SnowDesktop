#include "app.h"
#include "../shortcut_application_rules.h"

// Asynchronous icon-loading lifecycle.

namespace
{
void ClampAlphaToColorKey(HBITMAP bitmap, COLORREF key)
{
    if (!bitmap) return;
    BITMAP bm{};
    if (GetObjectW(bitmap, sizeof(bm), &bm) == 0 || bm.bmBitsPixel != 32 || !bm.bmBits) return;
    const int width = bm.bmWidth;
    const int height = std::abs(bm.bmHeight);
    auto* pixels = static_cast<std::uint32_t*>(bm.bmBits);
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < count; ++i)
    {
        const uint8_t alpha = (pixels[i] >> 24) & 0xff;
        const uint8_t red = (pixels[i] >> 16) & 0xff;
        const uint8_t green = (pixels[i] >> 8) & 0xff;
        const uint8_t blue = pixels[i] & 0xff;
        if (alpha < 250 && (int(red) + int(green) + int(blue)) < 150)
            pixels[i] = 0;
    }
    (void)key;
}
}

void DesktopApp::StartIconLoader()
{
    iconLoaderRunning_ = true;
    iconLoaderThread_ = std::thread([this]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        while (true) {
            IconLoadTask task;
            {
                std::unique_lock<std::mutex> lock(iconLoaderMutex_);
                iconLoaderCv_.wait(lock, [this] { return !iconLoaderQueue_.empty() || !iconLoaderRunning_; });
                if (!iconLoaderRunning_) break;
                if (iconLoaderQueue_.empty()) continue;
                task = std::move(iconLoaderQueue_.front());
                iconLoaderQueue_.pop_front();
            }
            if (task.absolutePidl.get() == nullptr)
            {
                std::lock_guard<std::mutex> lock(iconLoaderMutex_);
                iconLoaderPendingKeys_.erase(task.requestKey);
                continue;
            }

            SIZE bitmapSize{};
            const std::wstring_view representationName =
                !task.parsingName.empty()
                ? std::wstring_view(task.parsingName)
                : std::wstring_view(task.folderPath);
            const bool nameLooksApplicationLike =
                snowdesktop::shortcut_application_rules::
                    ShouldUseShellIconOnly(representationName);
            bool shellFolder = false;
            if (task.phase == IconLoadPhase::Phase2)
            {
                ComPtr<IShellItem> shellItem;
                if (SUCCEEDED(SHCreateItemFromIDList(
                        task.absolutePidl.get(), IID_PPV_ARGS(&shellItem))) &&
                    shellItem)
                {
                    SFGAOF attributes = 0;
                    if (SUCCEEDED(shellItem->GetAttributes(
                            SFGAO_FOLDER, &attributes)))
                        shellFolder = (attributes & SFGAO_FOLDER) != 0;
                }
            }
            const bool allowThumbnail =
                snowdesktop::icon_render_rules::ShouldRequestShellThumbnail(
                    task.phase == IconLoadPhase::Phase2,
                    nameLooksApplicationLike, shellFolder);
            const bool preferDirectIconExtraction =
                nameLooksApplicationLike && !shellFolder;
            const bool forShortcut =
                snowdesktop::shortcut_application_rules::HasExtension(
                    representationName, L".lnk") ||
                snowdesktop::shortcut_application_rules::HasExtension(
                    representationName, L".url");
            bool iconIsThumbnail = false;
            HBITMAP bitmap = GetHighResolutionShellIconBitmap(
                task.absolutePidl.get(), task.sysIconIndex, bitmapSize,
                allowThumbnail, task.requestedSize,
                preferDirectIconExtraction,
                forShortcut, representationName, &iconIsThumbnail);
            if (task.phase == IconLoadPhase::Phase1 && bitmap)
                ClampAlphaToColorKey(bitmap, kTransparentKey);

            bool isShortcut = false;
            bool isApplicationShortcut = false;
            if (task.phase == IconLoadPhase::Phase1)
            {
                namespace shortcutRules =
                    snowdesktop::shortcut_application_rules;
                const bool isLnk = shortcutRules::HasExtension(
                    task.parsingName, L".lnk");
                const bool isUrl = shortcutRules::HasExtension(
                    task.parsingName, L".url");
                isShortcut = isLnk || isUrl;
                if (isLnk)
                {
                    wchar_t lnkPath[32768]{};
                    if (SHGetPathFromIDListW(task.absolutePidl.get(), lnkPath))
                    {
                        ComPtr<IShellLinkW> shellLink;
                        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                            IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                        {
                            ComPtr<IPersistFile> persistFile;
                            if (SUCCEEDED(shellLink.As(&persistFile)) &&
                                SUCCEEDED(persistFile->Load(lnkPath, STGM_READ)))
                            {
                                if (!IsApplicationsShellLinkTarget(
                                        shellLink.Get(), lnkPath))
                                {
                                    wchar_t target[32768]{};
                                    if (SUCCEEDED(shellLink->GetPath(
                                            target, static_cast<int>(std::size(target)),
                                            nullptr, 0)) &&
                                        target[0] != L'\0')
                                    {
                                        isApplicationShortcut =
                                            shortcutRules::HasExtension(
                                                target, L".exe");
                                    }
                                }
                                else
                                {
                                    isApplicationShortcut = true;
                                }
                            }
                        }
                    }
                }
                else if (isUrl)
                {
                    wchar_t url[32768]{};
                    GetPrivateProfileStringW(
                        L"InternetShortcut", L"URL", L"", url,
                        static_cast<DWORD>(std::size(url)),
                        task.parsingName.c_str());
                    isApplicationShortcut =
                        shortcutRules::IsSteamApplicationUrl(url);
                }
            }

            if (bitmap || task.phase == IconLoadPhase::Phase2)
            {
                auto* result = new IconLoadResult();
                result->serial = task.serial;
                result->requestKey = std::move(task.requestKey);
                result->layoutKey = std::move(task.layoutKey);
                result->widgetId = std::move(task.widgetId);
                result->bitmap = bitmap;
                result->bitmapSize = bitmapSize;
                result->isShortcut = isShortcut;
                result->isApplicationShortcut = isApplicationShortcut;
                result->shortcutArrow = isShortcut && !isApplicationShortcut;
                result->iconIsMediaThumbnail =
                    snowdesktop::icon_render_rules::IsMediaThumbnail(
                        iconIsThumbnail, shellFolder);
                result->phase = task.phase;
                result->isDesktopItem = task.isDesktopItem;
                result->folderPath = std::move(task.folderPath);
                if (!PostMessageW(hwnd_, kIconLoadedMessage, 0, reinterpret_cast<LPARAM>(result)))
                {
                    {
                        std::lock_guard<std::mutex> lock(iconLoaderMutex_);
                        iconLoaderPendingKeys_.erase(result->requestKey);
                    }
                    if (result->bitmap) DeleteObject(result->bitmap);
                    delete result;
                }
            }
            else
            {
                std::lock_guard<std::mutex> lock(iconLoaderMutex_);
                iconLoaderPendingKeys_.erase(task.requestKey);
            }
        }
        CoUninitialize();
    });
}

int DesktopApp::GetShellIconBitmapSizeForPage(
    const std::wstring& pageId) const
{
    const GridPage* selected = nullptr;
    for (const auto& page : gridPages_)
    {
        if (page.id == pageId)
        {
            selected = &page;
            break;
        }
    }
    if (!selected && !gridPages_.empty())
        selected = &gridPages_.front();

    const int targetSize = selected
        ? GetGridPageItemIconSize(*selected)
        : kIconSize;
    return snowdesktop::icon_render_rules::
        SourcePixelsForTarget(targetSize);
}

int DesktopApp::GetMaximumShellIconBitmapSize() const
{
    int targetSize = kIconSize;
    for (const auto& page : gridPages_)
        targetSize = std::max(targetSize,
            GetGridPageItemIconSize(page));
    return snowdesktop::icon_render_rules::
        SourcePixelsForTarget(targetSize);
}

void DesktopApp::RefreshIconBitmapResolution()
{
    const int desktopRequired = GetMaximumShellIconBitmapSize();
    for (auto& item : items_)
    {
        if (item.iconState == IconState::Loading ||
            snowdesktop::icon_render_rules::SourceLongEdgeCoversTarget(
                item.iconBitmapSize.cx, item.iconBitmapSize.cy,
                desktopRequired) || !item.absolutePidl.get())
            continue;

        IconLoadTask task;
        task.serial = iconLoadSerial_;
        task.layoutKey = item.layoutKey;
        task.absolutePidl.reset(ILClone(item.absolutePidl.get()));
        task.sysIconIndex = item.sysIconIndex;
        task.parsingName = item.parsingName;
        task.isDesktopItem = true;
        task.phase = IconLoadPhase::Phase2;
        task.requestedSize = desktopRequired;
        EnqueueIconLoad(std::move(task));
    }

    const auto refreshFolderEntries =
        [&](DesktopWidget& widget, const std::wstring& pageId)
    {
        if (widget.folderEntries.empty())
            return;
        const int required = GetShellIconBitmapSizeForPage(pageId);
        for (auto& entry : widget.folderEntries)
        {
            if (entry.iconState == IconState::Loading ||
                snowdesktop::icon_render_rules::SourceLongEdgeCoversTarget(
                    entry.iconBitmapSize.cx, entry.iconBitmapSize.cy,
                    required))
                continue;

            PIDLIST_ABSOLUTE pidl = nullptr;
            if (FAILED(SHParseDisplayName(entry.fullPath.c_str(), nullptr,
                    &pidl, 0, nullptr)) || !pidl)
                continue;

            IconLoadTask task;
            task.serial = iconLoadSerial_;
            task.widgetId = widget.id;
            task.folderPath = entry.fullPath;
            task.absolutePidl.reset(pidl);
            task.sysIconIndex = entry.sysIconIndex;
            task.isDesktopItem = false;
            task.phase = IconLoadPhase::Phase2;
            task.requestedSize = required;
            EnqueueIconLoad(std::move(task));
        }
    };

    for (auto& widget : widgets_)
    {
        refreshFolderEntries(widget, widget.gridCell.pageId);
    }
    if (dockFolderPopupOpen_)
        refreshFolderEntries(dockFolderPopupWidget_, popupPageId_);
}

void DesktopApp::StopIconLoader()
{
    {
        std::lock_guard<std::mutex> lock(iconLoaderMutex_);
        iconLoaderRunning_ = false;
        iconLoaderQueue_.clear();
        iconLoaderPendingKeys_.clear();
    }
    iconLoaderCv_.notify_all();
    if (iconLoaderThread_.joinable())
        iconLoaderThread_.join();
    if (hwnd_)
    {
        MSG msg{};
        while (PeekMessageW(&msg, hwnd_, kIconLoadedMessage, kIconLoadedMessage, PM_REMOVE))
        {
            auto* result = reinterpret_cast<IconLoadResult*>(msg.lParam);
            if (result)
            {
                if (result->bitmap) DeleteObject(result->bitmap);
                delete result;
            }
        }
    }
}

void DesktopApp::BeginIconLoadGeneration()
{
    std::lock_guard<std::mutex> lock(iconLoaderMutex_);
    ++iconLoadSerial_;
    iconLoaderQueue_.clear();
    iconLoaderPendingKeys_.clear();
}

void DesktopApp::SetSoftwareDesktopEnabled(bool enabled, bool persist)
{
    const bool wasEnabled = customDesktopVisible_;
    if (!enabled)
        EndDesktopPassthroughHold(false);
    customDesktopVisible_ = enabled;
    generalSettings_.softwareDesktopEnabled = enabled;
    if (persist)
        SaveGeneralSettings(GetGeneralSettingsPath().c_str(), generalSettings_);
    if (settingsWindow_)
        settingsWindow_->SyncSoftwareDesktopEnabled(enabled);

    if (!hwnd_ || !IsWindow(hwnd_))
    {
        ApplyDesktopPassthroughHotkey();
        return;
    }

    if (!enabled)
    {
        if (wasEnabled)
        {
            SaveLayoutSlots();
            HideDragHintWindow();
        }
        desktopBackdropCompositor_.SetVisible(false);
        ShowWindow(hwnd_, SW_HIDE);
        if (inputHwnd_ && IsWindow(inputHwnd_))
            ShowWindow(inputHwnd_, SW_HIDE);
        RestoreExplorerIcons();
        ApplyDesktopPassthroughHotkey();
        return;
    }

    desktopIconsHidden_ = false;
    if (explorerDesktopRecreatePending_)
    {
        RecoverDesktopHostAfterExplorerRestart();
        return;
    }

    HideExplorerIcons();
    ShowWindow(hwnd_, SW_SHOW);
    if (!desktopBackdropCompositor_.IsAvailable())
    {
        if (desktopBackdropCompositor_.Initialize(hwnd_))
        {
            nativeGlassPanelReadyLogged_ = false;
            WriteDiagnosticLogEntry(
                L"Native desktop CompositionBackdropBrush initialized");
        }
        else
        {
            std::wstring message =
                L"Native desktop CompositionBackdropBrush unavailable: ";
            message += desktopBackdropCompositor_.LastError();
            WriteDiagnosticLogEntry(message.c_str());
        }
    }
    desktopBackdropCompositor_.SetVisible(true);
    ReconcileDesktopHoverState();
    if (inputHwnd_ && IsWindow(inputHwnd_))
        ShowWindow(inputHwnd_, SW_SHOWNA);
    if (controlHwnd_ && IsWindow(controlHwnd_))
        SetTimer(controlHwnd_, kDesktopHostWatchTimerId,
            kDesktopHostWatchIntervalMs, nullptr);
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (!wasEnabled)
        ReloadItems();
    ApplyDesktopPassthroughHotkey();
}
