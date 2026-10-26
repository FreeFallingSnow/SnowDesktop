#include "platform/force_restart_hotkey.h"
#include "app/app.h"
#include "app/input/software_desktop_hotkey.h"
#include "app/popup/categorized_popup_scope.h"
#include "app/dock/dock_platform_helpers.h"
#include "desktop/desktop_keyboard_rules.h"
#include "desktop/desktop_input_activation.h"
#include "desktop/desktop_layer_recovery.h"
#include "widget/settings/widget_engine_settings_backend.h"
#include "widget/settings/widget_settings_service.h"
#include "platform/http_runtime.h"
#include "platform/shell_overlay_window.h"
#include "platform/foreground_fullscreen.h"
#include "platform/owned_window_shutdown.h"
#include "shell/shell_extension_service.h"

// Desktop host lifecycle.

DesktopApp::DesktopApp() = default;

DesktopApp::~DesktopApp()
{
    FlushQueuedLayoutSave();
    snowdesktop::EmergencyRestartHotkey().Stop();
    StopRecycleBinWatcher();
    dockExternalMinimize_.reset();
    snowdesktop::SoftwareDesktopHotkey().Unregister();
    dataCleanupWorker_.Stop();
    if(systemDataProvider_)systemDataProvider_->Controls()->SetWake({});
    systemPanel_.reset();
    CancelStatusBarActivation();
    dockAppBars_.reset();
    statusBar_.reset();
    ResetDockTrayPopup();
    dockTrayService_.reset();
    // The shared service is constructed before the caches its asynchronous
    // scans use. Stop it before CRT static destruction reverses that order.
    snowdesktop::shell_extensions::SharedMenuService().Shutdown();
    if (largeIconAssets_) largeIconAssets_->Stop();
    uiAnimationScheduler_.CancelAll();
    floatingDockPromotionInputToken_ = 0;
    floatingDockInputDeactivated_ = false;
    dockWindowActivationObservationToken_ = 0;
    dockWindowActivationObservations_.clear();
    popupAnimationFrameToken_ = 0;
    popupFanScrollFrameToken_ = 0;
    luaPanelAnimationFrameToken_ = 0;
    quickNavigationAnimationFrameToken_ = 0;
    dockBounceAnimationFrameToken_ = 0;
    pageNotifyAnimationFrameToken_ = 0;
    navHotEdgeHintToken_ = 0;
    pointerRecoveryFrameToken_ = 0;
    floatingDockHoverTailToken_ = 0;
    desktopPointerPresentPending_ = false;
    floatingDockPointerPresentPending_ = false;
    pageNotifyFadeOutToken_ = 0;
    shellLaunchWorker_.Stop();
    shellElevationWorker_.Stop();
    StopShellFileOperationWorker();
    StopUrlDropDownloadWorker();
    StopWebsiteIconWorker();
    StopSteamWorkshopWatcher();
    EndDesktopPassthrough(false);
    UnregisterDesktopPassthroughHotkey();
    ApplySystemTaskbarBackdrop(false, false,
        ResolveSystemTaskbarAppearance(dockSettings_));
    if (dockWindowTransition_)
    {
        dockWindowTransition_->SetPresentationCallback({});
        dockWindowTransition_->SetOcclusionRectsProvider({});
        dockWindowTransition_->SetDiagnosticCallback({});
    }
    dockWindowTransition_.reset();
    dockWindowPreview_.reset();
    UnregisterFloatingDockHotkey();
    DestroyDragPreviewWindow();
    DestroyFloatingDockWindow();
    quickNavigationEverythingSearch_.Stop();
    StopQuickNavigationAppIndexing();
    StopDemoIconLoader();
    StopIconLoader();
    ClearQuickNavigationEverythingResults();
    widgetAccessibilityProvider_.reset();
    ShutdownSettingsInfrastructure();
    widgetEngine_.reset();
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
    // Startup can fail before the control window exists. Retire every host
    // window before the startup failure dialog starts another message loop.
    snowdesktop::RetireOwnedWindows({ &inputHwnd_, &hwnd_, &controlHwnd_ });
}

void DesktopApp::ShutdownSettingsInfrastructure() noexcept
{
    shellModelWork_.Cancel(L"settings-auto-start");
    settingsWindowOpening_ = false;
    settingsWindowOpenRequest_.Cancel();
    steamEntitlementService_.reset();
    settingsWindow_.reset();
    if (widgetSettingsService_)
    {
        widgetSettingsService_->CloseAll();
        widgetSettingsService_.reset();
    }
    widgetSettingsBackend_.reset();
    if (settingsController_)
        (void)settingsController_->CloseSession();
    settingsController_.reset();
    settingsHostActions_.reset();
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
    // Shell COM calls can dispatch host-watch messages during bootstrap.
    // Never remove the usable native desktop before our first frame is ready.
    if (desktopStartupPresentationPending_)
        return;
    explorerIconVisibility_.Hide(FindDesktopWindows().listView);
}

