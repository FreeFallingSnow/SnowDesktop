#include "app.h"
#include "shell_icon_request.h"
#include "../shell_call_diagnostics.h"

#include "../popup_icon_load_rules.h"
#include "../shortcut_application_rules.h"

namespace shellCalls = snowdesktop::shell_call_diagnostics;

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

void DesktopApp::StartIconLoader() {}

void DesktopApp::DrainBackgroundShellWork()
{
    if (exitRequested_ || compositionPaintInProgress_ || reloading_ ||
        dragSession_.HasContext() || dragDropController_.IsTransportActive() ||
        HasActiveContextMenuSession() || mouseDown_ || renameEdit_ ||
        shellFileOperationInFlight_ > 0 || !pendingRenames_.empty())
        return; // The maintenance timer retries after the interaction fence.
    iconWork_.Drain();
    dockIconWork_.Drain();
    shellVisualWork_.Drain();
    shellModelWork_.Drain();
    appIndexWork_.Drain();
    folderReadWork_.Drain();
    clipboardReadWork_.Drain();
}

void DesktopApp::QueueIconTask(IconLoadTask value)
{
    auto input = std::make_shared<IconLoadTask>(std::move(value));
    const auto key = input->requestKey;
    const auto queuedAt = GetTickCount64();
    const auto makeResult = [input] {
        auto result = std::shared_ptr<IconLoadResult>(new IconLoadResult,
            [](IconLoadResult* value) {
                if (value->bitmap) DeleteObject(value->bitmap);
                delete value;
            });
        result->serial = input->serial;
        result->popupGeneration = input->popupGeneration;
        result->requestKey = input->requestKey;
        result->layoutKey = input->layoutKey;
        result->widgetId = input->widgetId;
        result->phase = input->phase;
        result->isDesktopItem = input->isDesktopItem;
        result->folderPath = input->folderPath;
        return result;
    };
    auto image = [input, queuedAt, makeResult] {
        auto& task = *input;
        const auto& tracePath = task.parsingName.empty() ? task.folderPath : task.parsingName;
        shellCalls::Context trace(
            task.phase == IconLoadPhase::Phase1 ? L"icon.phase1" : L"icon.phase2", tracePath);
        const auto started = GetTickCount64();
        auto result = makeResult();
        // First pixels must not wait for the system image list or shortcut
        // classification. Those providers have independent queues.
        if (task.sysIconIndex < 0 && task.phase == IconLoadPhase::Phase2)
        {
            SHFILEINFOW info{};
            const auto& path = task.parsingName.empty() ? task.folderPath : task.parsingName;
            if (shellCalls::Call(L"Metadata.SHGetFileInfo", [&] {
                    return SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                                          SHGFI_SYSICONINDEX | SHGFI_TYPENAME);
                }))
            {
                task.sysIconIndex = info.iIcon;
                result->typeName = info.szTypeName;
            }
        }
        const auto metadataDone = GetTickCount64();
        result->sysIconIndex = task.sysIconIndex;
        if (!task.absolutePidl.get())
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            const auto& path = task.parsingName.empty() ? task.folderPath : task.parsingName;
            if (SUCCEEDED(shellCalls::Call(L"Pidl.SHParseDisplayName", [&] {
                    return SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
                })))
                task.absolutePidl.reset(pidl);
        }
        const auto pidlDone = GetTickCount64();
        const std::wstring_view representationName = task.parsingName.empty()
            ? std::wstring_view(task.folderPath) : std::wstring_view(task.parsingName);
        namespace shortcutRules = snowdesktop::shortcut_application_rules;
        const bool nameLooksApplicationLike = shortcutRules::ShouldUseShellIconOnly(representationName);
        bool shellFolder = false;
        if (task.phase == IconLoadPhase::Phase2)
        {
            ComPtr<IShellItem> shellItem;
            if (SUCCEEDED(shellCalls::Call(L"Attributes.SHCreateItemFromIDList",
                                           [&] {
                                               return SHCreateItemFromIDList(
                                                   task.absolutePidl.get(),
                                                   IID_PPV_ARGS(&shellItem));
                                           })) &&
                shellItem)
            {
                SFGAOF attributes = 0;
                if (SUCCEEDED(shellCalls::Call(L"Attributes.GetAttributes", [&] {
                        return shellItem->GetAttributes(SFGAO_FOLDER, &attributes);
                    })))
                    shellFolder = (attributes & SFGAO_FOLDER) != 0;
            }
        }
        const bool allowThumbnail = snowdesktop::icon_render_rules::ShouldRequestShellThumbnail(
            task.phase == IconLoadPhase::Phase2, nameLooksApplicationLike, shellFolder);
        const bool forShortcut = shortcutRules::HasExtension(representationName, L".lnk") ||
            shortcutRules::HasExtension(representationName, L".url");
        bool iconIsThumbnail = false;
        result->bitmap =
            task.absolutePidl.get()
                ? shellCalls::Call(L"Bitmap.GetHighResolution",
                                   [&] {
                                       return GetHighResolutionShellIconBitmap(
                                           task.absolutePidl.get(), task.sysIconIndex,
                                           result->bitmapSize, allowThumbnail, task.requestedSize,
                                           nameLooksApplicationLike && !shellFolder, forShortcut,
                                           representationName, &iconIsThumbnail);
                                   })
                : nullptr;
        if (task.phase == IconLoadPhase::Phase1 && result->bitmap)
            shellCalls::Call(L"Bitmap.ClampAlphaToColorKey", [&] {
                return ClampAlphaToColorKey(result->bitmap, kTransparentKey);
            });
        result->iconIsMediaThumbnail = snowdesktop::icon_render_rules::IsMediaThumbnail(
            iconIsThumbnail, shellFolder);
        const auto finished = GetTickCount64();
        if (finished - queuedAt >= 250)
        {
            wchar_t timing[512]{};
            swprintf_s(
                timing,
                L"Shell icon slow: phase=%u queueMs=%llu metadataMs=%llu "
                L"pidlMs=%llu bitmapMs=%llu shortcutMs=0 totalMs=%llu bitmap=%d trace=%llu path=",
                task.phase == IconLoadPhase::Phase1 ? 1u : 2u, started - queuedAt,
                metadataDone - started, pidlDone - metadataDone, finished - pidlDone,
                finished - queuedAt, result->bitmap ? 1 : 0, trace.Id());
            WriteDiagnosticLogEntry((std::wstring(timing) + std::wstring(representationName)).c_str());
        }
        return result;
    };
    auto apply = [this, key, queuedAt](std::shared_ptr<IconLoadResult> result) {
        if (!result) { iconLoaderPendingKeys_.erase(key); return false; }
        auto delivered = new IconLoadResult(*result);
        result->bitmap = nullptr;
        const auto beforeApply = GetTickCount64();
        const bool accepted = OnIconLoaded(0, reinterpret_cast<LPARAM>(delivered));
        if (!result->isDesktopItem && result->phase == IconLoadPhase::Phase1 &&
            (beforeApply - queuedAt >= 250 || !accepted))
        {
            const auto message = L"Folder first icon delivery: totalMs=" +
                std::to_wstring(beforeApply - queuedAt) + L" applyMs=" +
                std::to_wstring(GetTickCount64() - beforeApply) + L" accepted=" +
                std::to_wstring(accepted ? 1 : 0) + L" widget=" + result->widgetId +
                L" path=" + result->folderPath;
            WriteDiagnosticLogEntry(message.c_str());
        }
        return accepted;
    };
    bool submitted = false;
    if (input->phase == IconLoadPhase::Phase1)
    {
        auto localImage = [input, queuedAt, makeResult] {
            const auto& path = input->parsingName.empty() ? input->folderPath : input->parsingName;
            shellCalls::Context trace(L"icon.local", path);
            const auto started = GetTickCount64();
            auto result = makeResult();
            result->sysIconIndex = input->sysIconIndex;
            result->bitmap = GetLocalIconResourceBitmap(path, result->bitmapSize, input->requestedSize);
            if (result->bitmap) ClampAlphaToColorKey(result->bitmap, kTransparentKey);
            // Record successes as well as misses during rollout so a startup
            // log distinguishes local delivery from waiting for Shell fallback.
            wchar_t timing[256]{};
            swprintf_s(timing, L"Local icon: queueMs=%llu readMs=%llu bitmap=%d trace=%llu path=",
                started - queuedAt, GetTickCount64() - started, result->bitmap ? 1 : 0, trace.Id());
            WriteDiagnosticLogEntry((std::wstring(timing) + path).c_str());
            return result;
        };
        auto classify = [input, makeResult] {
            const auto& tracePath =
                input->parsingName.empty() ? input->folderPath : input->parsingName;
            shellCalls::Context trace(L"icon.shortcut", tracePath);
            const auto started = GetTickCount64();
            auto result = makeResult();
            result->phase = IconLoadPhase::Shortcut;
            const auto& path = input->parsingName.empty() ? input->folderPath : input->parsingName;
            namespace shortcutRules = snowdesktop::shortcut_application_rules;
            const bool isLnk = shortcutRules::HasExtension(path, L".lnk");
            const bool isUrl = shortcutRules::HasExtension(path, L".url");
            result->isShortcut = isLnk || isUrl;
            ULONGLONG loadMs = 0, classifyMs = 0, targetMs = 0;
            if (isLnk)
            {
                ComPtr<IShellLinkW> shellLink;
                if (SUCCEEDED(shellCalls::Call(L"Classify.CoCreateInstance", [&] {
                        return CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(&shellLink));
                    })))
                {
                    ComPtr<IPersistFile> persistFile;
                    const bool loaded =
                        SUCCEEDED(shellCalls::Call(L"Classify.QueryPersistFile",
                                                   [&] {
                                                       return shellLink.As(&persistFile);
                                                   })) &&
                        SUCCEEDED(shellCalls::Call(L"Classify.Load", [&] {
                            return persistFile->Load(path.c_str(), STGM_READ);
                        }));
                    const auto loadedAt = GetTickCount64();
                    loadMs = loadedAt - started;
                    if (loaded)
                    {
                        result->isApplicationShortcut =
                            shellCalls::Call(L"Classify.ApplicationTarget", [&] {
                                return IsApplicationsShellLinkTarget(shellLink.Get(), path);
                            });
                        const auto classifiedAt = GetTickCount64();
                        classifyMs = classifiedAt - loadedAt;
                        if (!result->isApplicationShortcut)
                        {
                            wchar_t target[32768]{};
                            if (SUCCEEDED(shellCalls::Call(L"Classify.GetPath.ExeFallback", [&] {
                                    return shellLink->GetPath(
                                        target, static_cast<int>(std::size(target)), nullptr, 0);
                                })))
                                result->isApplicationShortcut = shortcutRules::HasExtension(target, L".exe");
                        }
                        targetMs = GetTickCount64() - classifiedAt;
                    }
                }
            }
            else if (isUrl)
            {
                wchar_t url[32768]{};
                shellCalls::Call(L"Classify.ReadUrl", [&] {
                    return GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url,
                                                    static_cast<DWORD>(std::size(url)),
                                                    path.c_str());
                });
                result->isApplicationShortcut = shortcutRules::IsSteamApplicationUrl(url);
            }
            const auto elapsed = GetTickCount64() - started;
            if (elapsed >= 250)
            {
                wchar_t timing[256]{};
                swprintf_s(timing,
                           L"Shell shortcut slow: loadMs=%llu classifyMs=%llu targetMs=%llu "
                           L"totalMs=%llu trace=%llu path=",
                           loadMs, classifyMs, targetMs, elapsed, trace.Id());
                WriteDiagnosticLogEntry((std::wstring(timing) + path).c_str());
            }
            return result;
        };
        submitted = iconWork_.SubmitFirstWithFallback(key, std::move(localImage), std::move(image),
            [](const std::shared_ptr<IconLoadResult>& result) { return result && result->bitmap; },
            std::move(classify), apply, apply, hwnd_, kBackgroundShellReadyMessage);
    }
    else
    {
        submitted = iconWork_.Submit(true, key, std::move(image), std::move(apply),
            hwnd_, kBackgroundShellReadyMessage);
    }
    if (!submitted) iconLoaderPendingKeys_.erase(key);
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
        if (item.iconState == IconState::FullQuality &&
            snowdesktop::icon_render_rules::SourceLongEdgeCoversTarget(
                item.iconBitmapSize.cx, item.iconBitmapSize.cy,
                desktopRequired))
            continue;

        IconLoadTask task;
        task.sourceStamp = snowdesktop::shell_icon_request::Stamp(item);
        task.serial = iconLoadSerial_;
        task.layoutKey = item.layoutKey;
        task.absolutePidl.reset(ILClone(item.absolutePidl.get()));
        task.sysIconIndex = item.sysIconIndex;
        task.parsingName = item.parsingName;
        task.isDesktopItem = true;
        task.phase = item.iconState == IconState::Loading ? IconLoadPhase::Phase1 : IconLoadPhase::Phase2;
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
            if (entry.iconState == IconState::FullQuality &&
                snowdesktop::icon_render_rules::SourceLongEdgeCoversTarget(
                    entry.iconBitmapSize.cx, entry.iconBitmapSize.cy,
                    required))
                continue;

            IconLoadTask task;
            task.sourceStamp = snowdesktop::shell_icon_request::Stamp(entry);
            task.serial = iconLoadSerial_;
            task.widgetId = widget.id;
            task.folderPath = entry.fullPath;
            task.sysIconIndex = entry.sysIconIndex;
            task.isDesktopItem = false;
            task.phase = entry.iconState == IconState::Loading ? IconLoadPhase::Phase1 : IconLoadPhase::Phase2;
            task.parsingName = entry.fullPath;
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
    iconWork_.Stop();
    dockIconWork_.Stop();
    shellVisualWork_.Stop();
    shellModelWork_.Stop();
    folderReadWork_.Stop();
    clipboardReadWork_.Stop();
    iconLoaderPendingKeys_.clear();
}

void DesktopApp::BeginIconLoadGeneration()
{
    ++iconLoadSerial_;
    iconWork_.Cancel();
    iconLoaderPendingKeys_.clear();
}

void DesktopApp::CancelDockFolderPopupIconLoads()
{
    dockFolderPopupIconGeneration_ =
        snowdesktop::popup_icon_load_rules::NextGeneration(dockFolderPopupIconGeneration_);
    const auto prefix = std::to_wstring(iconLoadSerial_) + L"\nF\n" + kDockFolderPopupWidgetId + L"\n";
    iconWork_.Cancel(prefix);
    std::erase_if(iconLoaderPendingKeys_, [&](const auto& key) { return key.starts_with(prefix); });
}

void DesktopApp::SetSoftwareDesktopEnabled(bool enabled, bool persist)
{
    const bool wasEnabled = customDesktopVisible_;
    if (!enabled)
        EndDesktopPassthrough(false);
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
        UpdatePersistentDockHostVisibility();
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
    UpdatePersistentDockHostVisibility();
    ApplyDesktopPassthroughHotkey();
}
