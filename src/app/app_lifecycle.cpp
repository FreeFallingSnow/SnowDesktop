#include "app.h"

// Desktop host lifecycle.

DesktopApp::~DesktopApp()
{
    EndDesktopPassthroughHold(false);
    UnregisterDesktopPassthroughHotkey();
    ApplySystemTaskbarBackdrop(false, false,
        ResolveSystemTaskbarAppearance(dockSettings_));
    dockWindowTransition_.reset();
    dockWindowPreview_.reset();
    UnregisterFloatingDockHotkey();
    DestroyFloatingDockWindow();
    StopQuickNavigationAppIndexing();
    StopIconLoader();
    ClearQuickNavigationEverythingResults();
    widgetEngine_.reset();
    settingsWindow_.reset();
    for (DockRunningAppInfo& app : dockUnpinnedRunningApps_)
    {
        if (!app.iconBitmap) continue;
        EraseD2DIconCacheForBitmap(app.iconBitmap);
        DeleteObject(app.iconBitmap);
        app.iconBitmap = nullptr;
    }
    dockUnpinnedRunningApps_.clear();
    items_oo_.clear();
    containers_.clear();
    ClearMenuIcons();
    if (faFontHandle_)
    {
        RemoveFontMemResourceEx(faFontHandle_);
        faFontHandle_ = nullptr;
    }
    if (fluentIconFontHandle_)
    {
        RemoveFontMemResourceEx(fluentIconFontHandle_);
        fluentIconFontHandle_ = nullptr;
    }
    if (oleDragDropAdapter_)
        oleDragDropAdapter_->Detach();
}

/**
 * @brief 隐藏资源管理器桌面图标层
 *
 * 通过 ShowWindow(SW_HIDE) 隐藏桌面的 ListView 窗口（即图标层），
 * 并用窗口属性 kHiddenBySnowDesktopProp 标记此操作为 SnowDesktop 所为，
 * 以便后续恢复时判断。
 */
void DesktopApp::HideExplorerIcons()
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
void DesktopApp::RestoreExplorerIcons()
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
OleDragDropAdapter* DesktopApp::EnsureOleDragDropAdapter()
{
    if (!oleDragDropAdapter_)
    {
        oleDragDropAdapter_.Attach(
            new (std::nothrow) OleDragDropAdapter(this));
    }
    return oleDragDropAdapter_.Get();
}

void DesktopApp::RegisterOleDropTarget()
{
    if (!hwnd_ || !IsWindow(hwnd_) || dropTargetRegistered_)
        return;
    OleDragDropAdapter* adapter = EnsureOleDragDropAdapter();
    if (!adapter) return;
    dropTargetRegistered_ = SUCCEEDED(RegisterDragDrop(
        hwnd_, static_cast<IDropTarget*>(adapter)));
}