/**
 * @brief 恢复显示资源管理器桌面图标
 *
 * 重新查找 Explorer 当前的图标层；切回原生桌面时明确请求显示图标。
 * 确认图标层可见后才清除恢复请求，窗口重建期间由宿主监视器继续处理。
 */
void DesktopApp::RestoreExplorerIcons(bool force)
{
    explorerIconVisibility_.RequestRestore(force);
    explorerIconVisibility_.TryRestore(FindDesktopWindows().listView);
}

void DesktopApp::RestoreExplorerIconsOnExit()
{
    if (!explorerIconVisibility_.RestoreWhenReady(
            [] { return FindDesktopWindows().listView; }))
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Runtime,
            L"Explorer icon restoration incomplete at exit: current desktop layer is not visible",
            DiagnosticLogLevel::Warning);
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
    // Dock hosts are application-owned top-level windows. Recreating the
    // Explorer-owned overlay must not retire their monitor-local reveal state
    // or reset the shared strip to hidden. Device recovery and exit still
    // release every Dock window and its composition resources.
    // WM_DESTROY can arrive before TaskbarCreated marks the replacement.
    DWORD currentExplorerProcessId = 0;
    GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr),
        &currentExplorerProcessId);
    const bool replacingExplorer = explorerDesktopRecreatePending_ ||
        (desktopHostExplorerProcessId_ &&
            currentExplorerProcessId != desktopHostExplorerProcessId_);
    const bool preserveDockHosts = replacingExplorer &&
        !exitRequested_ && !graphicsDeviceRecovery_.Pending();
    if (widgetAccessibilityProvider_ && hwnd_)
        widgetAccessibilityProvider_->DetachWindow(hwnd_);
    EndDesktopPassthrough(false);
    UnregisterDesktopPassthroughHotkey();
    desktopBackdropCompositor_.Reset();
    if (dockWindowTransition_)
        dockWindowTransition_->Cancel();
    CancelAllDockWindowActivationObservations();
    CancelPendingExternalOleDragLeave();
    CancelCollectionPopupDwell();
    CancelPopupHover();
    CancelCollectionGroupTabDwell();
    nativeGlassPanelReadyLogged_ = false;
    if (hwnd_ && IsWindow(hwnd_))
    {
        UnregisterNavigationHotkey();
        UnregisterFloatingDockHotkey();
        KillTimer(hwnd_, kShellChangeTimerId);
        KillTimer(hwnd_, kDisplayTopologyRefreshTimerId);
        KillTimer(hwnd_, kRecycleBinPollTimerId);
        KillTimer(hwnd_, kWidgetRefreshTimerId);
        StopRecycleBinWatcher();
        StopSteamWorkshopWatcher();
        KillTimer(hwnd_, kOleDragUiPumpTimerId);
        CancelUiAnimationFrame();
        if (pageNotifyFadeOutToken_)
            uiAnimationScheduler_.Cancel(pageNotifyFadeOutToken_);
        pageNotifyFadeOutToken_ = 0;
        if (navHotEdgeHintToken_)
            uiAnimationScheduler_.Cancel(navHotEdgeHintToken_);
        navHotEdgeHintToken_ = 0;
        navHotEdgeHintVisible_ = false;
        navHotEdgeHover_ = false;
        navHoverSide_ = 0;
        KillTimer(hwnd_, kTaskbarRevealGuardTimerId);
        for (const auto& [timerId, _] : widgetTimerIds_)
            uiAnimationScheduler_.Cancel(timerId);
        if (popupAnimation_.IsClosing())
            FinalizeCloseCollectionPopup();
        else if (popupWidgetIndex_ < widgets_.size() ||
                 dockFolderPopupOpen_)
            popupAnimation_.ShowImmediately();
        else
            popupAnimation_.ResetHidden();
        if (!luaWidgetPanelRequest_.widgetId.empty())
            FinalizeCloseLuaWidgetPanel(false);
        else
        {
            pendingLuaWidgetPanelOpen_.reset();
            luaWidgetPanelAnimation_.ResetHidden();
        }
        if (dropTargetRegistered_)
            RevokeDragDrop(hwnd_);
    }
    StopDockForegroundMonitor();
    widgetTimerIds_.clear();
    dockLaunchBounces_.clear();
    dropTargetRegistered_ = false;

    folderNotifications_.Clear();

    if (shellChangeRegId_ != 0)
    {
        SHChangeNotifyDeregister(shellChangeRegId_);
        shellChangeRegId_ = 0;
    }

    DestroyDragHintWindow();
    DestroyDragPreviewWindow();
    DestroyFloatingPopupWindow();
    if (!preserveDockHosts)
        DestroyFloatingDockWindow();
    DestroyQuickNavigationWindow();
    if (inputHwnd_ && IsWindow(inputHwnd_))
        DestroyWindow(inputHwnd_);
    inputHwnd_ = nullptr;
    dragRenderCache_.Reset();
    ResetCollectionPopupAnimationCache();
    ResetLuaWidgetPanelAnimationCache();
    ResetPageNotifyTextCache();
    popupAnimationOverlay_.visual.Reset();
    popupAnimationOverlay_.effect.Reset();
    luaWidgetPanelAnimationOverlay_.visual.Reset();
    luaWidgetPanelAnimationOverlay_.effect.Reset();
    pageNotifyAnimationOverlay_.visual.Reset();
    pageNotifyAnimationOverlay_.effect.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    placeholderIconCache_.clear();
    dockFolderBitmapCache_.Retain([](const auto&) { return false; });
    ResetDesktopWidgetComposition();
    ResetDesktopForegroundComposition();
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
 * 同时调整位置到虚拟屏幕原点；启动首帧就绪前保持窗口隐藏。
 */
