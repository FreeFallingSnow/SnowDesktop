#include "app.h"
#include <algorithm>

// Floating-Dock hotkey and edge-swipe lifecycle.

namespace
{
bool ForegroundFullscreenBlocksEdgeSwipe(bool enabled, const RECT& monitor)
{
    if (!enabled) return false;
    const HWND foreground = GetForegroundWindow();
    if (!foreground || !IsWindowVisible(foreground) || IsIconic(foreground))
        return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId == GetCurrentProcessId()) return false;
    wchar_t className[256]{};
    GetClassNameW(foreground, className, 256);
    if (_wcsicmp(className, L"Progman") == 0 ||
        _wcsicmp(className, L"WorkerW") == 0)
        return false;
    RECT client{};
    if (!GetClientRect(foreground, &client)) return false;
    POINT origin{};
    if (!ClientToScreen(foreground, &origin)) return false;
    OffsetRect(&client, origin.x, origin.y);
    return snowdesktop::floating_dock_rules::ShouldBlockFullscreenEdgeSwipe(
        enabled, client, monitor);
}
}

LRESULT CALLBACK DesktopApp::FloatingDockEdgeSwipeMouseHookProc(
    int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION &&
        (message == WM_LBUTTONDOWN ||
         message == WM_LBUTTONUP ||
         message == WM_RBUTTONDOWN ||
         message == WM_RBUTTONUP ||
         message == WM_MBUTTONDOWN ||
         message == WM_MBUTTONUP ||
         message == WM_XBUTTONDOWN ||
         message == WM_XBUTTONUP))
    {
        floatingDockEdgeSwipeMouseActivity_.store(
            true, std::memory_order_relaxed);
    }
    return CallNextHookEx(nullptr, code, message, data);
}

bool DesktopApp::StartFloatingDockEdgeSwipeMouseMonitor()
{
    if (floatingDockEdgeSwipeMouseHook_)
        return true;

    floatingDockEdgeSwipeMouseActivity_.store(
        false, std::memory_order_relaxed);
    if (floatingDockEdgeSwipeMouseHook_.Start(
            instance_, &DesktopApp::FloatingDockEdgeSwipeMouseHookProc))
        return true;

    WriteDiagnosticLogEntry(
        L"Floating Dock edge-swipe mouse hook FAILED");
    return false;
}

void DesktopApp::StopFloatingDockEdgeSwipeMouseMonitor()
{
    floatingDockEdgeSwipeMouseHook_.Stop();
    floatingDockEdgeSwipeMouseActivity_.store(
        false, std::memory_order_relaxed);
}

void DesktopApp::UnregisterFloatingDockHotkey()
{
    StopFloatingDockEdgeSwipeMouseMonitor();
    if (floatingDockHotkeyRegistered_ && floatingDockHotkeyHwnd_)
        UnregisterHotKey(
            floatingDockHotkeyHwnd_, kFloatingDockHotkeyId);
    if (floatingDockEdgeSwipeHwnd_ &&
        IsWindow(floatingDockEdgeSwipeHwnd_))
    {
        KillTimer(
            floatingDockEdgeSwipeHwnd_,
            kFloatingDockEdgeSwipeTimerId);
    }
    floatingDockHotkeyRegistered_ = false;
    floatingDockHotkeyHwnd_ = nullptr;
    floatingDockEdgeSwipeHwnd_ = nullptr;
    floatingDockEdgeSwipeDetector_.Reset();
    floatingDockPointerButtonsDown_ = 0;
    floatingDockExternalPointerDrag_.Reset();
}

