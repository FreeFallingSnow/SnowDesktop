/**
 * @file app_run.h
 * @brief DesktopApp 内联实现 —— 主循环、窗口过程、桌面宿主管理、拖放支持
 *
 * 本文件包含 DesktopApp 类的内联方法实现，在 app_oo.h 中类定义之后
 * 通过 #include 引入。涵盖主消息循环（Run）、静态窗口过程（WndProc）、
 * 桌面覆盖层窗口的创建与销毁、资源管理器桌面图标的隐藏与恢复、
 * OLE 拖放支持注册、桌面宿主状态监视与恢复、快捷导航、
 * 以及部件（Widget）管理等相关功能。
 */

#pragma once
// Inline implementations for DesktopApp — destructor, Run, WndProc, HandleMessage.
// This file is included by app_oo.h after the class definition.

#include <commoncontrols.h>

// ── Inline implementations ──────────────────────────────────

/**
 * @brief 析构函数
 *
 * 依次释放 WidgetEngine、SettingsWindow、桌面项与容器等资源，
 * 清理 FontAwesome 菜单字体与内存字体资源的加载句柄。
 */
inline DesktopApp::~DesktopApp()
{
    StopIconLoader();
    widgetEngine_.reset();
    settingsWindow_.reset();
    items_oo_.clear();
    containers_.clear();
    ClearMenuIcons();
    if (faMenuFont_)
    {
        DeleteObject(faMenuFont_);
        faMenuFont_ = nullptr;
    }
    if (faFontHandle_)
    {
        RemoveFontMemResourceEx(faFontHandle_);
        faFontHandle_ = nullptr;
    }
}

/**
 * @brief 隐藏资源管理器桌面图标层
 *
 * 通过 ShowWindow(SW_HIDE) 隐藏桌面的 ListView 窗口（即图标层），
 * 并用窗口属性 kHiddenBySnowDesktopProp 标记此操作为 SnowDesktop 所为，
 * 以便后续恢复时判断。
 */
inline void DesktopApp::HideExplorerIcons()
{
    if (!desktopWindows_.listView || !IsWindow(desktopWindows_.listView))
        return;

    const bool hiddenBySnowDesktop = GetPropW(desktopWindows_.listView,
        kHiddenBySnowDesktopProp) != nullptr;
    const bool shouldHide = IsWindowVisible(desktopWindows_.listView) != FALSE ||
        hiddenBySnowDesktop;
    desktopWindows_.listViewWasVisible = shouldHide;
    if (!shouldHide)
        return;

    SetPropW(desktopWindows_.listView, kHiddenBySnowDesktopProp,
        reinterpret_cast<HANDLE>(1));
    ShowWindow(desktopWindows_.listView, SW_HIDE);
}

/**
 * @brief 恢复显示资源管理器桌面图标
 *
 * 若之前由 SnowDesktop 隐藏且记录为可见状态，
 * 则调用 ShowWindow(SW_SHOW) 恢复 ListView 显示，
 * 并清除隐藏标记属性。
 */
inline void DesktopApp::RestoreExplorerIcons()
{
    if (!desktopWindows_.listView || !IsWindow(desktopWindows_.listView))
        return;

    const bool hiddenBySnowDesktop = GetPropW(desktopWindows_.listView,
        kHiddenBySnowDesktopProp) != nullptr;
    if (desktopWindows_.listViewWasVisible || hiddenBySnowDesktop)
        ShowWindow(desktopWindows_.listView, SW_SHOW);
    RemovePropW(desktopWindows_.listView, kHiddenBySnowDesktopProp);
    desktopWindows_.listViewWasVisible = false;
}

/**
 * @brief 注册 OLE 拖放目标
 *
 * 调用 RegisterDragDrop 将主窗口注册为 OLE 拖放目标，
 * 使外部文件可拖放至桌面覆盖层。仅在首次成功时执行一次。
 */
inline void DesktopApp::RegisterOleDropTarget()
{
    if (!hwnd_ || !IsWindow(hwnd_) || dropTargetRegistered_)
        return;
    dropTargetRegistered_ = SUCCEEDED(RegisterDragDrop(hwnd_, static_cast<IDropTarget*>(this)));
}