void DesktopApp::AttachWindowToDesktopHost(HWND host)
{
    if (!hwnd_ || !IsWindow(hwnd_) || !host || !IsWindow(host))
        return;

    if (GetParent(hwnd_) != host)
        SetParent(hwnd_, host);

    LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
    const bool showDesktop =
        !desktopStartupPresentationPending_ && customDesktopVisible_;
    style &= ~(WS_POPUP | WS_VISIBLE);
    style |= WS_CHILD | (showDesktop ? WS_VISIBLE : 0);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);

    POINT origin{ virtualLeft_, virtualTop_ };
    ScreenToClient(host, &origin);
    SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_,
        SWP_FRAMECHANGED | SWP_NOACTIVATE |
            (showDesktop ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    desktopBackdropCompositor_.Reattach(hwnd_);
    if (inputHwnd_ && IsWindow(inputHwnd_))
        AttachInputWindowToDesktopHost(host);
    else
        CreateDesktopInputWindow(host);
}

/**
 * @brief 创建独立的键盘输入窗口。
 *
 * 输入窗口是 SnowDesktop 自己拥有的 1x1 顶层工具窗口。它平时位于
 * 屏幕之外，不参与 DirectComposition 渲染，只负责键盘焦点和全局热键。
 * 不把它挂成 Explorer 桌面宿主的跨进程子窗口，确保激活窗口与键盘焦点
 * 属于同一输入队列。
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
        WS_POPUP | WS_VISIBLE,
        -32000, -32000, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!inputHwnd_)
        return false;

    AttachInputWindowToDesktopHost(host);
    {
        wchar_t buf[192];
        wsprintfW(buf, L"Input window=%p parent=%p style=0x%08X exStyle=0x%08X",
            inputHwnd_, GetParent(inputHwnd_),
            static_cast<unsigned>(GetWindowLongPtrW(inputHwnd_, GWL_STYLE)),
            static_cast<unsigned>(GetWindowLongPtrW(inputHwnd_, GWL_EXSTYLE)));
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Runtime, buf);
    }
    return true;
}

/**
 * @brief 保持独立输入窗口为 SnowDesktop 顶层工具窗口。
 */
void DesktopApp::AttachInputWindowToDesktopHost(HWND host)
{
    if (!inputHwnd_ || !IsWindow(inputHwnd_) || !host || !IsWindow(host))
        return;

    LONG_PTR style = GetWindowLongPtrW(inputHwnd_, GWL_STYLE);
    style &= ~WS_CHILD;
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongPtrW(inputHwnd_, GWL_STYLE, style);
    SetWindowPos(inputHwnd_, HWND_TOP, -32000, -32000, 1, 1,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
}

/**
 * @brief 将键盘焦点从分层渲染窗口转移到普通输入窗口。
 */
void DesktopApp::FocusDesktopInputWindow()
{
    const HWND target = inputHwnd_ && IsWindow(inputHwnd_)
        ? inputHwnd_
        : nullptr;
    if (!target)
    {
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Runtime, L"Desktop input proxy unavailable; render child activation refused",
            DiagnosticLogLevel::Warning);
        return;
    }
    (void)FocusKeyboardWindow(
        target, true, L"Desktop input proxy");
}