void DesktopApp::ApplyFloatingDockHotkey()
{
    UnregisterFloatingDockHotkey();
    if (!dockSettings_.showWindowPreviews) HideDockWindowPreview();
    for (const auto& host : persistentDockHosts_)
        if (host) host->edgeHoverRequested = false;

    const bool edgeSwipeEnabled =
        snowdesktop::dock_settings_rules::
            IsFloatingEdgeSwipeEnabled(
                dockSettings_.showOnlyWhenSummoned,
                dockSettings_.floatingEdgeSwipeEnabled);
    const auto inputPolicy =
        snowdesktop::floating_dock_rules::ResolveFloatingDockInputPolicy(
            generalSettings_.dockEnabled,
            dockSettings_.floatingShortcutMode,
            edgeSwipeEnabled);

    // A settings apply starts a fresh passive session in either display mode.
    {
        bool passiveStateChanged = false;
        for (const auto& host : persistentDockHosts_)
        {
            if (!host)
                continue;
            passiveStateChanged = passiveStateChanged ||
                host->passivelyRevealed;
            host->passivelyRevealed = false;
            host->passiveRevealTick = 0;
            host->passiveLeaveStartTick = 0;
        }
        if (passiveStateChanged)
        {
            RefreshFloatingDockVisibilityState();
            UpdatePersistentDockHostVisibility();
        }
    }

    if (inputPolicy.closeFloatingDocks)
        CloseAllFloatingDocks();
    if (!inputPolicy.monitorPointer)
        return;

    // Register on the independent top-level control window. The desktop input
    // HWND is a 1x1 child of Explorer's current desktop host and can be
    // reparented or recreated while Explorer settles, which makes it a poor
    // long-lived WM_HOTKEY endpoint.
    HWND target =
        controlHwnd_ && IsWindow(controlHwnd_)
            ? controlHwnd_
            : (inputHwnd_ && IsWindow(inputHwnd_)
                ? inputHwnd_ : hwnd_);
    if (!target)
        return;

    if (inputPolicy.registerHotkey)
    {
        const UINT hotkeyModifiers =
            dockSettings_.floatingHotkeyModifiers |
            MOD_NOREPEAT;
        floatingDockHotkeyRegistered_ =
            RegisterHotKey(target, kFloatingDockHotkeyId,
                hotkeyModifiers,
                dockSettings_.floatingHotkeyVirtualKey) != FALSE;
        NavigationSettings hotkeyTextSettings;
        hotkeyTextSettings.modifiers =
            dockSettings_.floatingHotkeyModifiers;
        hotkeyTextSettings.virtualKey =
            dockSettings_.floatingHotkeyVirtualKey;
        const std::wstring hotkeyText =
            FormatNavigationHotkey(hotkeyTextSettings);
        if (floatingDockHotkeyRegistered_)
        {
            floatingDockHotkeyHwnd_ = target;
            const std::wstring message =
                L"Floating Dock hotkey " + hotkeyText +
                L" registered";
            WriteDiagnosticLogEntry(message.c_str());
        }
        else
        {
            const std::wstring message =
                L"Floating Dock hotkey " + hotkeyText +
                L" registration failed";
            WriteDiagnosticLogEntry(message.c_str());
        }
    }

    // This lightweight control timer serves passive drag-edge reveal,
    // optional edge-swipe recognition and mandatory outside-click dismissal.
    // Keeping them in one sampler prevents competing GetAsyncKeyState calls
    // from consuming the same click transition. It also remains available
    // while a hidden Dock HWND cannot receive mouse or OLE drag messages,
    // and when associated surfaces promote a Dock with no summon triggers.
    if (SetTimer(
            target, kFloatingDockEdgeSwipeTimerId,
            kFloatingDockEdgeSwipeIntervalMs,
            nullptr) != 0)
    {
        floatingDockEdgeSwipeHwnd_ = target;
        if (inputPolicy.monitorEdgeSwipe)
            StartFloatingDockEdgeSwipeMouseMonitor();
    }
}