/**
 * @brief 重置桌面窗口相关资源（反初始化）
 *
 * 注销导航热键、销毁各定时器、撤销 OLE 拖放注册、
 * 注销 Shell 变更通知、销毁拖拽提示窗口与快捷导航窗口、
 * 释放 DirectComposition 表面/视觉/目标，并将窗口句柄置空。
 */
inline void DesktopApp::ResetDesktopWindowResources()
{
    if (hwnd_ && IsWindow(hwnd_))
    {
        UnregisterNavigationHotkey();
        KillTimer(hwnd_, kShellChangeTimerId);
        KillTimer(hwnd_, kRecycleBinPollTimerId);
        KillTimer(hwnd_, kWidgetRefreshTimerId);
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        if (dropTargetRegistered_)
            RevokeDragDrop(hwnd_);
    }
    dropTargetRegistered_ = false;

    if (shellChangeRegId_ != 0)
    {
        SHChangeNotifyDeregister(shellChangeRegId_);
        shellChangeRegId_ = 0;
    }

    DestroyDragHintWindow();
    DestroyQuickNavigationWindow();
    dragRenderCache_.Reset();
    dcompSurface_.Reset();
    dcompVisual_.Reset();
    dcompTarget_.Reset();
    compositionWidth_ = 0;
    compositionHeight_ = 0;
    hwnd_ = nullptr;
}

/**
 * @brief 将桌面覆盖层窗口附加到指定桌面宿主窗口
 *
 * @param host 目标桌面宿主窗口句柄（通常是 WorkerW 或 Progman）
 *
 * 将主窗口样式改为 WS_CHILD 并设置 parent 为 host，
 * 同时调整位置到虚拟屏幕原点并显示窗口。
 */
inline void DesktopApp::AttachWindowToDesktopHost(HWND host)
{
    if (!hwnd_ || !IsWindow(hwnd_) || !host || !IsWindow(host))
        return;

    if (GetParent(hwnd_) != host)
        SetParent(hwnd_, host);

    LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    style &= ~WS_POPUP;
    style |= WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);

    POINT origin{ virtualLeft_, virtualTop_ };
    ScreenToClient(host, &origin);
    SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

/**
 * @brief 创建桌面覆盖层窗口
 *
 * @return true  窗口创建成功且所有组件初始化完成
 * @return false 创建失败，已自动回滚相关资源
 *
 * 依次创建覆盖层窗口（首选子窗口，兜底弹窗）、DirectComposition 目标
 * 与视觉树、组合表面，设置应用图标，注册 OLE 拖放与导航热键，
 * 启动 Shell 变更通知和定时器，最终使窗口可见并触发首次绘制。
 */
inline bool DesktopApp::CreateDesktopOverlayWindow()
{
    auto fail = [this]() {
        if (hwnd_ && IsWindow(hwnd_))
            DestroyWindow(hwnd_);
        ResetDesktopWindowResources();
        return false;
    };

    HWND parent = desktopWindows_.host && IsWindow(desktopWindows_.host)
        ? desktopWindows_.host
        : GetDesktopWindow();
    POINT origin{ virtualLeft_, virtualTop_ };
    ScreenToClient(parent, &origin);

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"SnowDesktopWindow", L"SnowDesktop",
        WS_CHILD | WS_VISIBLE, origin.x, origin.y, virtualWidth_, virtualHeight_,
        parent, nullptr, instance_, this);
    if (!hwnd_)
    {
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"SnowDesktopWindow", L"SnowDesktop",
            WS_POPUP | WS_VISIBLE, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_,
            nullptr, nullptr, instance_, this);
        if (hwnd_ && parent && parent != GetDesktopWindow())
            AttachWindowToDesktopHost(parent);
    }
    if (!hwnd_)
        return false;

    SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_,
        SWP_NOACTIVATE);

    if (FAILED(dcompDevice_->CreateTargetForHwnd(hwnd_, FALSE, &dcompTarget_)))
        return fail();
    if (FAILED(dcompDevice_->CreateVisual(&dcompVisual_)))
        return fail();
    dcompTarget_->SetRoot(dcompVisual_.Get());
    if (FAILED(CreateOrResizeCompositionSurface()))
        return fail();

    if (HICON appIcon = LoadAppIcon())
    {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }

    RegisterOleDropTarget();
    ApplyNavigationHotkey();
    RegisterShellChangeNotifications();
    SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);
    SetTimer(hwnd_, kWidgetRefreshTimerId, kWidgetRefreshIntervalMs, nullptr);

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    InvalidateRect(hwnd_, nullptr, TRUE);
    UpdateWindow(hwnd_);
    return true;
}