bool DesktopApp::FocusKeyboardWindow(
    HWND target, bool requestForeground,
    const wchar_t* diagnosticLabel, bool deliberateFullscreenExit)
{
    if (!target || !IsWindow(target))
        return false;

    HWND activationWindow = GetAncestor(target, GA_ROOT);
    if (!activationWindow)
        activationWindow = target;
    // Only the popup presenter may reveal this host after a usable frame.
    // Generic Shell/focus requests must not revive a closed or failed popup.
    if (activationWindow == floatingPopupHwnd_ &&
        !IsWindowVisible(activationWindow))
        return false;
    const DWORD currentThread = GetCurrentThreadId();

    struct FocusObservation
    {
        HWND foreground = nullptr;
        HWND foregroundFocus = nullptr;
        DWORD foregroundThread = 0;
        bool foregroundFocusKnown = false;
        bool ready = false;
    };
    const auto observeFocus = [target, activationWindow,
                                  currentThread, requestForeground]() {
        FocusObservation observation;
        observation.foreground = GetForegroundWindow();
        observation.foregroundThread = observation.foreground
            ? GetWindowThreadProcessId(
                observation.foreground, nullptr)
            : 0;
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (observation.foregroundThread != 0 &&
            GetGUIThreadInfo(
                observation.foregroundThread, &info))
        {
            observation.foregroundFocusKnown = true;
            observation.foregroundFocus = info.hwndFocus;
        }
        else if (observation.foregroundThread == currentThread)
        {
            observation.foregroundFocusKnown = true;
            observation.foregroundFocus = GetFocus();
        }
        observation.ready = snowdesktop::desktop_keyboard_rules::
            IsForegroundFocusReady(
                observation.foregroundFocusKnown,
                observation.foregroundFocus == target,
                requestForeground,
                observation.foreground == activationWindow);
        return observation;
    };
    const auto requestFocus = [this, target, activationWindow,
                                  requestForeground, deliberateFullscreenExit]() {
        const bool deliberateExitAllowed = deliberateFullscreenExit &&
            !FullscreenBlocksDockReveal(DockRevealSource::Hotkey, nullptr);
        return snowdesktop::fullscreen::GuardFocusRequest(deliberateExitAllowed,
            snowdesktop::fullscreen::ObserveForeground, [=]() {
                if (requestForeground)
                {
                    ShowWindow(target, SW_SHOWNOACTIVATE);
                    (void)SetForegroundWindow(activationWindow);
                    (void)SetActiveWindow(activationWindow);
                }
                (void)SetFocus(target);
            });
    };

    FocusObservation observation = observeFocus();
    if (observation.ready)
    {
        TraceDesktopInteraction(L"focus-already-ready", target);
        return true;
    }
    TraceDesktopInteraction(L"focus-request", target);
    if (!requestFocus())
    {
        TraceDesktopInteraction(L"focus-suppressed-fullscreen", target);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Runtime, L"Keyboard focus request suppressed: foreground fullscreen");
        return false;
    }
    observation = observeFocus();
    TraceDesktopInteraction(L"focus-result", target);
    if (observation.ready)
        return true;

    // Activation permission and cross-thread delivery belong to User32. A
    // failed/queued foreground request must not couple our input queue to an
    // unrelated application: synchronous activation can then block both.

    wchar_t message[384]{};
    swprintf_s(
        message,
        L"%ls focus FAILED target=%p activation=%p foreground=%p "
        L"foregroundThread=%lu foregroundFocus=%p focusKnown=%d "
        L"localFocus=%p",
        diagnosticLabel ? diagnosticLabel : L"Keyboard window",
        target, activationWindow, observation.foreground,
        observation.foregroundThread,
        observation.foregroundFocus,
        observation.foregroundFocusKnown ? 1 : 0,
        GetFocus());
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Runtime, message, DiagnosticLogLevel::Warning);
    return false;
}

bool DesktopApp::EnsureFloatingDockInputWindow()
{
    if (floatingDockInputHwnd_ &&
        IsWindow(floatingDockInputHwnd_))
        return true;
    if (!instance_)
        return false;

    floatingDockInputHwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kInputWindowClassName,
        L"SnowDesktopFloatingDockInput",
        WS_POPUP,
        -32000, -32000, 1, 1,
        nullptr, nullptr, instance_, this);
    return floatingDockInputHwnd_ != nullptr;
}