void DesktopApp::ResetDesktopWindowResources()
{
    EndDesktopPassthroughHold(false);
    UnregisterDesktopPassthroughHotkey();
    desktopBackdropCompositor_.Reset();
    if (dockWindowTransition_)
        dockWindowTransition_->Cancel();
    nativeGlassPanelReadyLogged_ = false;
    recycleBinPollState_->targetWindow = nullptr;
    if (hwnd_ && IsWindow(hwnd_))
    {
        UnregisterNavigationHotkey();
        UnregisterFloatingDockHotkey();
        KillTimer(hwnd_, kShellChangeTimerId);
        KillTimer(hwnd_, kDisplayTopologyRefreshTimerId);
        KillTimer(hwnd_, kRecycleBinPollTimerId);
        KillTimer(hwnd_, kWidgetRefreshTimerId);
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        KillTimer(hwnd_, kCollectionGroupTabDwellTimerId);
        KillTimer(hwnd_, kCollectionPopupAnimationTimerId);
        KillTimer(hwnd_, kLuaWidgetPanelAnimationTimerId);
        KillTimer(hwnd_, kPageNotifyTimerId);
        KillTimer(hwnd_, kDockLaunchBounceTimerId);
        KillTimer(hwnd_, kTaskbarRevealGuardTimerId);
        for (const auto& [timerId, _] : widgetTimerIds_)
            KillTimer(hwnd_, timerId);
        if (popupAnimation_.IsClosing())
            FinalizeCloseCollectionPopup();
        else if (popupWidgetIndex_ < widgets_.size() ||
                 dockFolderPopupOpen_)
            popupAnimation_.ShowImmediately();
        else
            popupAnimation_.ResetHidden();
        if (!luaWidgetPanelRequest_.widgetId.empty())
            FinalizeCloseLuaWidgetPanel();
        else
            luaWidgetPanelAnimation_.ResetHidden();
        if (dropTargetRegistered_)
            RevokeDragDrop(hwnd_);
    }
    StopDockForegroundMonitor();
    widgetTimerIds_.clear();
    dockLaunchBounces_.clear();
    nextWidgetTimerId_ = kWidgetTimerIdBase;
    dropTargetRegistered_ = false;

    if (shellChangeRegId_ != 0)
    {
        SHChangeNotifyDeregister(shellChangeRegId_);
        shellChangeRegId_ = 0;
    }

    DestroyDragHintWindow();
    DestroyFloatingDockWindow();
    DestroyQuickNavigationWindow();
    if (inputHwnd_ && IsWindow(inputHwnd_))
        DestroyWindow(inputHwnd_);
    inputHwnd_ = nullptr;
    dragRenderCache_.Reset();
    ResetCollectionPopupAnimationCache();
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    placeholderIconCache_.clear();
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
void DesktopApp::AttachWindowToDesktopHost(HWND host)
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
    desktopBackdropCompositor_.Reattach(hwnd_);
    if (inputHwnd_ && IsWindow(inputHwnd_))
        AttachInputWindowToDesktopHost(host);
    else
        CreateDesktopInputWindow(host);
}

/**
 * @brief 创建独立的键盘输入窗口。
 *
 * 输入窗口与渲染窗口属于同一个桌面宿主，但仅占 1x1 像素并放置在
 * 宿主客户区之外。它不参与 DirectComposition 渲染，只负责键盘焦点
 * 和全局热键，避免 WS_EX_LAYERED 渲染窗口的输入兼容问题。
 */
bool DesktopApp::CreateDesktopInputWindow(HWND host)
{
    if (inputHwnd_ && IsWindow(inputHwnd_))
    {
        AttachInputWindowToDesktopHost(host);
        return true;
    }

    inputHwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW,
        kInputWindowClassName, L"SnowDesktopInput",
        WS_CHILD | WS_VISIBLE,
        -32000, -32000, 1, 1,
        host, nullptr, instance_, this);
    if (!inputHwnd_)
        return false;

    AttachInputWindowToDesktopHost(host);
    {
        wchar_t buf[192];
        wsprintfW(buf, L"Input window=%p parent=%p style=0x%08X exStyle=0x%08X",
            inputHwnd_, GetParent(inputHwnd_),
            static_cast<unsigned>(GetWindowLongPtrW(inputHwnd_, GWL_STYLE)),
            static_cast<unsigned>(GetWindowLongPtrW(inputHwnd_, GWL_EXSTYLE)));
        WriteDiagnosticLogEntry(buf);
    }
    return true;
}

/**
 * @brief 将独立输入窗口重新挂载到当前桌面宿主。
 */
