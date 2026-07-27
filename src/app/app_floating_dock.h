/**
 * @file app_floating_dock.h
 * @brief Shortcut-summoned compact Dock host.
 *
 * The host is deliberately an ordinary, non-activating popup. It never uses
 * WS_EX_TOPMOST and never covers the virtual desktop: its region is the union
 * of the Dock interaction bounds and the optional collection popup.
 */
#pragma once

inline void DesktopApp::UnregisterFloatingDockHotkey()
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

inline void DesktopApp::ApplyFloatingDockHotkey()
{
    UnregisterFloatingDockHotkey();
    if (!generalSettings_.dockEnabled ||
        !snowdesktop::floating_dock_rules::
            HasAnySummonTrigger(
                dockSettings_.floatingShortcutMode,
                dockSettings_.floatingEdgeSwipeEnabled))
    {
        CloseFloatingDock();
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
            WriteCrashLogEntry(message.c_str());
        }
        else
        {
            const std::wstring message =
                L"Floating Dock hotkey " + hotkeyText +
                L" registration failed";
            WriteCrashLogEntry(message.c_str());
        }
    }

    // This lightweight control timer serves both optional edge-swipe
    // recognition and mandatory outside-click dismissal. Keeping both in one
    // sampler prevents competing GetAsyncKeyState calls from consuming the
    // same click transition.
    if (SetTimer(
            target, kFloatingDockEdgeSwipeTimerId,
            kFloatingDockEdgeSwipeIntervalMs,
            nullptr) != 0)
    {
        floatingDockEdgeSwipeHwnd_ = target;
    }
}