/**
 * @brief 资源管理器重启后恢复桌面宿主连接
 *
 * 重新查找当前桌面窗口（WorkerW/Progman），将覆盖层窗口重新附加为子窗口，
 * 恢复 OLE 拖放注册与 Shell 变更通知，隐藏桌面图标，添加托盘图标。
 * 若原有窗口已失效则重建覆盖层窗口，并根据当前状态决定是否重载桌面项。
 */
inline void DesktopApp::RecoverDesktopHostAfterExplorerRestart()
{
    if (exitRequested_)
        return;

    DesktopWindows current = FindDesktopWindows();
    if (!current.host || !IsWindow(current.host))
        return;

    desktopWindows_ = current;
    if (hwnd_ && IsWindow(hwnd_))
    {
        AttachWindowToDesktopHost(desktopWindows_.host);
        RegisterOleDropTarget();
        RegisterShellChangeNotifications();
    }
    else
    {
        ResetDesktopWindowResources();
        if (!CreateDesktopOverlayWindow())
            return;
    }

    HideExplorerIcons();
    AddTrayIcon(true);
    if (controlHwnd_ && IsWindow(controlHwnd_))
        SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);

    if (!mouseDown_ && !reloading_)
        ReloadItems(false);
    else if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 定时监视桌面宿主窗口状态
 *
 * 检查主窗口的父窗口是否仍为正确的桌面宿主窗口，
 * 若检测到宿主丢失、ListView 消失或宿主发生变化，
 * 则调用 RecoverDesktopHostAfterExplorerRestart 进行恢复。
 */
inline void DesktopApp::WatchDesktopHost()
{
    if (exitRequested_)
        return;

    if (!customDesktopVisible_)
        return;

    if (!hwnd_ || !IsWindow(hwnd_))
    {
        RecoverDesktopHostAfterExplorerRestart();
        return;
    }

    DesktopWindows current = FindDesktopWindows();
    HWND currentHost = current.host;
    HWND parent = GetParent(hwnd_);
    const bool parentMissing = parent == nullptr || !IsWindow(parent);
    const bool knownHostMissing = desktopWindows_.host == nullptr || !IsWindow(desktopWindows_.host);
    const bool knownListViewMissing = desktopWindows_.listView != nullptr &&
        !IsWindow(desktopWindows_.listView);
    const bool hostChanged = currentHost && IsWindow(currentHost) &&
        currentHost != desktopWindows_.host;
    const bool parentDetached = currentHost && IsWindow(currentHost) &&
        parent != currentHost;

    if (parentMissing || knownHostMissing || knownListViewMissing ||
        hostChanged || parentDetached)
    {
        RecoverDesktopHostAfterExplorerRestart();
        return;
    }

    if (current.listView && IsWindow(current.listView) && IsWindowVisible(current.listView))
    {
        desktopWindows_ = current;
        HideExplorerIcons();
    }
}

/**
 * @brief 请求退出应用程序
 *
 * 设置退出标志，恢复资源管理器桌面图标，销毁主窗口触发 WM_DESTROY，
 * 或直接执行清理流程（保存布局槽位、移除托盘图标、重置资源）
 * 并发送 PostQuitMessage 退出消息循环。
 */
inline void DesktopApp::RequestExit()
{
    if (exitRequested_)
        return;
    exitRequested_ = true;
    StopIconLoader();
    RestoreExplorerIcons();
    if (hwnd_ && IsWindow(hwnd_))
    {
        DestroyWindow(hwnd_);
        return;
    }

    SaveLayoutSlots();
    RemoveTrayIcon();
    ResetDesktopWindowResources();
    if (controlHwnd_ && IsWindow(controlHwnd_))
        DestroyWindow(controlHwnd_);
    else
        PostQuitMessage(0);
}