bool DesktopApp::UpdatePassiveDragRevealHosts(
    POINT cursorScreen)
{
    if (desktopPassthroughActive_ || !generalSettings_.dockEnabled)
    {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    const bool internalDragActive =
        dragSession_.IsActive();
    const bool oleDragActive =
        dragDropController_.IsTransportActive() || floatingDockExternalPointerDrag_.Active();
    const bool dragRevealActive =
        internalDragActive || oleDragActive;
    bool passiveDragRevealedThisSample = false;

    for (const auto& ownedHost : persistentDockHosts_)
    {
        if (!ownedHost || !ownedHost->active ||
            !ownedHost->container)
        {
            continue;
        }
        PersistentDockHost& host = *ownedHost;
        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        if (!host.monitor ||
            !GetMonitorInfoW(host.monitor, &monitorInfo))
        {
            host.passiveLeaveStartTick = 0;
            continue;
        }

        RECT dockScreenRect = host.dockRect;
        if (hwnd_ && IsWindow(hwnd_))
        {
            MapWindowPoints(
                hwnd_, nullptr,
                reinterpret_cast<POINT*>(&dockScreenRect), 2);
        }
        else
        {
            OffsetRect(
                &dockScreenRect,
                virtualLeft_, virtualTop_);
        }
        const bool merged = host.container->IsMergedWithStatusBar();
        RECT edgeScreenRect = dockScreenRect;
        if (merged && statusBar_)
            if (const auto strip = statusBar_->MergedStripBounds(host.monitor)) edgeScreenRect = *strip;

        UINT dpiX = 96;
        UINT dpiY = 96;
        if (FAILED(GetDpiForMonitor(
                host.monitor, MDT_EFFECTIVE_DPI,
                &dpiX, &dpiY)))
        {
            dpiX = 96;
        }
        const int edgeBand =
            snowdesktop::floating_dock_rules::
                ScaleEdgeSwipeDip(
                    snowdesktop::floating_dock_rules::
                        kPassiveDragRevealEdgeBandDip,
                    dpiX);
        const bool pointerInEdgeProjection =
            snowdesktop::floating_dock_rules::
                IsPointInDockEdgeProjection(
                    cursorScreen,
                    monitorInfo.rcMonitor,
                    edgeScreenRect,
                    dockSettings_.position,
                    edgeBand);
        const bool passiveDragRevealRequested =
            snowdesktop::floating_dock_rules::
                ShouldPassivelyRevealDockForDragAtEdge(
                    pointerInEdgeProjection,
                    internalDragActive,
                    oleDragActive) || host.edgeHoverRequested;
        host.edgeHoverRequested = false;
        const bool pointerInEdgeCorridor =
            snowdesktop::floating_dock_rules::
                IsPointInDockEdgeCorridor(
                    cursorScreen,
                    monitorInfo.rcMonitor,
                    edgeScreenRect,
                    dockSettings_.position);

        bool previewAssociated = false;
        if (dockWindowPreview_ &&
            dockWindowPreview_->IsVisible() &&
            !IsRectEmpty(&dockWindowPreviewAnchorScreen_))
        {
            const POINT previewAnchorCenter{
                (dockWindowPreviewAnchorScreen_.left +
                    dockWindowPreviewAnchorScreen_.right) / 2,
                (dockWindowPreviewAnchorScreen_.top +
                    dockWindowPreviewAnchorScreen_.bottom) / 2
            };
            previewAssociated = MonitorFromPoint(
                previewAnchorCenter,
                MONITOR_DEFAULTTONEAREST) == host.monitor;
        }
        const bool associatedSurfaceActive =
            collectionPopupDockHost_ == &host ||
            quickNavigationDockHost_ == &host ||
            previewAssociated ||
            snowdesktop::floating_dock_rules::IsMenuOwnedByDock(
                snowdesktop::modern_menu::ActiveRootWindow(), host.hwnd) ||
            snowdesktop::floating_dock_rules::IsMenuOwnedByDock(
                shellPopupTrackerOwnerHwnd_.load(std::memory_order_acquire), host.hwnd);
        const bool keepPassiveDragReveal =
            associatedSurfaceActive || pointerInEdgeCorridor ||
            PtInRect(&dockScreenRect, cursorScreen) ||
            (merged && ((statusBar_ && statusBar_->HasInteractionSession(host.monitor)) ||
                pointerInEdgeCorridor ||
                snowdesktop::floating_dock_rules::IsPointInMergedDockInteraction(
                    cursorScreen, dockScreenRect, edgeScreenRect))) ||
            (dragRevealActive &&
                pointerInEdgeCorridor);
        const bool leaveDelayElapsed =
            snowdesktop::floating_dock_rules::
                HasPassiveDragLeaveDelayElapsed(
                    host.passiveLeaveStartTick,
                    now);
        const auto action =
            snowdesktop::floating_dock_rules::
                ResolvePassiveDragRevealUpdate(
                    true,
                    host.promoted,
                    host.passivelyRevealed,
                    passiveDragRevealRequested,
                    keepPassiveDragReveal,
                    host.passiveLeaveStartTick != 0,
                    leaveDelayElapsed);

        bool visibilityChanged = false;
        switch (action)
        {
        case snowdesktop::floating_dock_rules::
                PassiveDragRevealAction::Reveal:
            host.passivelyRevealed = true;
            host.passiveRevealTick = now;
            host.passiveLeaveStartTick = 0;
            visibilityChanged = true;
            passiveDragRevealedThisSample = true;
            break;
        case snowdesktop::floating_dock_rules::
                PassiveDragRevealAction::BeginLeave:
            host.passiveLeaveStartTick = now;
            break;
        case snowdesktop::floating_dock_rules::
                PassiveDragRevealAction::CancelLeave:
            host.passiveLeaveStartTick = 0;
            break;
        case snowdesktop::floating_dock_rules::
                PassiveDragRevealAction::Hide:
            host.passivelyRevealed = false;
            host.passiveRevealTick = 0;
            host.passiveLeaveStartTick = 0;
            visibilityChanged = true;
            break;
        case snowdesktop::floating_dock_rules::
                PassiveDragRevealAction::None:
        default:
            break;
        }

        if (visibilityChanged)
        {
            RefreshFloatingDockVisibilityState();
            bool revealFramePrepared = false;
            if (host.passivelyRevealed)
            {
                POINT cursorDesktop = cursorScreen;
                if (hwnd_ && IsWindow(hwnd_))
                    ScreenToClient(hwnd_, &cursorDesktop);
                else
                {
                    cursorDesktop.x -= virtualLeft_;
                    cursorDesktop.y -= virtualTop_;
                }
                lastMousePoint_ = cursorDesktop;
                // Prepare the Dock and native-backdrop panel list while both
                // HWNDs are still hidden. Showing first can expose the frame
                // retained from the previous reveal for one composition tick.
                revealFramePrepared =
                    RenderFloatingDockCompositionFrame(host);
                if (revealFramePrepared)
                    revealFramePrepared =
                        FlushPendingCompositionCommit();
            }
            UpdatePersistentDockHostVisibility(host);
            if (host.passivelyRevealed &&
                !revealFramePrepared)
                InvalidateFloatingDockWindow(host, true);
        }
    }

    return passiveDragRevealedThisSample;
}

bool DesktopApp::DismissMergedDockBackground(PersistentDockHost& host, POINT desktopPoint, POINT screenPoint)
{
    if (!host.active || !host.container || !host.container->IsMergedWithStatusBar() ||
        !host.mergedPresentation.inputEnabled || !statusBar_ ||
        dragSession_.IsActive() || dragDropController_.IsTransportActive()) return false;
    const auto& dock = *host.container;
    if (dock.IsWindowsButtonPoint(desktopPoint) || dock.IsSearchPoint(desktopPoint) ||
        dock.EntryAtPoint(desktopPoint) || dock.RunningItemAtPoint(desktopPoint) ||
        dock.FrequentItemAtPoint(desktopPoint)) return false;
    // Called from the actual Dock HWND's down message, not from a global
    // pointer sample: overlapping popup controls keep their own input route.
    return statusBar_->DismissMergedBackground(host.monitor, screenPoint);
}

void DesktopApp::UpdateFloatingDockEdgeSwipe()
{
    // Poll only cheap, already-owned surface state. Hold release does not
    // require another physical button-down or a fullscreen/window scan.
    for (const auto& ownedHost : persistentDockHosts_)
    {
        if (!ownedHost || !ownedHost->active || !ownedHost->container ||
            !ownedHost->container->IsMergedWithStatusBar()) continue;
        auto& host = *ownedHost;
        const bool held = statusBar_ && statusBar_->HasInteractionSession(host.monitor);
        if (held != host.mergedInteractionHeld)
        {
            host.mergedInteractionHeld = held;
            if (!held && host.mergedCloseAfterInteraction)
            {
                host.mergedCloseAfterInteraction = false;
                CloseFloatingDock(host, FloatingDockCloseFocusPolicy::PreserveCurrent);
            }
            UpdatePersistentDockHostVisibility(host);
        }
    }
    if (desktopPassthroughActive_)
    {
        floatingDockEdgeSwipeDetector_.Reset();
        floatingDockPointerButtonsDown_ = 0;
        floatingDockExternalPointerDrag_.Reset();
        return;
    }
    constexpr UINT leftButtonBit = 1u << 0;
    constexpr UINT rightButtonBit = 1u << 1;
    constexpr UINT middleButtonBit = 1u << 2;
    const SHORT leftState =
        GetAsyncKeyState(VK_LBUTTON);
    const SHORT rightState =
        GetAsyncKeyState(VK_RBUTTON);
    const SHORT middleState =
        GetAsyncKeyState(VK_MBUTTON);
    const UINT buttonsDown =
        ((leftState & 0x8000) ? leftButtonBit : 0) |
        ((rightState & 0x8000) ? rightButtonBit : 0) |
        ((middleState & 0x8000) ? middleButtonBit : 0);
    const UINT pressedSinceLastSample =
        ((leftState & 1) ? leftButtonBit : 0) |
        ((rightState & 1) ? rightButtonBit : 0) |
        ((middleState & 1) ? middleButtonBit : 0);
    const UINT previousButtonsDown =
        floatingDockPointerButtonsDown_;
    const bool pointerPressed =
        snowdesktop::floating_dock_rules::
            HasNewPointerButtonPress(
                buttonsDown,
                previousButtonsDown,
                pressedSinceLastSample);
    const bool leftButtonPressed =
        ((buttonsDown & leftButtonBit) != 0 &&
            (previousButtonsDown &
                leftButtonBit) == 0) ||
        (pressedSinceLastSample & leftButtonBit) != 0;
    floatingDockPointerButtonsDown_ = buttonsDown;
    if ((buttonsDown & leftButtonBit) == 0 &&
        !mouseDown_)
    {
        // A menu can consume the original Dock button-down completely. In
        // that case no Dock button-up handler will clear the one-shot release
        // suppression, so retire it after the physical press has ended.
        dockSuppressClickReleaseEntry_ =
            static_cast<size_t>(-1);
        ClearDockPressedState();
    }

    POINT cursor{};
    if (!GetCursorPos(&cursor))
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }

    POINT desktopPoint = cursor;
    if (hwnd_ && IsWindow(hwnd_))
        ScreenToClient(hwnd_, &desktopPoint);
    else
    {
        desktopPoint.x -= virtualLeft_;
        desktopPoint.y -= virtualTop_;
    }

    GUITHREADINFO dragGui{sizeof(dragGui)};
    const bool haveDragGui = GetGUIThreadInfo(0, &dragGui) != FALSE;
    DWORD dragSourceProcess = 0;
    GetWindowThreadProcessId(haveDragGui && dragGui.hwndCapture ? dragGui.hwndCapture : GetForegroundWindow(),
        &dragSourceProcess);
    floatingDockExternalPointerDrag_.Update(cursor, (buttonsDown & (leftButtonBit | rightButtonBit)) != 0,
        dragSourceProcess != 0 && dragSourceProcess != GetCurrentProcessId(),
        (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 || (haveDragGui && (dragGui.flags & GUI_INMOVESIZE)),
        GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG));

    // Passive drag reveal must run before the legacy button/drag early return
    // below. An ordinary pointer still needs the existing edge-swipe gesture,
    // which starts a manual floating session and remains visible until closed.
    const bool passiveDragRevealedThisSample =
        UpdatePassiveDragRevealHosts(cursor);

    if ((floatingDockVisible_ || statusBar_) && pointerPressed &&
        !passiveDragRevealedThisSample)
    {
        if (floatingDockVisible_ && leftButtonPressed &&
            TryActivateDockPopupFromMenuPointerPress(
                desktopPoint,
                cursor,
                (buttonsDown & leftButtonBit) != 0))
        {
            floatingDockEdgeSwipeDetector_.
                SuppressUntilEdgeLeave();
            return;
        }
        // The thumbnail preview panel belongs to the Dock's interactive
        // surface. A press there (card click or close button) must not be
        // read as an outside click that tears the host down mid-click; the
        // preview's own button-up handler owns that click.
        RECT previewDesktopRect{};
        if (dockWindowPreview_ &&
            dockWindowPreview_->IsVisible() &&
            hwnd_ && IsWindow(hwnd_))
        {
            RECT previewScreenRect{};
            if (GetWindowRect(
                    dockWindowPreview_->GetWindow(),
                    &previewScreenRect))
            {
                MapWindowPoints(
                    nullptr, hwnd_,
                    reinterpret_cast<POINT*>(
                        &previewScreenRect),
                    2);
                previewDesktopRect = previewScreenRect;
            }
        }
        const RECT quickNavigationInteractionRect =
            quickNavigationOpen_
                ? quickNavigationRect_
                : RECT{};
        const RECT dockPopupInteractionRect =
            snowdesktop::floating_dock_rules::
                DockAssociatedPopupInteractionRect(
                    popupAnchoredToDock_,
                    floatingPopupCollectionRegion_);
        const bool onBar = statusBar_ && statusBar_->ContainsPoint(cursor);
        const bool onPanel = systemPanel_ && systemPanel_->ContainsPoint(cursor);
        bool onOwnedMenu = false;
        const HWND menu = snowdesktop::modern_menu::ActiveRootWindow();
        if (menu && statusBarMenuOwner_ && GetWindow(menu, GW_OWNER) == statusBarMenuOwner_)
        {
            HWND target = GetAncestor(WindowFromPoint(cursor), GA_ROOT);
            for (unsigned depth = 0; target && depth < 16; ++depth, target = GetWindow(target, GW_OWNER))
                if (target == menu) { onOwnedMenu = true; break; }
        }
        const bool onNavigation = quickNavigationInvocationSource_ == QuickNavigationInvocationSource::StatusBar &&
            PtInRect(&quickNavigationInteractionRect, desktopPoint);
        bool barSession = false;
        bool onRevealedDockSurface = false;
        for (const auto& ownedHost : persistentDockHosts_)
        {
            if (!ownedHost || !ownedHost->active || !ownedHost->container || !statusBar_) continue;
            auto& host = *ownedHost;
            const bool merged = host.container->IsMergedWithStatusBar();
            const bool heldMerged = merged && host.promoted && statusBar_->HasInteractionSession(host.monitor);
            const bool separateReveal = !merged && statusBar_->HasTemporaryReveal(host.monitor);
            if (!heldMerged && !separateReveal) continue;
            barSession = barSession || heldMerged;
            const bool ownSurface = (onBar && MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL) == host.monitor) ||
                (statusBar_ && statusBar_->ContainsTrayMenuPoint(host.monitor, cursor)) ||
                (onPanel && systemPanel_->IsOpenForMonitor(host.monitor)) ||
                (onOwnedMenu && statusBarMenuMonitor_ == host.monitor) ||
                (onNavigation && statusBarQuickNavigationMonitor_ == host.monitor) ||
                (IsWindowVisible(host.hwnd) && PtInRect(&host.dockRect, desktopPoint));
            onRevealedDockSurface = onRevealedDockSurface || (separateReveal && host.promoted && ownSurface);
            if (!ownSurface && !dragSession_.IsActive() && !dragDropController_.IsTransportActive())
            {
                // Preserve the outside down even while the owned popup is
                // fading out. Release this monitor's promotion after its hold
                // ends; a second button-down is neither required nor invented.
                if (heldMerged)
                {
                    host.mergedCloseAfterInteraction = true;
                    host.mergedInteractionHeld = true;
                }
                else statusBar_->DismissTemporaryReveal(host.monitor);
                CancelStatusBarActivation(host.monitor, false);
            }
        }
        // The bar and its owned surface are one interaction session with the
        // merged Dock. Its own dismissal logic releases this hold; the global
        // pointer sampler must not tear the owner down between down and up.
        if (floatingDockVisible_ && !barSession && !onBar && !onRevealedDockSurface &&
            !IsPointOnPromotedDock(desktopPoint) &&
            snowdesktop::floating_dock_rules::
                ShouldDismissForPointerDown(
                    dragSession_.IsActive() ||
                        dragDropController_.IsExternalDragActive(),
                    HasActiveContextMenuSession(),
                    desktopPoint,
                    RECT{},
                    dockPopupInteractionRect,
                    previewDesktopRect,
                    quickNavigationInteractionRect))
        {
            // A physical outside click owns the foreground transition. Do not
            // restore the window that preceded the floating Dock: doing so can
            // race the clicked window's activation and force two DWM/backdrop
            // source changes through the shared close handoff.
            CloseAllFloatingDocks(
                FloatingDockCloseFocusPolicy::PreserveCurrent);
            floatingDockEdgeSwipeDetector_.
                SuppressUntilEdgeLeave();
            return;
        }
    }

    if (!generalSettings_.dockEnabled ||
        !snowdesktop::dock_settings_rules::
            IsFloatingEdgeSwipeEnabled(
                dockSettings_.showOnlyWhenSummoned,
                dockSettings_.floatingEdgeSwipeEnabled))
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }

    // A click can finish before a context menu enters its nested input loop.
    // Keep that click from becoming the first half of a buttonless edge swipe,
    // and require a real edge leave before gestures become eligible again.
    const bool pointerButtonActivity =
        snowdesktop::floating_dock_rules::
            HasPointerButtonActivity(
                buttonsDown,
                previousButtonsDown,
                pressedSinceLastSample,
                floatingDockEdgeSwipeMouseActivity_.exchange(
                    false, std::memory_order_relaxed));
    GUITHREADINFO foregroundGuiThreadInfo{
        sizeof(foregroundGuiThreadInfo)
    };
    const bool foregroundGuiMenuActive =
        GetGUIThreadInfo(0, &foregroundGuiThreadInfo) &&
        snowdesktop::floating_dock_rules::
            IsGuiMenuModeActive(
                foregroundGuiThreadInfo.flags);
    const bool suppressEdgeSwipeUntilLeave =
        pointerButtonActivity ||
        HasActiveContextMenuSession() ||
        foregroundGuiMenuActive ||
        dragSession_.IsActive() ||
        dragDropController_.IsTransportActive();
    if (suppressEdgeSwipeUntilLeave)
    {
        floatingDockEdgeSwipeDetector_.
            SuppressUntilEdgeLeave();
        return;
    }

    const HMONITOR monitor = MonitorFromPoint(
        cursor, MONITOR_DEFAULTTONULL);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    if (!monitor ||
        !GetMonitorInfoW(monitor, &monitorInfo))
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }

    if (ForegroundFullscreenBlocksEdgeSwipe(
            dockSettings_.floatingEdgeSwipeBlockFullscreen,
            monitorInfo.rcMonitor))
    {
        // A fresh edge entry is required after leaving fullscreen.
        floatingDockEdgeSwipeDetector_.SuppressUntilEdgeLeave();
        return;
    }

    // A monitor outside the configured Dock scope must not redirect the
    // gesture to a Dock on another display.
    DockContainer* dock =
        SelectFloatingDockContainerForMonitor(monitor);
    if (!dock)
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }
    const RECT dockBounds = dock->GetBounds();
    const POINT dockCenter{
        (dockBounds.left + dockBounds.right) / 2 +
            virtualLeft_,
        (dockBounds.top + dockBounds.bottom) / 2 +
            virtualTop_
    };
    if (MonitorFromPoint(
            dockCenter, MONITOR_DEFAULTTONEAREST) != monitor)
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(
            monitor, MDT_EFFECTIVE_DPI,
            &dpiX, &dpiY)))
        dpiX = 96;
    const int edgeBand =
        snowdesktop::floating_dock_rules::
            ScaleEdgeSwipeDip(
                snowdesktop::floating_dock_rules::
                    kEdgeSwipeBandDip,
                dpiX);
    const int requiredTravel =
        snowdesktop::floating_dock_rules::
            ScaleEdgeSwipeDip(
                snowdesktop::floating_dock_rules::
                    kEdgeSwipeTravelDip,
                dpiX);
    const bool triggered =
        floatingDockEdgeSwipeDetector_.Update(
            cursor, monitorInfo.rcMonitor,
            dockSettings_.position,
            GetTickCount(), edgeBand,
            requiredTravel,
            snowdesktop::floating_dock_rules::kEdgeSwipeMaximumDurationMs,
            dockSettings_.edgeRevealGesture == 1);
    PersistentDockHost* targetHost =
        FindPersistentDockHost(dock);
    if (triggered &&
        (!targetHost ||
            !IsPersistentDockHostPromoted(*targetHost)))
    {
        WriteDiagnosticLogEntry(L"Floating Dock edge gesture received");
        if (dockSettings_.showOnlyWhenSummoned && dockSettings_.edgeRevealGesture == 1 && targetHost)
        {
            targetHost->edgeHoverRequested = true;
            UpdatePassiveDragRevealHosts(cursor);
        }
        else ShowFloatingDock(monitor);
    }
}