inline void DesktopApp::UpdateFloatingDockEdgeSwipe()
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
    floatingDockPointerButtonsDown_ = buttonsDown;

    POINT cursor{};
    if (!GetCursorPos(&cursor))
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }

    if (floatingDockVisible_ && pointerPressed)
    {
        POINT desktopPoint = cursor;
        if (hwnd_ && IsWindow(hwnd_))
            ScreenToClient(hwnd_, &desktopPoint);
        else
        {
            desktopPoint.x -= virtualLeft_;
            desktopPoint.y -= virtualTop_;
        }
        if (snowdesktop::floating_dock_rules::
                ShouldDismissForPointerDown(
                    dragSession_.IsActive() ||
                        externalDragActive_,
                    desktopPoint,
                    floatingDockRect_,
                    floatingDockPopupRect_))
        {
            CloseFloatingDock();
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
    if (triggered && !floatingDockVisible_)
    {
        WriteCrashLogEntry(
            L"Floating Dock edge swipe received");
        ShowFloatingDock();
    }
}

inline bool DesktopApp::CreateFloatingDockWindow()
{
    if (floatingDockHwnd_ && IsWindow(floatingDockHwnd_))
        return true;

    floatingDockHwnd_ = CreateWindowExW(
        snowdesktop::floating_dock_rules::kWindowExStyle,
        kFloatingDockWindowClassName,
        _LW("app.settings.dock_bar"),
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!floatingDockHwnd_)
        return false;

    floatingDockDropTargetRegistered_ =
        SUCCEEDED(RegisterDragDrop(
            floatingDockHwnd_,
            static_cast<IDropTarget*>(this)));
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(floatingDockHwnd_,
        DWMWA_TRANSITIONS_FORCEDISABLED,
        &disableTransitions, sizeof(disableTransitions));
    return true;
}

inline void DesktopApp::ResetFloatingDockCompositionResources()
{
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    if (floatingDockDcompVisual_)
        floatingDockDcompVisual_->SetContent(nullptr);
    floatingDockDcompSurface_.Reset();
    floatingDockCompWidth_ = 0;
    floatingDockCompHeight_ = 0;
}

inline void DesktopApp::DestroyFloatingDockWindow()
{
    floatingDockVisible_ = false;
    floatingDockRevealPending_ = false;
    floatingDockContainer_ = nullptr;
    floatingDockMonitor_ = nullptr;
    floatingDockSourceRect_ = {};
    floatingDockRect_ = {};
    floatingDockPopupRect_ = {};
    floatingDockTooltipRect_ = {};
    floatingDockBackdropCompositor_.Reset();
    if (floatingDockDropTargetRegistered_ &&
        floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
        RevokeDragDrop(floatingDockHwnd_);
    floatingDockDropTargetRegistered_ = false;
    ResetFloatingDockCompositionResources();
    floatingDockDcompVisual_.Reset();
    floatingDockDcompTarget_.Reset();
    if (floatingDockHwnd_ && IsWindow(floatingDockHwnd_))
        DestroyWindow(floatingDockHwnd_);
    floatingDockHwnd_ = nullptr;
}

inline DockContainer*
DesktopApp::SelectFloatingDockContainerAtCursor() const
{
    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    const HMONITOR cursorMonitor =
        MonitorFromPoint(cursorScreen, MONITOR_DEFAULTTONEAREST);
    return SelectFloatingDockContainerForMonitor(
        cursorMonitor);
}

inline DockContainer*
DesktopApp::SelectFloatingDockContainerForMonitor(
    HMONITOR monitor) const
{
    DockContainer* fallback = nullptr;
    for (const auto& container : containers_)
    {
        auto* dock =
            dynamic_cast<DockContainer*>(container.get());
        if (!dock)
            continue;
        if (!fallback)
            fallback = dock;
        const RECT bounds = dock->GetBounds();
        POINT centerScreen{
            (bounds.left + bounds.right) / 2 + virtualLeft_,
            (bounds.top + bounds.bottom) / 2 + virtualTop_
        };
        if (MonitorFromPoint(centerScreen,
                MONITOR_DEFAULTTONEAREST) == monitor)
            return dock;
    }
    return fallback;
}

inline RECT DesktopApp::
CalculateFloatingDockStableSourceRect() const
{
    if (!floatingDockContainer_)
        return RECT{};

    const RECT dockRect =
        floatingDockContainer_->
            GetInteractiveBounds();
    RECT sourceRect =
        snowdesktop::floating_dock_rules::
            ExpandForBorderOverdraw(
                snowdesktop::
                    floating_dock_rules::
                        ExpandHostForTitleLayer(
                            dockRect,
                            dockSettings_.position));

    RECT popupWork = layoutWorkArea_;
    const POINT dockCenter{
        (dockRect.left + dockRect.right) / 2,
        (dockRect.top + dockRect.bottom) / 2
    };
    for (const auto& page : gridPages_)
    {
        if (PtInRect(&page.bounds, dockCenter))
        {
            popupWork = page.workArea;
            break;
        }
    }

    const int workWidth = std::max<LONG>(
        1, popupWork.right - popupWork.left);
    const int workHeight = std::max<LONG>(
        1, popupWork.bottom - popupWork.top);
    const int cellWidth =
        GetCollectionPopupCellWidth();
    const int cellHeight =
        GetCollectionPopupCellHeight();
    const int availableWidth =
        std::max(1, workWidth - 24);
    const int maxWidth =
        std::min(560, availableWidth);
    const int popupContentWidth =
        std::max(1, maxWidth -
            kCollectionPopupPaddingX * 2);
    const int maxColumns = std::max(
        1, (popupContentWidth +
                kCollectionPopupGapX) /
            std::max(1, cellWidth +
                kCollectionPopupGapX));
    const int maxHeight =
        std::max(1, workHeight - 24);

    SIZE maximumPopupSize{};
    for (const DockEntry& entry : dockEntries_)
    {
        const bool folderEntry =
            IsFolderDockEntry(entry);
        int itemCount = 0;
        if (entry.type ==
                DockEntryType::Collection)
        {
            const size_t widgetIndex =
                FindWidgetIndexById(entry.reference);
            if (widgetIndex >= widgets_.size())
                continue;
            itemCount = std::max(
                1, static_cast<int>(
                    GetPopupItemKeys(
                        widgets_[widgetIndex]).size()));
        }
        else if (folderEntry)
        {
            // A folder stack is re-enumerated on open. Reserve the full
            // clamped popup envelope so a large directory cannot resize or
            // clip the floating Dock host after it becomes visible.
            maximumPopupSize.cx = std::max(
                maximumPopupSize.cx,
                static_cast<LONG>(maxWidth));
            maximumPopupSize.cy = std::max(
                maximumPopupSize.cy,
                static_cast<LONG>(maxHeight));
            continue;
        }
        else
            continue;

        int columns = std::clamp(
            std::min(itemCount, 5),
            1, maxColumns);
        int rows =
            (itemCount + columns - 1) /
            columns;
        auto widthForColumns =
            [&](int count) {
                return
                    kCollectionPopupPaddingX * 2 +
                    count * cellWidth +
                    std::max(0, count - 1) *
                        kCollectionPopupGapX;
            };
        auto heightForRows =
            [&](int count) {
                return
                    kCollectionPopupHeaderHeight +
                    count * cellHeight +
                    std::max(0, count - 1) *
                        kCollectionPopupGapY +
                    kCollectionPopupBottomPadding;
            };
        int width =
            widthForColumns(columns);
        int height =
            heightForRows(rows);
        if (height > maxHeight &&
            columns < maxColumns)
        {
            columns = maxColumns;
            rows =
                (itemCount + columns - 1) /
                columns;
            width =
                widthForColumns(columns);
            height =
                heightForRows(rows);
        }
        maximumPopupSize.cx = std::max(
            maximumPopupSize.cx,
            static_cast<LONG>(
                std::min(width, availableWidth)));
        maximumPopupSize.cy = std::max(
            maximumPopupSize.cy,
            static_cast<LONG>(
                std::min(height, maxHeight)));
    }

    const RECT popupEnvelope =
        snowdesktop::floating_dock_rules::
            ReserveCollectionPopupEnvelope(
                dockRect, popupWork,
                dockSettings_.position,
                maximumPopupSize);
    return snowdesktop::floating_dock_rules::
        UnionNonEmptyRects(
            sourceRect, popupEnvelope);
}

inline void DesktopApp::UpdateFloatingDockWindowBounds()
{
    if (!floatingDockVisible_ || !floatingDockHwnd_ ||
        !IsWindow(floatingDockHwnd_) ||
        !floatingDockContainer_)
        return;

    const RECT nextDockRect =
        floatingDockContainer_->GetInteractiveBounds();
    const RECT nextTooltipRect =
        floatingDockContainer_->
            GetHoveredTitleBounds(
                lastMousePoint_);
    RECT nextPopupRect{};
    if (popupAnchoredToDock_)
    {
        if (const DesktopWidget* popupWidget =
                GetOpenPopupWidget())
            nextPopupRect =
                GetCollectionPopupRect(*popupWidget);
    }
    if (IsRectEmpty(&floatingDockSourceRect_))
        floatingDockSourceRect_ =
            CalculateFloatingDockStableSourceRect();
    if (IsRectEmpty(&floatingDockSourceRect_))
    {
        CloseFloatingDock();
        return;
    }
    const bool dockGeometryChanged =
        !EqualRect(&nextDockRect,
            &floatingDockRect_);
    const bool popupRegionChanged =
        !EqualRect(&nextPopupRect,
            &floatingDockPopupRect_);
    const bool titleRegionChanged =
        !EqualRect(&nextTooltipRect,
            &floatingDockTooltipRect_);
    if (!dockGeometryChanged &&
        !popupRegionChanged &&
        !titleRegionChanged)
    {
        InvalidateFloatingDockWindow();
        return;
    }
    floatingDockRect_ = nextDockRect;
    floatingDockPopupRect_ = nextPopupRect;
    floatingDockTooltipRect_ = nextTooltipRect;

    const int width = std::max<LONG>(
        1, floatingDockSourceRect_.right -
            floatingDockSourceRect_.left);
    const int height = std::max<LONG>(
        1, floatingDockSourceRect_.bottom -
            floatingDockSourceRect_.top);

    const RECT dockRegionRect =
        snowdesktop::floating_dock_rules::
            ExpandForBorderOverdraw(
                floatingDockRect_);
    const RECT dockLocal =
        snowdesktop::floating_dock_rules::
            DesktopRectToWindowRect(
                dockRegionRect,
                floatingDockSourceRect_);
    const int radius = std::max(1,
        static_cast<int>(std::round(
            settingsWindow_
                ? settingsWindow_->GetPersonalization().
                    cornerRadius
                : floatingDockPersonalization_.
                    cornerRadius)));
    constexpr int borderOverdraw = 2;
    HRGN windowRegion = CreateRoundRectRgn(
        dockLocal.left, dockLocal.top,
        dockLocal.right + 1, dockLocal.bottom + 1,
        (radius + borderOverdraw) * 2,
        (radius + borderOverdraw) * 2);
    auto appendRegion = [&](
        const RECT& desktopRect,
        int cornerRadius) {
        if (!windowRegion ||
            IsRectEmpty(&desktopRect))
            return;
        const RECT overdrawRect =
            snowdesktop::
                floating_dock_rules::
                    ExpandForBorderOverdraw(
                        desktopRect);
        const RECT local =
            snowdesktop::floating_dock_rules::
                DesktopRectToWindowRect(
                    overdrawRect,
                    floatingDockSourceRect_);
        HRGN addedRegion = CreateRoundRectRgn(
            local.left, local.top,
            local.right + 1,
            local.bottom + 1,
            (cornerRadius +
                borderOverdraw) * 2,
            (cornerRadius +
                borderOverdraw) * 2);
        if (addedRegion)
        {
            CombineRgn(windowRegion, windowRegion,
                addedRegion, RGN_OR);
            DeleteObject(addedRegion);
        }
    };
    appendRegion(
        floatingDockPopupRect_, radius);
    appendRegion(
        floatingDockTooltipRect_, 7);
    if (windowRegion &&
        !SetWindowRgn(
            floatingDockHwnd_, windowRegion, FALSE))
        DeleteObject(windowRegion);

    // Popup and title changes only alter the visible/input region inside the
    // stable host allocation. Do not resize the HWND or recreate its DComp
    // surface after the floating Dock has been revealed.
    if (!dockGeometryChanged &&
        IsWindowVisible(floatingDockHwnd_))
    {
        InvalidateFloatingDockWindow(true);
        return;
    }

    const bool firstReveal =
        floatingDockRevealPending_ ||
        !IsWindowVisible(floatingDockHwnd_);
    if (firstReveal)
    {
        // A no-activate popup can otherwise remain underneath the foreground
        // process. Promote it only for its initial reveal, then return it to
        // the ordinary non-topmost band.
        SetWindowPos(
            floatingDockHwnd_, HWND_TOPMOST,
            floatingDockSourceRect_.left +
                virtualLeft_,
            floatingDockSourceRect_.top +
                virtualTop_,
            width, height,
            SWP_NOACTIVATE |
                (floatingDockRevealPending_
                    ? 0 : SWP_SHOWWINDOW));
    }
    else
    {
        // Opening or closing a collection popup only changes the compact
        // host's geometry. Preserve its existing Z band to avoid a visible
        // TOPMOST -> NOTOPMOST flash.
        SetWindowPos(
            floatingDockHwnd_, nullptr,
            floatingDockSourceRect_.left +
                virtualLeft_,
            floatingDockSourceRect_.top +
                virtualTop_,
            width, height,
            SWP_NOZORDER | SWP_NOACTIVATE |
                SWP_SHOWWINDOW);
    }

    if (!floatingDockBackdropCompositor_.IsAvailable())
    {
        if (!floatingDockBackdropCompositor_.
                InitializePopup(
                    floatingDockHwnd_, false,
                    !floatingDockRevealPending_))
        {
            std::wstring message =
                L"Floating Dock native backdrop unavailable: ";
            message += floatingDockBackdropCompositor_.
                LastError();
            WriteCrashLogEntry(message.c_str());
        }
    }
    if (firstReveal)
    {
        SetWindowPos(
            floatingDockHwnd_, HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOACTIVATE |
                (floatingDockRevealPending_
                    ? 0 : SWP_SHOWWINDOW));
    }
    RECT loggedRect{};
    GetWindowRect(floatingDockHwnd_, &loggedRect);
    wchar_t message[224]{};
    wsprintfW(message,
        L"Floating Dock shown rect=(%ld,%ld)-(%ld,%ld) exStyle=0x%08X topmost=%d",
        loggedRect.left, loggedRect.top,
        loggedRect.right, loggedRect.bottom,
        static_cast<unsigned>(GetWindowLongPtrW(
            floatingDockHwnd_, GWL_EXSTYLE)),
        (GetWindowLongPtrW(
            floatingDockHwnd_, GWL_EXSTYLE) &
            WS_EX_TOPMOST) != 0);
    WriteCrashLogEntry(message);
    InvalidateFloatingDockWindow(true);
}

inline void DesktopApp::ShowFloatingDock()
{
    WriteCrashLogEntry(
        L"Floating Dock shortcut received");
    if (!generalSettings_.dockEnabled)
    {
        WriteCrashLogEntry(
            L"Floating Dock shortcut ignored: feature disabled");
        return;
    }
    if (!CreateFloatingDockWindow())
    {
        WriteCrashLogEntry(
            L"Floating Dock CreateWindowEx failed");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    HideDockWindowPreview();

    // Layout rebuilding may replace every runtime DockContainer. Resolve the
    // pointer at the last possible moment and never retain it while hidden.
    floatingDockContainer_ =
        SelectFloatingDockContainerAtCursor();
    if (!floatingDockContainer_)
    {
        WriteCrashLogEntry(
            L"Floating Dock shortcut ignored: no Dock container");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    const RECT selectedDockBounds =
        floatingDockContainer_->GetBounds();
    const POINT selectedDockCenterScreen{
        (selectedDockBounds.left +
            selectedDockBounds.right) / 2 +
            virtualLeft_,
        (selectedDockBounds.top +
            selectedDockBounds.bottom) / 2 +
            virtualTop_
    };
    floatingDockMonitor_ = MonitorFromPoint(
        selectedDockCenterScreen,
        MONITOR_DEFAULTTONEAREST);
    const RECT desktopDockRect =
        floatingDockContainer_->
            GetInteractiveBounds();
    const RECT desktopDockPanelRect =
        floatingDockContainer_->
            GetVisualPanelBounds(
                lastMousePoint_);
    floatingDockPersonalization_ =
        PersonalizationSettings::DarkPreset();
    if (settingsWindow_)
        floatingDockPersonalization_ =
            settingsWindow_->GetPersonalization();
    else
        LoadPersonalization(
            GetPersonalizationPath().c_str(),
            floatingDockPersonalization_);
    const bool wasFloatingDockVisible =
        floatingDockVisible_;
    floatingDockVisible_ = true;
    if (snowdesktop::floating_dock_rules::
            FloatingVisibilityChangesStaticScene(
                wasFloatingDockVisible,
                floatingDockVisible_))
    {
        // A drag frame caches the desktop Dock as part of its static layer.
        // Remove both that bitmap and its independent backdrop panel before
        // presenting the top-level copy, otherwise both Docks remain visible.
        InvalidateDragStaticScene();
        desktopBackdropCompositor_.RemovePanel(
            desktopDockPanelRect);
    }
    floatingDockRevealPending_ = true;
    UpdateFloatingDockWindowBounds();
    // Commit the floating copy before removing the corresponding desktop
    // copy. A one-frame overlap is visually stable; the reverse order exposes
    // the wallpaper and causes the switch flash reported by users.
    InvalidateFloatingDockWindow(true);
    floatingDockBackdropCompositor_.
        SetVisible(true);
    SetWindowPos(
        floatingDockHwnd_, HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE |
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    floatingDockBackdropCompositor_.
        Reattach(floatingDockHwnd_);
    floatingDockRevealPending_ = false;
    if (hwnd_)
    {
        InvalidateRect(
            hwnd_, &desktopDockRect, TRUE);
        UpdateWindow(hwnd_);
    }
}

inline void DesktopApp::CloseFloatingDock(
    bool closeDockPopup)
{
    if (!floatingDockVisible_ &&
        (!floatingDockHwnd_ ||
            !IsWindowVisible(floatingDockHwnd_)))
        return;
    HideDockWindowPreview();
    if (closeDockPopup && popupAnchoredToDock_ &&
        GetOpenPopupWidget())
    {
        // Clear the popup state directly through the shared close path, while
        // keeping this host marked visible until that path has rebuilt any
        // grouped runtime container.
        CloseCollectionPopup();
    }
    const RECT desktopDockRect =
        floatingDockContainer_
            ? floatingDockContainer_->
                GetInteractiveBounds()
            : floatingDockRect_;
    // Restore the desktop copy first, then hide the floating copy. This keeps
    // the hand-off free of a transparent intermediate frame.
    const bool wasFloatingDockVisible =
        floatingDockVisible_;
    floatingDockVisible_ = false;
    if (snowdesktop::floating_dock_rules::
            FloatingVisibilityChangesStaticScene(
                wasFloatingDockVisible,
                floatingDockVisible_))
    {
        InvalidateDragStaticScene();
    }
    floatingDockRevealPending_ = false;
    if (hwnd_)
    {
        InvalidateRect(
            hwnd_,
            IsRectEmpty(&desktopDockRect)
                ? nullptr : &desktopDockRect,
            TRUE);
        UpdateWindow(hwnd_);
    }
    if (floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
    {
        ShowWindow(floatingDockHwnd_, SW_HIDE);
    }
    floatingDockBackdropCompositor_.Reset();
    floatingDockContainer_ = nullptr;
    floatingDockMonitor_ = nullptr;
    floatingDockSourceRect_ = {};
    floatingDockRect_ = {};
    floatingDockPopupRect_ = {};
    floatingDockTooltipRect_ = {};
}

inline void DesktopApp::ToggleFloatingDock()
{
    if (floatingDockVisible_)
        CloseFloatingDock();
    else
        ShowFloatingDock();
}

inline void DesktopApp::InvalidateFloatingDockWindow(
    bool immediate) const
{
    if (floatingDockVisible_ &&
        floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
    {
        InvalidateRect(
            floatingDockHwnd_, nullptr, FALSE);
        // WM_MOUSEMOVE and OLE DragOver can keep the UI thread's input queue
        // continuously busy. A plain invalidation is then painted only after
        // the queue becomes idle, which makes Dock magnification and insertion
        // previews appear frozen. Force only the already-invalid floating
        // surface to paint; do not resize or rebuild its composition resources.
        if (immediate &&
            !floatingDockCompositionPaintInProgress_)
            UpdateWindow(floatingDockHwnd_);
    }
}

inline POINT DesktopApp::FloatingDockClientToDesktop(
    POINT point) const
{
    return snowdesktop::floating_dock_rules::
        WindowPointToDesktopPoint(
            point, floatingDockSourceRect_);
}

inline HRESULT DesktopApp::
CreateOrResizeFloatingDockCompositionSurface()
{
    if (!dcompDevice_ || !floatingDockHwnd_ ||
        !IsWindow(floatingDockHwnd_))
        return E_UNEXPECTED;
    RECT client{};
    GetClientRect(floatingDockHwnd_, &client);
    const UINT width = static_cast<UINT>(
        std::max<LONG>(1, client.right));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(1, client.bottom));

    if (!floatingDockDcompTarget_)
    {
        HRESULT hr = dcompDevice_->CreateTargetForHwnd(
            floatingDockHwnd_, FALSE,
            &floatingDockDcompTarget_);
        if (FAILED(hr))
            return hr;
    }
    if (!floatingDockDcompVisual_)
    {
        HRESULT hr = dcompDevice_->CreateVisual(
            &floatingDockDcompVisual_);
        if (FAILED(hr) || !floatingDockDcompVisual_)
            return FAILED(hr) ? hr : E_FAIL;
        hr = floatingDockDcompTarget_->SetRoot(
            floatingDockDcompVisual_.Get());
        if (FAILED(hr))
            return hr;
    }
    if (floatingDockDcompSurface_ &&
        floatingDockCompWidth_ == width &&
        floatingDockCompHeight_ == height)
        return S_OK;

    ComPtr<IDCompositionSurface> surface;
    HRESULT hr = dcompDevice_->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &surface);
    if (FAILED(hr))
        return hr;
    hr = floatingDockDcompVisual_->SetContent(
        surface.Get());
    if (FAILED(hr))
        return hr;
    hr = dcompDevice_->Commit();
    if (FAILED(hr))
        return hr;
    floatingDockDcompSurface_ = surface;
    floatingDockCompWidth_ = width;
    floatingDockCompHeight_ = height;
    return S_OK;
}

inline void DesktopApp::
RecoverFloatingDockCompositionFailure(
    const wchar_t* stage, HRESULT hr)
{
    wchar_t message[192]{};
    wsprintfW(message,
        L"FloatingDock %s FAILED hr=0x%08X; resetting composition surface",
        stage ? stage : L"Render",
        static_cast<unsigned>(hr));
    WriteCrashLogEntry(message);
    ResetFloatingDockCompositionResources();
    if (!floatingDockCompositionRenderRecoveryPending_ &&
        floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
    {
        floatingDockCompositionRenderRecoveryPending_ = true;
        InvalidateRect(
            floatingDockHwnd_, nullptr, FALSE);
    }
}

inline void DesktopApp::PaintFloatingDockWindow(
    HWND hwnd)
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    if (!dc)
        return;
    if (floatingDockCompositionPaintInProgress_)
    {
        EndPaint(hwnd, &paint);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    floatingDockCompositionPaintInProgress_ = true;
    struct FloatingPaintScope final
    {
        bool& active;
        ~FloatingPaintScope() { active = false; }
    } paintScope{
        floatingDockCompositionPaintInProgress_
    };

    HRESULT hr =
        CreateOrResizeFloatingDockCompositionSurface();
    if (FAILED(hr))
    {
        RecoverFloatingDockCompositionFailure(
            L"CreateOrResize", hr);
        EndPaint(hwnd, &paint);
        return;
    }

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    // IDCompositionSurface rejects partial BeginDraw rectangles on this
    // HWND-backed path with E_INVALIDARG. The surface allocation is stable
    // for the lifetime of the visible floating Dock, so redraw the existing
    // compact surface without recreating it.
    hr = floatingDockDcompSurface_->BeginDraw(
        nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext),
        &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        RecoverFloatingDockCompositionFailure(
            L"BeginDraw", hr);
        EndPaint(hwnd, &paint);
        return;
    }

    ComPtr<ID2D1DeviceContext> context;
    context.Attach(rawContext);
    context->SetDpi(96.0f, 96.0f);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context->SetTransform(
        D2D1::Matrix3x2F::Translation(
            static_cast<float>(
                updateOffset.x -
                floatingDockSourceRect_.left),
            static_cast<float>(
                updateOffset.y -
                floatingDockSourceRect_.top)));
    context->SetAntialiasMode(
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    context->Clear(
        D2D1::ColorF(0, 0, 0, 0));

    brushCache_.clear();
    brushCacheContext_ = context.Get();
    renderingFloatingDock_ = true;
    floatingDockBackdropCompositor_.BeginFrame(true);
    if (floatingDockContainer_)
    {
        floatingDockContainer_->DrawChrome(
            context.Get(), lastMousePoint_);
        floatingDockContainer_->DrawContents(
            context.Get());
    }
    if (popupAnchoredToDock_ &&
        GetOpenPopupWidget())
        DrawCollectionPopup(context.Get());
    DrawDynamicOverlays(context.Get());
    floatingDockBackdropCompositor_.EndFrame();
    renderingFloatingDock_ = false;

    context->SetTransform(
        D2D1::Matrix3x2F::Identity());
    context.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;

    hr = floatingDockDcompSurface_->EndDraw();
    if (FAILED(hr))
    {
        RecoverFloatingDockCompositionFailure(
            L"EndDraw", hr);
        EndPaint(hwnd, &paint);
        return;
    }
    hr = dcompDevice_->Commit();
    if (FAILED(hr))
    {
        RecoverFloatingDockCompositionFailure(
            L"Commit", hr);
        EndPaint(hwnd, &paint);
        return;
    }
    floatingDockCompositionRenderRecoveryPending_ =
        false;
    EndPaint(hwnd, &paint);
}

inline LRESULT DesktopApp::HandleFloatingDockMessage(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto desktopLParam = [&]() {
        const POINT point = FloatingDockClientToDesktop(
            POINT{ GET_X_LPARAM(lp),
                GET_Y_LPARAM(lp) });
        return MAKELPARAM(point.x, point.y);
    };

    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintFloatingDockWindow(hwnd);
        return 0;
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT tracking{ sizeof(tracking) };
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = hwnd;
        TrackMouseEvent(&tracking);
        handlingFloatingDockInput_ = true;
        OnMouseMove(wp, desktopLParam());
        handlingFloatingDockInput_ = false;
        UpdateFloatingDockWindowBounds();
        PresentPointerInteractionFrame();
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        POINT cursor{};
        if (GetCursorPos(&cursor))
        {
            ScreenToClient(hwnd, &cursor);
            cursor =
                FloatingDockClientToDesktop(
                    cursor);
            if (snowdesktop::
                    floating_dock_rules::
                        IsPointInVisibleLayer(
                            cursor,
                            floatingDockRect_,
                            floatingDockPopupRect_,
                            floatingDockTooltipRect_))
                return 0;
        }
        OnMouseLeave();
        InvalidateFloatingDockWindow(true);
        return 0;
    }
    case WM_LBUTTONDOWN:
        handlingFloatingDockInput_ = true;
        OnLeftButtonDown(wp, desktopLParam());
        handlingFloatingDockInput_ = false;
        InvalidateFloatingDockWindow(true);
        return 0;
    case WM_LBUTTONUP:
        handlingFloatingDockInput_ = true;
        OnLeftButtonUp(
            wp, desktopLParam());
        handlingFloatingDockInput_ = false;
        UpdateFloatingDockWindowBounds();
        InvalidateFloatingDockWindow(true);
        return 0;
    case WM_LBUTTONDBLCLK:
        return HandleMessage(
            hwnd_, msg, wp, desktopLParam());
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        OnMiddleButtonDown(wp, desktopLParam());
        return 0;
    case WM_MBUTTONUP:
        OnMiddleButtonUp(wp, desktopLParam());
        return 0;
    case WM_RBUTTONUP:
        OnRightButtonUp(desktopLParam());
        InvalidateFloatingDockWindow(true);
        return 0;
    case WM_MOUSEWHEEL:
        OnMouseWheel(wp, lp);
        UpdateFloatingDockWindowBounds();
        InvalidateFloatingDockWindow(true);
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        CloseFloatingDock();
        return 0;
    case WM_CLOSE:
        CloseFloatingDock();
        return 0;
    case WM_DESTROY:
        if (floatingDockHwnd_ == hwnd)
            floatingDockHwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