/**
 * @brief 应用程序主入口 —— 初始化与消息循环
 *
 * @param instance  当前应用程序实例句柄
 * @param showCommand 窗口显示命令（由系统传入，暂未使用）
 * @return int 消息循环的退出码（WM_QUIT 的 wParam）
 *
 * 执行完整初始化流程：设置 DPI 感知、初始化通用控件与 OLE、
 * 查找桌面窗口并隐藏图标、注册窗口类、创建覆盖层窗口、
 * 初始化 DirectComposition 图形管线、加载桌面项与网格布局、
 * 创建 SettingsWindow 与 WidgetEngine，最后进入 GetMessage 消息循环
 * 直至收到 WM_QUIT。
 */
inline void ClampAlphaToColorKey(HBITMAP bitmap, COLORREF key)
{
    if (!bitmap) return;
    BITMAP bm{};
    if (GetObjectW(bitmap, sizeof(bm), &bm) == 0 || bm.bmBitsPixel != 32 || !bm.bmBits) return;
    int w = bm.bmWidth, absH = std::abs(bm.bmHeight);
    auto* pixels = static_cast<std::uint32_t*>(bm.bmBits);
    size_t count = static_cast<size_t>(w) * static_cast<size_t>(absH);
    for (size_t i = 0; i < count; ++i)
    {
        uint8_t a = (pixels[i] >> 24) & 0xff;
        uint8_t r = (pixels[i] >> 16) & 0xff;
        uint8_t g = (pixels[i] >> 8)  & 0xff;
        uint8_t b = pixels[i] & 0xff;
        if (a < 250 && (int(r) + int(g) + int(b)) < 150)
            pixels[i] = 0;
    }
    (void)key;
}

