#include "app.h"

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
            HBITMAP bitmap = GetHighResolutionShellIconBitmap(
                task.absolutePidl.get(), task.sysIconIndex, bitmapSize,
                task.phase == IconLoadPhase::Phase2);
            if (task.phase == IconLoadPhase::Phase1 && bitmap)
                ClampAlphaToColorKey(bitmap, kTransparentKey);

            bool isShortcut = false;
            bool isApplicationShortcut = false;
            if (task.phase == IconLoadPhase::Phase1)
            {
                std::wstring upper = task.parsingName;
                for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
                const bool isLnk = upper.size() > 4 && upper.compare(upper.size() - 4, 4, L".LNK") == 0;
                const bool isUrl = upper.size() > 4 && upper.compare(upper.size() - 4, 4, L".URL") == 0;
                isShortcut = isLnk || isUrl;
                if (isLnk)
                {
                    wchar_t lnkPath[MAX_PATH]{};
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
                                if (!IsApplicationsShellLinkTarget(shellLink.Get()))
                                {
                                    wchar_t target[MAX_PATH]{};
                                    if (SUCCEEDED(shellLink->GetPath(target, MAX_PATH, nullptr, 0)) &&
                                        target[0] != L'\0')
                                    {
                                        std::wstring t(target);
                                        for (auto& c : t) c = static_cast<wchar_t>(towupper(c));
                                        isApplicationShortcut =
                                            t.size() >= 4 && t.compare(t.size() - 4, 4, L".EXE") == 0;
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