void DesktopApp::BeginFloatingDockKeyboardSession(bool deliberateFullscreenExit)
{
    floatingDockInputDeactivated_ = false;
    if (deliberateFullscreenExit && FullscreenBlocksDockReveal(DockRevealSource::Hotkey, nullptr)) return;
    if (!deliberateFullscreenExit && snowdesktop::fullscreen::ObserveForeground())
        return;
    if (floatingDockKeyboardSessionActive_)
    {
        RefocusFloatingDockKeyboardSession(deliberateFullscreenExit);
        return;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground)
    {
        if (HWND root = GetAncestor(foreground, GA_ROOT))
            foreground = root;
    }
    DWORD foregroundProcess = 0;
    if (foreground)
        GetWindowThreadProcessId(
            foreground, &foregroundProcess);
    if (foreground &&
        foregroundProcess != GetCurrentProcessId() &&
        IsDockTaskWindow(foreground))
    {
        floatingDockLogicalForegroundWindow_ = foreground;
    }
    else
    {
        HWND previous = dockPreviousForegroundWindow_.load();
        if (previous)
        {
            if (HWND root = GetAncestor(previous, GA_ROOT))
                previous = root;
        }
        DWORD previousProcess = 0;
        if (previous)
            GetWindowThreadProcessId(
                previous, &previousProcess);
        floatingDockLogicalForegroundWindow_ =
            shellFileOperationInFlight_ > 0 &&
                previous &&
                previousProcess != GetCurrentProcessId() &&
                IsDockTaskWindow(previous)
            ? previous : nullptr;
    }

    if (!EnsureFloatingDockInputWindow())
    {
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Runtime,
            L"Floating Dock input proxy creation FAILED", DiagnosticLogLevel::Warning);
        return;
    }
    floatingDockKeyboardSessionActive_ = true;
    ShowWindow(
        floatingDockInputHwnd_, SW_SHOWNOACTIVATE);
    RefocusFloatingDockKeyboardSession(deliberateFullscreenExit);
}

void DesktopApp::RefocusFloatingDockKeyboardSession(bool deliberateFullscreenExit)
{
    if (deliberateFullscreenExit && FullscreenBlocksDockReveal(DockRevealSource::Hotkey, nullptr)) return;
    // Menu/file-operation completions must not reclaim the keyboard after the
    // user returned to a fullscreen application. Only an allowed explicit
    // Dock hotkey can deliberately exit fullscreen at the start of a session.
    if (!deliberateFullscreenExit && snowdesktop::fullscreen::ObserveForeground())
        return;
    if (!snowdesktop::floating_dock_rules::
            ShouldRefocusFloatingDockKeyboardSession(
                floatingDockVisible_,
                floatingDockKeyboardSessionActive_,
                shellFileOperationInFlight_,
                shellPopupMenuLayerDepth_) ||
        !EnsureFloatingDockInputWindow())
        return;

    // Capture a genuine external switch before the input proxy becomes the
    // real foreground window again. Internal/menu foreground changes resolve
    // back to the existing logical snapshot.
    ResolveDockSemanticForegroundWindow();

    (void)FocusKeyboardWindow(floatingDockInputHwnd_, true,
        L"Floating Dock input proxy", deliberateFullscreenExit);
}

void DesktopApp::EndFloatingDockKeyboardSession(
    FloatingDockCloseFocusPolicy focusPolicy)
{
    floatingDockInputDeactivated_ = false;
    if (!floatingDockKeyboardSessionActive_)
    {
        floatingDockLogicalForegroundWindow_ = nullptr;
        return;
    }

    const HWND currentForeground = GetForegroundWindow();
    DWORD currentForegroundProcess = 0;
    if (currentForeground)
        GetWindowThreadProcessId(currentForeground, &currentForegroundProcess);
    const bool mayRestore = currentForegroundProcess == GetCurrentProcessId();
    HWND restoreWindow =
        focusPolicy ==
                FloatingDockCloseFocusPolicy::RestorePrevious
            ? ResolveDockSemanticForegroundWindow()
            : nullptr;
    DWORD restoreProcess = 0;
    if (restoreWindow)
        GetWindowThreadProcessId(
            restoreWindow, &restoreProcess);
    if (!restoreWindow ||
        restoreProcess == GetCurrentProcessId() ||
        !IsDockTaskWindow(restoreWindow))
    {
        restoreWindow = nullptr;
    }
    floatingDockKeyboardSessionActive_ = false;
    if (floatingDockInputHwnd_ &&
        IsWindow(floatingDockInputHwnd_))
    {
        ShowWindow(floatingDockInputHwnd_, SW_HIDE);
    }
    floatingDockLogicalForegroundWindow_ = nullptr;

    if (focusPolicy ==
            FloatingDockCloseFocusPolicy::RestorePrevious &&
        mayRestore && restoreWindow && IsWindow(restoreWindow) &&
        !IsIconic(restoreWindow))
    {
        snowdesktop::fullscreen::GuardFocusRequest(false,
            snowdesktop::fullscreen::ObserveForeground,
            [restoreWindow]() { SetForegroundWindow(restoreWindow); });
    }
}