inline void DesktopApp::StartIconLoader()
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

            bool shortcutArrow = false;
            if (task.phase == IconLoadPhase::Phase1)
            {
                std::wstring upper = task.parsingName;
                for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
                if (upper.size() > 4 && upper.compare(upper.size() - 4, 4, L".LNK") == 0)
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
                                wchar_t target[MAX_PATH]{};
                                if (SUCCEEDED(shellLink->GetPath(target, MAX_PATH, nullptr, 0)))
                                {
                                    std::wstring t(target);
                                    for (auto& c : t) c = static_cast<wchar_t>(towupper(c));
                                    if (t.size() < 4 || t.compare(t.size() - 4, 4, L".EXE") != 0)
                                        shortcutArrow = true;
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
                result->shortcutArrow = shortcutArrow;
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

inline void DesktopApp::StopIconLoader()
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

inline void DesktopApp::BeginIconLoadGeneration()
{
    std::lock_guard<std::mutex> lock(iconLoaderMutex_);
    ++iconLoadSerial_;
    iconLoaderQueue_.clear();
    iconLoaderPendingKeys_.clear();
}

inline int DesktopApp::Run(HINSTANCE instance, int showCommand)
{
    (void)showCommand;

    WriteCrashLogEntry(L"Run start");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    HRESULT hr = OleInitialize(nullptr);
    WriteCrashLogEntry(SUCCEEDED(hr) ? L"OleInit ok" : L"OleInit FAILED");

    instance_ = instance;

    // Find and hide Explorer icon layer
    desktopWindows_ = FindDesktopWindows();
    {
        wchar_t buf[256];
        wsprintfW(buf, L"Desktop: progman=%p defView=%p listView=%p host=%p",
            desktopWindows_.progman, desktopWindows_.defView,
            desktopWindows_.listView, desktopWindows_.host);
        WriteCrashLogEntry(buf);
    }
    HideExplorerIcons();
    if (desktopWindows_.listView && desktopWindows_.listViewWasVisible)
        WriteCrashLogEntry(L"Explorer icon layer hidden");
    else
        WriteCrashLogEntry(L"Explorer icon layer not found or already hidden");

    // Create desktop overlay window as child of desktop host
    virtualLeft_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    virtualTop_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    virtualWidth_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virtualHeight_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    BeginIconLoadGeneration();
    LoadLayoutSlots();
    UpdateLayoutWorkArea();

    HWND parent = desktopWindows_.host ? desktopWindows_.host : GetDesktopWindow();
    POINT origin{ virtualLeft_, virtualTop_ };
    ScreenToClient(parent, &origin);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"SnowDesktopWindow";
    RegisterClassExW(&wc);

    {
        WNDCLASSEXW hint{};
        hint.cbSize = sizeof(hint);
        hint.lpfnWndProc = DefWindowProcW;
        hint.hInstance = instance;
        hint.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        hint.hbrBackground = nullptr;
        hint.lpszClassName = kHintWindowClassName;
        RegisterClassExW(&hint);
    }
    {
        WNDCLASSEXW nav{};
        nav.cbSize = sizeof(nav);
        nav.style = CS_DBLCLKS;
        nav.lpfnWndProc = QuickNavigationWndProc;
        nav.hInstance = instance;
        nav.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        nav.hbrBackground = nullptr;
        nav.lpszClassName = kQuickNavigationWindowClassName;
        RegisterClassExW(&nav);
    }

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"SnowDesktop",
        WS_CHILD | WS_VISIBLE, origin.x, origin.y, virtualWidth_, virtualHeight_,
        parent, nullptr, instance, this);
    if (!hwnd_)
    {
        WriteCrashLogEntry(L"Child failed, fallback popup");
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"SnowDesktop",
            WS_POPUP | WS_VISIBLE, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_,
            nullptr, nullptr, instance, this);
        if (hwnd_ && parent && parent != GetDesktopWindow())
        {
            SetParent(hwnd_, parent);
            LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            style &= ~WS_POPUP;
            style |= WS_CHILD | WS_VISIBLE;
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
            SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
    }
    if (!hwnd_) { WriteCrashLogEntry(L"CreateWindow FAILED"); return __LINE__; }
    SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
    WriteCrashLogEntry(L"Window created");
    {
        wchar_t buf[256];
        wsprintfW(buf, L"Parent=%p origin=(%d,%d) size=%dx%d",
            parent, origin.x, origin.y, virtualWidth_, virtualHeight_);
        WriteCrashLogEntry(buf);
    }

    if (!InitGraphics()) { WriteCrashLogEntry(L"InitGraphics FAILED"); return __LINE__; }
    WriteCrashLogEntry(L"InitGraphics ok");

    // Create control window for tray icon ownership
    {
        WNDCLASSEXW cwc{};
        cwc.cbSize = sizeof(cwc);
        cwc.lpfnWndProc = ControlWndProc;
        cwc.hInstance = instance;
        cwc.lpszClassName = kControlWindowClassName;
        RegisterClassExW(&cwc);
    }
    controlHwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kControlWindowClassName, L"SnowDesktopControl", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance, this);
    taskbarRestartMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    // Create DComp target and initial surface
    if (FAILED(dcompDevice_->CreateTargetForHwnd(hwnd_, FALSE, &dcompTarget_)))
        { WriteCrashLogEntry(L"CreateTargetForHwnd FAILED"); return __LINE__; }
    if (FAILED(dcompDevice_->CreateVisual(&dcompVisual_)))
        { WriteCrashLogEntry(L"CreateVisual FAILED"); return __LINE__; }
    dcompTarget_->SetRoot(dcompVisual_.Get());
    if (FAILED(CreateOrResizeCompositionSurface()))
        { WriteCrashLogEntry(L"CreateCompositionSurface FAILED"); return __LINE__; }
    WriteCrashLogEntry(L"Composition target ready");

    // Use the same placement pipeline as runtime refreshes so a desktop that
    // already contains more items than the visible grids can create virtual
    // overflow pages during the initial load.
    ReloadItems(false);
    StartIconLoader();
    WriteCrashLogEntry(L"LoadDesktopItems ok");
    WriteCrashLogEntry(L"Layout done");
    WriteCrashLogEntry(L"RebuildContainersAndItems ok");

    // App icon
    if (HICON appIcon = LoadAppIcon())
    {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }

    AddTrayIcon();
    RegisterShellChangeNotifications();
    RegisterOleDropTarget();
    LoadNavigationSettingsAndApply();

    // Timers
    SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);
    SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);
    SetTimer(hwnd_, kWidgetRefreshTimerId, kWidgetRefreshIntervalMs, nullptr);

    settingsWindow_ = std::make_unique<SettingsWindow>();
    if (settingsWindow_->Init(instance, d3dDevice_.Get()))
    {
        settingsWindow_->SetReloadCallback([this]() { ReloadItems(); });
        settingsWindow_->SetExitCallback([this]() { RequestExit(); });
        settingsWindow_->SetInvalidateCallback([this]() {
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        });
        settingsWindow_->SetNavigationSettingsChangedCallback([this]() {
            LoadNavigationSettingsAndApply();
        });
    }
    else
    {
        settingsWindow_.reset();
    }

    widgetEngine_ = std::make_unique<WidgetEngine>();
    if (widgetEngine_->Init(d2dContext_.Get(), dwriteFactory_.Get()))
    {
        widgetEngine_->SetDesktopSnapshotProvider([this]() {
            return BuildLuaDesktopSnapshot(false);
        });
        widgetEngine_->SetSelectionProvider([this]() {
            return BuildLuaDesktopSnapshot(true);
        });
        widgetEngine_->SetWidgetTitleCallback([this](const std::wstring& widgetId, const std::wstring& title) {
            LuaSetWidgetTitle(widgetId, title);
        });
        widgetEngine_->SetInvalidateCallback([this]() {
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        });
        widgetEngine_->SetDesktopOpenCallback([this](const std::wstring& path) {
            return LuaOpenPath(path);
        });
        widgetEngine_->SetDesktopRevealCallback([this](const std::wstring& path) {
            return LuaRevealPath(path);
        });
        widgetEngine_->SetDesktopRefreshCallback([this]() {
            ReloadItems();
        });
        widgetEngine_->SetInlineTextEditCallback([this](const LuaInlineTextEditRequest& request) {
            BeginLuaInlineTextEdit(request);
        });
        widgetEngine_->SetNotifyCallback([this](const std::wstring& title, const std::wstring& message) {
            ShowBalloonNotification(title, message);
        });
        widgetEngine_->SetOpenWidgetSettingsCallback([this](const std::wstring& widgetId, const std::wstring&) {
            for (size_t i = 0; i < widgets_.size(); ++i)
            {
                if (widgets_[i].id == widgetId && widgets_[i].type == DesktopWidgetType::LuaScript)
                {
                    ShowWidgetEditorHost(i);
                    break;
                }
            }
        });
        if (settingsWindow_)
            settingsWindow_->SetWidgetEngine(widgetEngine_.get());
    }
    else
    {
        widgetEngine_.reset();
    }

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    WriteCrashLogEntry(L"Window shown, entering loop");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (settingsWindow_ && settingsWindow_->IsVisible())
            settingsWindow_->Render();
    }
    OleUninitialize();
    return static_cast<int>(msg.wParam);
}

