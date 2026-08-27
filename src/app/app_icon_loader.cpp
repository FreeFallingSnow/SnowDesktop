#include "app.h"
#include "../popup_icon_load_rules.h"
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

bool DecodeDemoIconPixels(const std::filesystem::path& path,
    int targetPixels, std::vector<std::uint32_t>& pixels,
    int& width, int& height)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        return false;

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0)
        return false;
    const double scale = std::min({ 1.0,
        static_cast<double>(targetPixels) / sourceWidth,
        static_cast<double>(targetPixels) / sourceHeight });
    const UINT scaledWidth = std::max(1U,
        static_cast<UINT>(std::lround(sourceWidth * scale)));
    const UINT scaledHeight = std::max(1U,
        static_cast<UINT>(std::lround(sourceHeight * scale)));
    IWICBitmapSource* source = frame.Get();
    if (scaledWidth != sourceWidth || scaledHeight != sourceHeight)
    {
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), scaledWidth, scaledHeight,
                WICBitmapInterpolationModeFant)))
            return false;
        source = scaler.Get();
    }
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
        return false;

    width = static_cast<int>(scaledWidth);
    height = static_cast<int>(scaledHeight);
    pixels.resize(static_cast<std::size_t>(width) * height);
    const UINT stride = scaledWidth * sizeof(std::uint32_t);
    return SUCCEEDED(converter->CopyPixels(nullptr, stride,
        stride * scaledHeight, reinterpret_cast<BYTE*>(pixels.data())));
}
}

void DesktopApp::StartDemoIconLoader()
{
    {
        std::lock_guard lock(demoIconLoaderMutex_);
        if (demoIconLoaderRunning_ || demoIconLoaderThread_.joinable())
            return;
        demoIconLoaderRunning_ = true;
    }
    demoIconLoaderThread_ = std::thread([this]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (true)
        {
            DemoIconLoadTask task;
            {
                std::unique_lock lock(demoIconLoaderMutex_);
                demoIconLoaderCv_.wait(lock, [this] {
                    return !demoIconLoaderQueue_.empty() ||
                        !demoIconLoaderRunning_;
                });
                if (!demoIconLoaderRunning_)
                    break;
                task = std::move(demoIconLoaderQueue_.front());
                demoIconLoaderQueue_.pop_front();
            }

            auto result = std::make_unique<DemoIconDecodeResult>();
            result->generation = task.generation;
            result->visualIndex = task.visualIndex;
            constexpr int kDemoSourcePixels = 96;
            if (DecodeDemoIconPixels(task.path, kDemoSourcePixels,
                    result->pixels, result->width, result->height) &&
                task.beautify.enabled)
            {
                const auto edge = task.beautify.mode == 0
                    ? snowdesktop::icon_beautify::DetectEdgeFill(
                        result->pixels, result->width, result->height)
                    : std::nullopt;
                result->pixels = snowdesktop::icon_beautify::Render(
                    result->pixels, result->width, result->height,
                    task.beautify, edge);
            }
            if (result->pixels.empty())
            {
                std::lock_guard lock(demoIconLoaderMutex_);
                if (task.generation == demoIconLoadGeneration_)
                {
                    demoIconLoaderPending_[task.visualIndex] = false;
                    demoIconLoaderFailed_[task.visualIndex] = true;
                }
                continue;
            }
            if (!PostMessageW(hwnd_, kDemoIconDecodedMessage, 0,
                    reinterpret_cast<LPARAM>(result.get())))
            {
                std::lock_guard lock(demoIconLoaderMutex_);
                if (task.generation == demoIconLoadGeneration_)
                    demoIconLoaderPending_[task.visualIndex] = false;
                continue;
            }
            result.release();
        }
        CoUninitialize();
    });
}