void DesktopApp::AttachInputWindowToDesktopHost(HWND host)
{
    if (!inputHwnd_ || !IsWindow(inputHwnd_) || !host || !IsWindow(host))
        return;

    if (GetParent(inputHwnd_) != host)
        SetParent(inputHwnd_, host);

    LONG_PTR style = GetWindowLongPtrW(inputHwnd_, GWL_STYLE);
    style &= ~WS_POPUP;
    style |= WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(inputHwnd_, GWL_STYLE, style);
    SetWindowPos(inputHwnd_, HWND_TOP, -32000, -32000, 1, 1,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
}

/**
 * @brief 将键盘焦点从分层渲染窗口转移到普通输入窗口。
 */
void DesktopApp::FocusDesktopInputWindow()
{
    if (inputHwnd_ && IsWindow(inputHwnd_))
        SetFocus(inputHwnd_);
    else if (hwnd_ && IsWindow(hwnd_))
        SetFocus(hwnd_);
}

/**
 * @brief 将隐藏输入窗口和 IMM 窗口锚定到宿主绘制的文本光标。
 *
 * 输入窗口平时位于屏幕外。宿主输入框获得焦点时，将这个 1x1 窗口移到
 * DirectWrite 光标位置，兼容依赖焦点窗口原点的输入法；同时显式设置组合
 * 窗口和候选窗口坐标，避免输入法回退到桌面左上角。
 */
void DesktopApp::UpdateHostInputImePosition()
{
    if (!inputHwnd_ || !IsWindow(inputHwnd_))
        return;

    RECT caret{};
    bool hasCaret = widgetEngine_ &&
        widgetEngine_->GetFocusedHostInputCaretRect(caret);
    if (!hasCaret)
    {
        for (const auto& container : containers_)
        {
            const auto* searchable =
                dynamic_cast<const ScrollingItemWidget*>(
                    container.get());
            if (searchable &&
                searchable->GetSearchCaretRect(caret))
            {
                hasCaret = true;
                break;
            }
        }
    }
    if (!hasCaret || !hwnd_ || !IsWindow(hwnd_))
    {
        SetWindowPos(inputHwnd_, nullptr, -32000, -32000, 1, 1,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
        return;
    }

    POINT origin{ caret.left, caret.top };
    ClientToScreen(hwnd_, &origin);
    HWND parent = GetParent(inputHwnd_);
    if (parent && IsWindow(parent))
        ScreenToClient(parent, &origin);

    SetWindowPos(inputHwnd_, HWND_TOP, origin.x, origin.y, 1, 1,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    const LONG caretHeight = std::max<LONG>(
        1, caret.bottom - caret.top);
    HIMC context = ImmGetContext(inputHwnd_);
    if (!context)
        return;

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = { 0, 0 };
    ImmSetCompositionWindow(context, &composition);

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = { 0, caretHeight };
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(inputHwnd_, context);
}

HWND DesktopApp::ShellDialogOwnerHwnd() const
{
    if (controlHwnd_ && IsWindow(controlHwnd_))
        return controlHwnd_;
    return hwnd_;
}

bool DesktopApp::CreateDesktopOverlayWindow()
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

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED,
        L"SnowDesktopWindow", L"SnowDesktop",
        WS_POPUP, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_,
        nullptr, nullptr, instance_, this);
    if (!hwnd_)
        return false;

    AttachWindowToDesktopHost(parent);
    if (!CreateDesktopInputWindow(parent))
        return fail();

    if (FAILED(dcompDevice_->CreateTargetForHwnd(hwnd_, FALSE, &dcompTarget_)))
        return fail();
    if (FAILED(dcompDevice_->CreateVisual(&dcompVisual_)))
        return fail();
    dcompTarget_->SetRoot(dcompVisual_.Get());
    if (FAILED(CreateOrResizeCompositionSurface()))
        return fail();
    if (desktopBackdropCompositor_.Initialize(hwnd_))
    {
        nativeGlassPanelReadyLogged_ = false;
        WriteDiagnosticLogEntry(L"Native desktop CompositionBackdropBrush initialized");
    }
    else
    {
        std::wstring message = L"Native desktop CompositionBackdropBrush unavailable: ";
        message += desktopBackdropCompositor_.LastError();
        WriteDiagnosticLogEntry(message.c_str());
    }

    if (HICON appIcon = LoadAppIcon())
    {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }

    RegisterOleDropTarget();
    ApplyNavigationHotkey();
    ApplyFloatingDockHotkey();
    ApplyDesktopPassthroughHotkey();
    RegisterShellChangeNotifications();
    SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);
    SetTimer(hwnd_, kWidgetRefreshTimerId, kWidgetRefreshIntervalMs, nullptr);
    SetTimer(hwnd_, kTaskbarRevealGuardTimerId,
        kTaskbarRevealGuardIntervalMs, nullptr);
    StartDockForegroundMonitor();

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
void DesktopApp::RecoverDesktopHostAfterExplorerRestart()
{
    if (exitRequested_)
        return;

    // TaskbarCreated can be dispatched re-entrantly by a shell COM call made
    // during painting. Destroying the overlay between BeginDraw and EndDraw
    // leaves the DComp surface permanently in SURFACE_BEING_RENDERED state.
    // The independent control timer will retry as soon as this frame unwinds.
    if (compositionPaintInProgress_)
        return;

    // These resources belong to the Explorer shell rather than to the custom
    // desktop window. Restore them even when the native desktop is selected.
    if (startupInitializationComplete_)
        AddTrayIcon(true);
    SetSystemTaskbarAutoHideEnabled(dockSettings_.systemTaskbarAutoHide);
    SetSystemTaskbarAlignmentCentered(dockSettings_.systemTaskbarAlignment == 1);
    if (controlHwnd_ && IsWindow(controlHwnd_))
        SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);

    // System-taskbar personalization is independent of the custom desktop.
    // Do not accidentally show SnowDesktop again when the user selected the
    // native desktop before Explorer restarted.
    if (!customDesktopVisible_)
        return;

    DesktopWindows current = FindDesktopWindows();
    if (!current.host || !IsWindow(current.host))
        return;

    desktopWindows_ = current;

    // A child HWND can survive the death of its cross-process Explorer parent
    // and be reparented to the newly created Progman. Its visibility flags then
    // look healthy even though the old DirectComposition target no longer
    // presents. Rebuild the complete overlay pipeline after TaskbarCreated.
    if (explorerDesktopRecreatePending_ && hwnd_ && IsWindow(hwnd_))
        DestroyWindow(hwnd_);

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
    explorerDesktopRecreatePending_ = false;
    GetWindowThreadProcessId(desktopWindows_.host,
        &desktopHostExplorerProcessId_);

    HideExplorerIcons();

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
void DesktopApp::WatchDesktopHost()
{
    if (exitRequested_)
        return;

    // Low-cost fallback for display-driver paths that do not broadcast the
    // usual display messages. This also catches a desktop child window that
    // Explorer left at the old virtual-screen bounds.
    PollDisplayTopology();

    if (!customDesktopVisible_)
        return;

    // A pending Explorer restart must rebuild the DComp target even if Windows
    // automatically reparented the old HWND and it still reports as visible.
    // This also keeps the blurred background alive while desktop icons are
    // hidden by double-click.
    if (explorerDesktopRecreatePending_ || !hwnd_ || !IsWindow(hwnd_))
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
    const bool inputMissing = !inputHwnd_ || !IsWindow(inputHwnd_);
    const bool inputDetached = currentHost && IsWindow(currentHost) &&
        inputHwnd_ && IsWindow(inputHwnd_) && GetParent(inputHwnd_) != currentHost;

    if (parentMissing || knownHostMissing || knownListViewMissing ||
        hostChanged || parentDetached || inputMissing || inputDetached)
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
void DesktopApp::InvalidateAllWidgetSlots()
{
    for (auto& c : containers_)
        c->InvalidateSlots();
}

void DesktopApp::RequestExit()
{
    if (exitRequested_)
        return;
    exitRequested_ = true;
    StopQuickNavigationAppIndexing();
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

void DesktopApp::RequestRestart()
{
    wchar_t exePath[MAX_PATH * 4]{};
    const DWORD pathLen = GetModuleFileNameW(
        nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (pathLen == 0 || pathLen >= std::size(exePath))
    {
        MessageBoxW(controlHwnd_ ? controlHwnd_ : hwnd_,
            _LW("app.run.no_path"), _LW("app.run.restart_failed"),
            MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring commandLine = L"\"";
    commandLine.append(exePath, pathLen);
    commandLine += L"\" --wait-for-pid=";
    commandLine += std::to_wstring(GetCurrentProcessId());
    std::vector<wchar_t> commandLineBuffer(commandLine.begin(), commandLine.end());
    commandLineBuffer.push_back(L'\0');

    std::wstring workingDir(exePath, pathLen);
    const size_t slash = workingDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        workingDir.resize(slash);
    else
        workingDir.clear();

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(
        exePath,
        commandLineBuffer.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &startupInfo,
        &processInfo);

    if (!created)
    {
        const DWORD error = GetLastError();
        std::wstring message =
            _LFW("app.run.restart_error", std::to_wstring(error));
        MessageBoxW(controlHwnd_ ? controlHwnd_ : hwnd_, message.c_str(),
            _LW("app.run.restart_failed"), MB_OK | MB_ICONERROR);
        return;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    RequestExit();
}