/**
 * @brief 主窗口的静态窗口过程
 *
 * @param hwnd 窗口句柄
 * @param msg  消息标识符
 * @param wp   WPARAM 参数
 * @param lp   LPARAM 参数
 * @return LRESULT 消息处理结果
 *
 * 在 WM_NCCREATE 时将 DesktopApp 实例指针存入 GWLP_USERDATA，
 * 后续消息中取出实例并转发至 HandleMessage。
 */
inline LRESULT CALLBACK DesktopApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopApp* app = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<DesktopApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // Log first few messages
    if (msg == WM_NCCREATE || msg == WM_CREATE || msg == WM_SIZE || msg == WM_PAINT || msg == WM_SHOWWINDOW)
    {
        wchar_t buf[128];
        wsprintfW(buf, L"WndProc msg=0x%04X app=%p", msg, app);
        WriteCrashLogEntry(buf);
    }

    if (app) return app->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 快捷导航窗口的静态窗口过程
 *
 * @param hwnd 窗口句柄
 * @param msg  消息标识符
 * @param wp   WPARAM 参数
 * @param lp   LPARAM 参数
 * @return LRESULT 消息处理结果
 *
 * 在 WM_NCCREATE 时存储 DesktopApp 实例指针，
 * 后续消息转发至 HandleQuickNavigationMessage 处理。
 */