void DesktopApp::StopDemoIconLoader()
{
    {
        std::lock_guard lock(demoIconLoaderMutex_);
        demoIconLoaderRunning_ = false;
        demoIconLoaderQueue_.clear();
        demoIconLoaderPending_.fill(false);
        demoIconLoaderFailed_.fill(false);
    }
    demoIconLoaderCv_.notify_all();
    if (demoIconLoaderThread_.joinable())
        demoIconLoaderThread_.join();
    if (hwnd_)
    {
        MSG message{};
        while (PeekMessageW(&message, hwnd_, kDemoIconDecodedMessage,
                kDemoIconDecodedMessage, PM_REMOVE))
            delete reinterpret_cast<DemoIconDecodeResult*>(message.lParam);
    }
}

void DesktopApp::ResetDemoIconLoader()
{
    {
        std::lock_guard lock(demoIconLoaderMutex_);
        ++demoIconLoadGeneration_;
        demoIconLoaderQueue_.clear();
        demoIconLoaderPending_.fill(false);
        demoIconLoaderFailed_.fill(false);
    }
    for (auto& bitmap : demoIdentityIconBitmaps_)
        bitmap.Reset();
}

void DesktopApp::QueueDemoIdentityBitmap(std::size_t visualIndex)
{
    if (!demoIdentityAssetsAvailable_ ||
        visualIndex >= demoIdentityIconPaths_.size() ||
        demoIdentityIconPaths_[visualIndex].empty())
        return;
    {
        std::lock_guard lock(demoIconLoaderMutex_);
        if (!demoIconLoaderRunning_)
            return;
        if (demoIconLoaderPending_[visualIndex] ||
            demoIconLoaderFailed_[visualIndex])
            return;
        demoIconLoaderPending_[visualIndex] = true;
        demoIconLoaderQueue_.push_back(DemoIconLoadTask{
            demoIconLoadGeneration_, visualIndex,
            demoIdentityIconPaths_[visualIndex], iconBeautifySettings_ });
    }
    demoIconLoaderCv_.notify_one();
}

void DesktopApp::OnDemoIconDecoded(LPARAM lParam)
{
    std::unique_ptr<DemoIconDecodeResult> result(
        reinterpret_cast<DemoIconDecodeResult*>(lParam));
    if (!result || result->visualIndex >= demoIdentityIconBitmaps_.size())
        return;
    {
        std::lock_guard lock(demoIconLoaderMutex_);
        if (result->generation != demoIconLoadGeneration_)
            return;
        demoIconLoaderPending_[result->visualIndex] = false;
    }
    if (!d2dContext_ || result->width <= 0 || result->height <= 0 ||
        result->pixels.size() != static_cast<std::size_t>(result->width) *
            result->height)
        return;

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(d2dContext_->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(result->width),
                static_cast<UINT32>(result->height)),
            result->pixels.data(),
            static_cast<UINT32>(result->width * sizeof(std::uint32_t)),
            &properties, &bitmap)))
        return;
    demoIdentityIconBitmaps_[result->visualIndex] = std::move(bitmap);
    InvalidateDragStaticScene();
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateFloatingDockWindow(false);
    if (quickNavigationOpen_)
        InvalidateQuickNavigationWindow();
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
                result->popupGeneration = task.popupGeneration;
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

void DesktopApp::CancelDockFolderPopupIconLoads()
{
    std::lock_guard<std::mutex> lock(iconLoaderMutex_);
    dockFolderPopupIconGeneration_ =
        snowdesktop::popup_icon_load_rules::NextGeneration(
            dockFolderPopupIconGeneration_);
    snowdesktop::popup_icon_load_rules::CancelQueuedTasks(
        iconLoaderQueue_, iconLoaderPendingKeys_,
        [](const IconLoadTask& task) {
            return !task.isDesktopItem &&
                task.widgetId == kDockFolderPopupWidgetId;
        });
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
    if (settingsController_)
        (void)settingsController_->SynchronizeGeneral(generalSettings_);

    if (!hwnd_ || !IsWindow(hwnd_))
    {
        ApplyDesktopPassthroughHotkey();
        return;
    }

    if (!enabled)
    {
        if (widgetEngine_)
            widgetEngine_->SetAllWidgetDesktopVisible(false);
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
    ReconcileDesktopHoverState(
        snowdesktop::desktop_hover_rules::
            ReconcileMode::AllowImmediateActivation);
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
