#include "app.h"

// Floating-Dock hotkey and edge-swipe lifecycle.

void DesktopApp::UnregisterFloatingDockHotkey()
{
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
}

void DesktopApp::ApplyFloatingDockHotkey()
{
    UnregisterFloatingDockHotkey();

    // Passive reveal belongs only to auto-hide. Clearing it here keeps a
    // settings toggle from leaving a Host effectively promoted after ordinary
    // desktop visibility has been restored.
    if (!dockSettings_.autoHide)
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

    if (!generalSettings_.dockEnabled ||
        (!dockSettings_.autoHide &&
            !snowdesktop::floating_dock_rules::
                HasAnySummonTrigger(
                    dockSettings_.floatingShortcutMode,
                    dockSettings_.floatingEdgeSwipeEnabled)))
    {
        CloseAllFloatingDocks();
        return;
    }

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

    if (dockSettings_.floatingShortcutMode)
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

    // This lightweight control timer serves auto-hide edge projection,
    // optional edge-swipe recognition and mandatory outside-click dismissal.
    // Keeping them in one sampler prevents competing GetAsyncKeyState calls
    // from consuming the same click transition. It also remains available
    // while a hidden Dock HWND cannot receive mouse or OLE drag messages.
    if (SetTimer(
            target, kFloatingDockEdgeSwipeTimerId,
            kFloatingDockEdgeSwipeIntervalMs,
            nullptr) != 0)
    {
        floatingDockEdgeSwipeHwnd_ = target;
    }
}

bool DesktopApp::UpdateAutoHiddenDockHosts(
    POINT cursorScreen, UINT buttonsDown)
{
    if (!generalSettings_.dockEnabled ||
        !dockSettings_.autoHide)
    {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    const bool interactionBusy =
        buttonsDown != 0 || mouseDown_ ||
        dragSession_.IsActive() ||
        dragDropController_.IsTransportActive();
    const bool contextMenuActive =
        HasActiveContextMenuSession();
    bool revealedThisSample = false;

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
                        kAutoHideEdgeBandDip,
                    dpiX);
        const bool pointerInEdgeProjection =
            snowdesktop::floating_dock_rules::
                IsPointInDockEdgeProjection(
                    cursorScreen,
                    monitorInfo.rcMonitor,
                    dockScreenRect,
                    dockSettings_.position,
                    edgeBand);
        const bool pointerInEdgeCorridor =
            snowdesktop::floating_dock_rules::
                IsPointInDockEdgeCorridor(
                    cursorScreen,
                    monitorInfo.rcMonitor,
                    dockScreenRect,
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
            contextMenuActive ||
            collectionPopupDockHost_ == &host ||
            quickNavigationDockHost_ == &host ||
            previewAssociated;
        const bool keepRevealed =
            pointerInEdgeCorridor ||
            associatedSurfaceActive ||
            interactionBusy;
        const bool leaveDelayElapsed =
            snowdesktop::floating_dock_rules::
                HasAutoHideLeaveDelayElapsed(
                    host.passiveLeaveStartTick,
                    now);
        const auto action =
            snowdesktop::floating_dock_rules::
                ResolveAutoHideUpdate(
                    true,
                    host.promoted,
                    host.passivelyRevealed,
                    pointerInEdgeProjection,
                    keepRevealed,
                    host.passiveLeaveStartTick != 0,
                    leaveDelayElapsed);

        bool visibilityChanged = false;
        switch (action)
        {
        case snowdesktop::floating_dock_rules::
                AutoHideUpdateAction::Reveal:
            host.passivelyRevealed = true;
            host.passiveRevealTick = now;
            host.passiveLeaveStartTick = 0;
            visibilityChanged = true;
            revealedThisSample = true;
            break;
        case snowdesktop::floating_dock_rules::
                AutoHideUpdateAction::BeginLeave:
            host.passiveLeaveStartTick = now;
            break;
        case snowdesktop::floating_dock_rules::
                AutoHideUpdateAction::CancelLeave:
            host.passiveLeaveStartTick = 0;
            break;
        case snowdesktop::floating_dock_rules::
                AutoHideUpdateAction::Hide:
            host.passivelyRevealed = false;
            host.passiveRevealTick = 0;
            host.passiveLeaveStartTick = 0;
            visibilityChanged = true;
            break;
        case snowdesktop::floating_dock_rules::
                AutoHideUpdateAction::None:
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

    return revealedThisSample;
}

void DesktopApp::UpdateFloatingDockEdgeSwipe()
{
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
    const bool pointerPressed =
        snowdesktop::floating_dock_rules::
            HasNewPointerButtonPress(
                buttonsDown,
                floatingDockPointerButtonsDown_,
                pressedSinceLastSample);
    const bool leftButtonPressed =
        ((buttonsDown & leftButtonBit) != 0 &&
            (floatingDockPointerButtonsDown_ &
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

    // Auto-hide must run before the legacy button/drag early return below.
    // This global sampler is the only reliable reveal source while a hidden
    // Host cannot receive WM_MOUSEMOVE or OLE DragEnter/DragOver itself.
    const bool autoHideRevealedThisSample =
        UpdateAutoHiddenDockHosts(cursor, buttonsDown);

    if (floatingDockVisible_ && pointerPressed &&
        !autoHideRevealedThisSample)
    {
        if (leftButtonPressed &&
            TryActivateDockPopupFromMenuPointerPress(
                desktopPoint,
                cursor,
                (buttonsDown & leftButtonBit) != 0))
        {
            floatingDockEdgeSwipeDetector_.Reset();
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
        if (!IsPointOnPromotedDock(desktopPoint) &&
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
            floatingDockEdgeSwipeDetector_.Reset();
            return;
        }
    }

    if (!generalSettings_.dockEnabled ||
        !dockSettings_.floatingEdgeSwipeEnabled ||
        dragSession_.IsActive() ||
        buttonsDown != 0)
    {
        floatingDockEdgeSwipeDetector_.Reset();
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
            requiredTravel);
    const PersistentDockHost* targetHost =
        FindPersistentDockHost(dock);
    if (triggered &&
        (!targetHost ||
            !IsPersistentDockHostPromoted(*targetHost)))
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock edge swipe received");
        ShowFloatingDock(monitor);
    }
}