inline LRESULT CALLBACK DesktopApp::QuickNavigationWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopApp* app = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<DesktopApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (app)
        return app->HandleQuickNavigationMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 主窗口消息处理函数
 *
 * @param hwnd 窗口句柄
 * @param msg  消息标识符
 * @param wp   WPARAM 参数
 * @param lp   LPARAM 参数
 * @return LRESULT 消息处理结果，未处理的消息交由 DefWindowProcW
 *
 * 集中处理桌面覆盖层窗口的所有消息：
 * - 上下文菜单（IContextMenu 接口路由）
 * - WM_PAINT / WM_ERASEBKGND 绘制
 * - WM_SIZE 尺寸变更及布局重算
 * - 鼠标消息（左键/右键/滚轮/双击及快捷导航点击）
 * - 键盘消息与导航热键
 * - WM_DISPLAYCHANGE / WM_SETTINGCHANGE 多显示器变更
 * - Shell 变更通知与各定时器回调
 * - 托盘回调、WM_CLOSE 关闭、WM_DESTROY 销毁清理
 */
inline LRESULT DesktopApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MENUSELECT && gridAdjustmentParentMenu_ &&
        reinterpret_cast<HMENU>(lp) == gridAdjustmentParentMenu_ &&
        LOWORD(wp) == kContextGridAdjustmentMenu &&
        !(HIWORD(wp) & MF_POPUP))
    {
        const int itemCount = GetMenuItemCount(gridAdjustmentParentMenu_);
        for (int i = 0; i < itemCount; ++i)
        {
            MENUITEMINFOW itemInfo{ sizeof(itemInfo) };
            itemInfo.fMask = MIIM_ID;
            if (!GetMenuItemInfoW(gridAdjustmentParentMenu_,
                    static_cast<UINT>(i), TRUE, &itemInfo) ||
                itemInfo.wID != kContextGridAdjustmentMenu)
                continue;

            RECT itemRect{};
            if (GetMenuItemRect(hwnd_, gridAdjustmentParentMenu_,
                    static_cast<UINT>(i), &itemRect))
            {
                gridAdjustmentMenuAnchor_ = { itemRect.right, itemRect.top };
                gridAdjustmentMenuAnchorValid_ = true;
            }
            break;
        }
    }

    if (newMenuContextMenu_ && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM))
    {
        if (SUCCEEDED(newMenuContextMenu_->HandleMenuMsg(msg, wp, lp)))
            return 0;
    }
    if (activeContextMenu3_)
    {
        LRESULT result = 0;
        if (SUCCEEDED(activeContextMenu3_->HandleMenuMsg2(msg, wp, lp, &result)))
            return result;
    }
    else if (activeContextMenu2_)
    {
        if (SUCCEEDED(activeContextMenu2_->HandleMenuMsg(msg, wp, lp)))
            return 0;
    }

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        OnPaint();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
    {
        bool wasDragging = dragSession_.IsActive();
        virtualWidth_ = LOWORD(lp);
        virtualHeight_ = HIWORD(lp);
        dcompSurface_.Reset();
        UpdateLayoutWorkArea();
        LayoutItems();
        if (wasDragging && !dragSession_.IsActive())
        {
            mouseDownHit_ = nullptr;
            mouseDown_ = false;
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        OnLeftButtonDown(wp, lp);
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(wp, lp);
        return 0;
    case WM_LBUTTONUP:
        OnLeftButtonUp(wp, lp);
        return 0;
    case WM_MOUSEWHEEL:
        OnMouseWheel(wp, lp);
        return 0;
    case WM_RBUTTONUP:
        OnRightButtonUp(lp);
        return 0;
    case WM_LBUTTONDBLCLK:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (quickNavigationOpen_)
        {
            HandleQuickNavigationClick(pt);
            return 0;
        }

        if (popupWidgetIndex_ < widgets_.size())
        {
            RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
            if (PtInRect(&popup, pt))
            {
                std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
                RECT content = GetCollectionPopupContentRect(popup);
                for (size_t i = 0; i < popupKeys.size(); ++i)
                {
                    RECT itemRect = GetCollectionPopupItemRect(popup, i);
                    RECT clipped = itemRect;
                    clipped.top = std::max(clipped.top, content.top);
                    clipped.bottom = std::min(clipped.bottom, content.bottom);
                    if (clipped.bottom <= clipped.top || !PtInRect(&clipped, pt)) continue;
                    size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                    if (itemIndex != static_cast<size_t>(-1))
                    {
                        ShellExecuteW(nullptr, L"open", items_[itemIndex].parsingName.c_str(),
                            nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                }
            }
        }

        size_t standaloneWidget = HitTestStandaloneWidgetIndex(pt);
        if (standaloneWidget != static_cast<size_t>(-1) &&
            widgets_[standaloneWidget].type == DesktopWidgetType::LuaScript)
        {
            if (HitTestStandaloneWidget(standaloneWidget, pt) == WidgetHit::Content && widgetEngine_)
            {
                RECT frame = GetStandaloneWidgetFrameRect(widgets_[standaloneWidget]);
                widgetEngine_->EnsureWidgetLoaded(widgets_[standaloneWidget].id,
                    widgets_[standaloneWidget].scriptPath);
                widgetEngine_->InvokeMouseEvent(widgets_[standaloneWidget].id, "onDoubleClick",
                    pt.x - frame.left, pt.y - frame.top, 1, 0);
            }
            return 0;
        }

        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(it->get());
            if (!wc) continue;
            for (auto& slot : wc->GetSlots())
            {
                if (!slot) continue;
                RECT slotBounds = slot->GetBounds();
                if (!PtInRect(&slotBounds, pt)) continue;
                if (auto* icon = dynamic_cast<DesktopIcon*>(slot->GetItem()))
                {
                    DesktopItem* item = icon->GetDesktopItem();
                    if (item)
                    {
                        ShellExecuteW(nullptr, L"open", item->parsingName.c_str(),
                            nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                }
                if (auto* folderIcon = dynamic_cast<FolderEntryIcon*>(slot->GetItem()))
                {
                    FolderEntry* entry = folderIcon->GetFolderEntry();
                    if (entry)
                    {
                        ShellExecuteW(nullptr, L"open", entry->fullPath.c_str(),
                            nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                }
            }
        }

        int hit = HitTestItem(pt);
        if (hit >= 0)
        {
            ShellExecuteW(nullptr, L"open", items_[hit].parsingName.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;
    case WM_KEYDOWN:
        OnKeyDown(wp);
        return 0;
    case WM_HOTKEY:
        if (static_cast<int>(wp) == kQuickNavigationHotkeyId)
        {
            ToggleQuickNavigation();
            return 0;
        }
        break;
    case WM_KEYUP:
        RefreshDragHintFromKeyboard();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        virtualLeft_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
        virtualTop_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
        virtualWidth_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        virtualHeight_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (dcompVisual_)
            CreateOrResizeCompositionSurface();
        UpdateLayoutWorkArea();
        LayoutItems();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
    case kShellChangeMessage:
        SetTimer(hwnd_, kShellChangeTimerId, kShellChangeDebounceMs, nullptr);
        return 0;
    case kIconLoadedMessage:
        OnIconLoaded(wp, lp);
        return 0;
    case WM_TIMER:
        OnTimer(wp);
        return 0;
    case kTrayCallbackMessage:
        OnTrayCallback(lp);
        return 0;
    case WM_CLOSE:
        RequestExit();
        return 0;
    case WM_DESTROY:
        if (luaInlineEdit_)
            CommitLuaInlineTextEdit(false);
        UnregisterNavigationHotkey();
        if (!exitRequested_)
        {
            ResetDesktopWindowResources();
            if (controlHwnd_ && IsWindow(controlHwnd_))
                SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);
            return 0;
        }
        SaveLayoutSlots();
        RemoveTrayIcon();
        ResetDesktopWindowResources();
        if (controlHwnd_ && IsWindow(controlHwnd_))
            KillTimer(controlHwnd_, kDesktopHostWatchTimerId);
        RestoreExplorerIcons();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