void DesktopApp::RestoreInteractionInputFocus(bool pointerPress)
{
    snowdesktop::desktop_input_activation::RestoreInteractionFocus(
        systemPanel_ && systemPanel_->IsOpen(), pointerPress,
        [this] { systemPanel_->Hide(); },
        [this] {
            if (floatingDockKeyboardSessionActive_ &&
                floatingDockVisible_)
                RefocusFloatingDockKeyboardSession();
            else
                FocusDesktopInputWindow();
        });
}

HWND DesktopApp::ResolveDockSemanticForegroundWindow()
{
    HWND actual = GetForegroundWindow();
    if (!actual || !IsWindow(actual))
        actual = dockForegroundWindow_.load();
    if (actual)
    {
        if (HWND root = GetAncestor(actual, GA_ROOT))
            actual = root;
    }
    if (!floatingDockKeyboardSessionActive_)
        return actual;

    DWORD processId = 0;
    if (actual)
        GetWindowThreadProcessId(actual, &processId);
    const bool ownedByCurrentProcess =
        processId != 0 &&
        processId == GetCurrentProcessId();
    const bool taskWindow =
        actual && IsDockTaskWindow(actual);
    if (snowdesktop::floating_dock_rules::
            ShouldUseFloatingDockLogicalForeground(
                true,
                ownedByCurrentProcess,
                shellFileOperationInFlight_ > 0,
                taskWindow))
    {
        return floatingDockLogicalForegroundWindow_ &&
                IsWindow(
                    floatingDockLogicalForegroundWindow_)
            ? floatingDockLogicalForegroundWindow_
            : nullptr;
    }

    if (actual && !ownedByCurrentProcess && taskWindow)
        floatingDockLogicalForegroundWindow_ = actual;
    return actual;
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
    // IMM positioning must use the same proxy that receives the composition
    // messages. Floating Dock keyboard sessions focus their own input window.
    HWND imeInputHwnd = GetFocus();
    if (!imeInputHwnd || (imeInputHwnd != inputHwnd_ &&
        imeInputHwnd != floatingDockInputHwnd_))
    {
        imeInputHwnd = floatingDockKeyboardSessionActive_ &&
                floatingDockVisible_ && floatingDockInputHwnd_ &&
                IsWindow(floatingDockInputHwnd_)
            ? floatingDockInputHwnd_
            : inputHwnd_;
    }
    if (!imeInputHwnd || !IsWindow(imeInputHwnd))
        return;

    RECT caret{};
    bool hasCaret = widgetEngine_ &&
        widgetEngine_->GetFocusedHostInputCaretRect(caret);
    if (!hasCaret)
        if (auto* view = GetCategorizedPopupView(); view && view->IsSearchFocused())
        {
            const RECT popup = GetCollectionPopupRect(*GetOpenPopupWidget());
            CategorizedPopupScope scope(view, GetCategorizedPopupFrame(popup));
            hasCaret = view->GetSearchCaretRect(caret);
        }
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
        SetWindowPos(imeInputHwnd, nullptr, -32000, -32000, 1, 1,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
        return;
    }

    POINT origin{ caret.left, caret.top };
    ClientToScreen(hwnd_, &origin);
    HWND parent = GetParent(imeInputHwnd);
    if (parent && IsWindow(parent))
        ScreenToClient(parent, &origin);

    SetWindowPos(imeInputHwnd, HWND_TOP, origin.x, origin.y, 1, 1,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    const LONG caretHeight = std::max<LONG>(
        1, caret.bottom - caret.top);
    HIMC context = ImmGetContext(imeInputHwnd);
    if (!context)
        return;

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = { 0, 0 };
    ImmSetCompositionWindow(context, &composition);

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = { 0, caretHeight };
    candidate.rcArea = { 0, 0, 1, caretHeight };
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(imeInputHwnd, context);
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

    hwnd_ = snowdesktop::CreateShellOverlayWindowEx(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED,
        L"SnowDesktopWindow", L"SnowDesktop",
        WS_POPUP, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_,
        nullptr, nullptr, instance_, this);
    if (!hwnd_)
        return false;

    if (widgetAccessibilityProvider_)
        widgetAccessibilityProvider_->AttachWindow(hwnd_);

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
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Runtime, L"Native desktop CompositionBackdropBrush initialized");
    }
    else
    {
        std::wstring message = L"Native desktop CompositionBackdropBrush unavailable: ";
        message += desktopBackdropCompositor_.LastError();
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Runtime, message.c_str(), DiagnosticLogLevel::Warning);
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
    StartDemoIconLoader();
    StartSteamWorkshopWatcher();
    SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);
    SetTimer(hwnd_, kWidgetRefreshTimerId, kWidgetRefreshIntervalMs, nullptr);
    SetTimer(hwnd_, kTaskbarRevealGuardTimerId,
        kTaskbarRevealGuardIntervalMs, nullptr);
    StartDockForegroundMonitor();

    // Every replacement overlay is preceded by ResetDesktopWindowResources,
    // which retires the scheduler tokens owned by the previous HWND. Rebind
    // here, after the new host is fully initialized, so both display-topology
    // recovery and Explorer recreation after system resume restore Lua widget
    // refresh, named-schedule and animation deadlines.
    if (widgetEngine_)
        widgetEngine_->RebindHostTimers();

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ReconcileDesktopHoverState(
        snowdesktop::desktop_hover_rules::
            ReconcileMode::AllowImmediateActivation);
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
    // Bootstrap owns initial attachment; Shell COM calls can dispatch this
    // callback while the main thread is intentionally detached from Explorer.
    if (exitRequested_ || desktopStartupPresentationPending_ ||
        graphicsDeviceRecovery_.Pending())
        return;

    // TaskbarCreated can be dispatched re-entrantly by a shell COM call made
    // during painting. Destroying the overlay between BeginDraw and EndDraw
    // leaves the DComp surface permanently in SURFACE_BEING_RENDERED state.
    // The independent control timer will retry as soon as this frame unwinds.
    if (compositionPaintInProgress_)
        return;

    EndDesktopPassthrough();

    // These resources belong to the Explorer shell rather than to the custom
    // desktop window. Restore the tray icon even when the native desktop is
    // selected, but never push cached taskbar settings back into Windows here.
    if (startupInitializationComplete_)
        AddTrayIcon(true);
    SyncSystemTaskbarSettingsFromWindows();
    if (controlHwnd_ && IsWindow(controlHwnd_))
        SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);

    // System-taskbar personalization is independent of the custom desktop.
    // Do not accidentally show SnowDesktop again when the user selected the
    // native desktop before Explorer restarted.
    if (!customDesktopVisible_)
    {
        if (explorerIconVisibility_.RestorePending())
            RestoreExplorerIcons();
        return;
    }

    // Reuse the new Explorer's own desktop host. Creating a wallpaper WorkerW
    // here would also disturb other desktop clients after a real Shell restart.
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
        StartRecycleBinWatcher();
        StartSteamWorkshopWatcher();
    }
    else
    {
        ResetDesktopWindowResources();
        if (!CreateDesktopOverlayWindow())
            return;
        StartRecycleBinWatcher();
        StartSteamWorkshopWatcher();
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
    if (exitRequested_ || desktopStartupPresentationPending_)
        return;

    // Low-cost fallback for display-driver paths that do not broadcast the
    // usual display messages. This also catches a desktop child window that
    // Explorer left at the old virtual-screen bounds.
    PollDisplayTopology();

    if (!customDesktopVisible_)
    {
        if (explorerIconVisibility_.RestorePending())
            RestoreExplorerIcons();
        return;
    }

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

    if (parentMissing || knownHostMissing || knownListViewMissing ||
        hostChanged || parentDetached || inputMissing)
    {
        RecoverDesktopHostAfterExplorerRestart();
        return;
    }

    if (current.listView && IsWindow(current.listView) && IsWindowVisible(current.listView))
    {
        desktopWindows_ = current;
        HideExplorerIcons();
    }

    // Explorer can raise its still-valid DefView without changing our parent,
    // HWND or bounds. Hiding its ListView alone leaves that view in front of
    // the renderer and capable of receiving the desktop's mouse input.
    namespace layer = snowdesktop::desktop_layer_recovery;
    const bool interactionActive = mouseDown_ || marqueeActive_ ||
        middleButtonWidgetMove_ || detailColumnResizeActive_ ||
        luaWidgetPanelMouseDown_ || widgetAction_ != WidgetAction::None ||
        dragSession_.HasContext() || dragDropController_.IsTransportActive() ||
        GetCapture() != nullptr || HasActiveContextMenuSession();
    const auto recovery = layer::TryRestore(hwnd_, currentHost, current.defView,
        {customDesktopVisible_, !desktopPassthroughActive_ &&
            !compositionPaintInProgress_ && !updatingDisplayTopology_ && !reloading_,
            interactionActive});
    if (recovery.status == layer::Status::Restored)
    {
        desktopBackdropCompositor_.Reattach(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        BeginDesktopInteractionTrace(L"desktop-layer-recovered");
    }
    static ULONGLONG nextFailureDiagnostic = 0;
    const ULONGLONG now = GetTickCount64();
    if (recovery.status == layer::Status::Restored ||
        (recovery.status == layer::Status::Failed && now >= nextFailureDiagnostic))
    {
        const bool restored = recovery.status == layer::Status::Restored;
        nextFailureDiagnostic = restored ? 0 : now + 30000;
        wchar_t message[384]{};
        swprintf_s(message,
            L"DesktopLayerRecovery result=%ls render=%p host=%p defView=%p previous=%p insertAfter=%p error=%lu",
            restored ? L"restored" : L"failed", hwnd_, currentHost,
            current.defView, recovery.previous, recovery.insertAfter, recovery.error);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Input, message,
            restored ? DiagnosticLogLevel::Info : DiagnosticLogLevel::Warning);
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
    UpdateWidgetHoverExpansion(lastMousePoint_);
    for (auto& c : containers_)
    {
        c->InvalidateSlots();
        // Changing the global title-bar position can restore a saved fold.
        // Do not retain a search caret inside content that just became hidden.
        auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
        if (searchable && searchable->IsCollapsed())
            searchable->SetSearchFocused(false);
    }
    if (keyboardNavInsideWidget_ && keyboardNavWidgetIndex_ < widgets_.size() &&
        IsWidgetCollapsed(widgets_[keyboardNavWidgetIndex_]))
        SelectWidgetOnly(keyboardNavWidgetIndex_);
}

void DesktopApp::RequestExit()
{
    if (exitRequested_)
        return;
    if (settingsWindow_ && settingsWindow_->IsVisible() &&
        !settingsWindow_->FlushPendingChanges())
    {
        return;
    }
    CompleteExitRequest();
}

void DesktopApp::CompleteExitRequest()
{
    if (exitRequested_) return;
    exitRequested_ = true;
    dataCleanupWorker_.Stop();
    // A confirmation may be pumping messages inside task dispatch. Invalidate
    // tasks now, keeping the engine alive until that modal stack has unwound.
    if (widgetEngine_) widgetEngine_->BeginTaskShutdown();
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Runtime, (L"Application exit begin: pid=" +
        std::to_wstring(GetCurrentProcessId()) + L" restart=" +
        std::to_wstring(preparedRestart_ != nullptr)).c_str());
    shellLaunchWorker_.Stop();
    shellElevationWorker_.Stop();
    StopShellFileOperationWorker();
    StopUrlDropDownloadWorker();
    StopWebsiteIconWorker();
    quickNavigationEverythingSearch_.Stop();
    StopQuickNavigationAppIndexing();
    StopDemoIconLoader();
    StopIconLoader();
    RestoreExplorerIconsOnExit();
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

bool DesktopApp::RequestRestart()
{
    if (exitRequested_ || preparedRestart_) return false;
    if (settingsWindow_ && settingsWindow_->IsVisible() &&
        !settingsWindow_->FlushPendingChanges())
    {
        return false;
    }

    wchar_t exePath[MAX_PATH * 4]{};
    const DWORD pathLen = GetModuleFileNameW(
        nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (pathLen == 0 || pathLen >= std::size(exePath))
    {
        MessageBoxW(controlHwnd_ ? controlHwnd_ : hwnd_,
            _LW("app.run.no_path"), _LW("app.run.restart_failed"),
            MB_OK | MB_ICONERROR);
        return false;
    }

    auto restart = std::make_unique<
        snowdesktop::single_instance::PreparedRestart>();
    const DWORD error = restart->Prepare(std::wstring_view(exePath, pathLen));
    if (error != ERROR_SUCCESS)
    {
        std::wstring message =
            _LFW("app.run.restart_error", std::to_wstring(error));
        MessageBoxW(controlHwnd_ ? controlHwnd_ : hwnd_, message.c_str(),
            _LW("app.run.restart_failed"), MB_OK | MB_ICONERROR);
        return false;
    }

    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Runtime, (L"Application restart prepared: pid=" +
        std::to_wstring(GetCurrentProcessId()) + L" child=" +
        std::to_wstring(restart->ProcessId())).c_str());
    preparedRestart_ = std::move(restart);
    // Settings were already flushed above. A second flush after child creation
    // could reject exit and leave a restart waiting for a host that stays open.
    CompleteExitRequest();
    return true;
}
