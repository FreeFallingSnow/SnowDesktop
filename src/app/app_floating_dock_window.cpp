#include "app.h"

// Floating-Dock window, composition surface and bounds management.

bool DesktopApp::CreateFloatingDockWindow()
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

    OleDragDropAdapter* oleAdapter = EnsureOleDragDropAdapter();
    floatingDockDropTargetRegistered_ = oleAdapter &&
        SUCCEEDED(RegisterDragDrop(
            floatingDockHwnd_,
            static_cast<IDropTarget*>(oleAdapter)));
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(floatingDockHwnd_,
        DWMWA_TRANSITIONS_FORCEDISABLED,
        &disableTransitions, sizeof(disableTransitions));
    return true;
}

void DesktopApp::ResetFloatingDockCompositionResources()
{
    floatingDockFrameReady_ = false;
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    if (floatingDockDcompVisual_)
        floatingDockDcompVisual_->SetContent(nullptr);
    if (floatingDockDesktopCacheVisual_)
        floatingDockDesktopCacheVisual_->SetContent(nullptr);
    if (floatingDockDesktopCacheEffect_)
        floatingDockDesktopCacheEffect_->SetOpacity(0.0f);
    floatingDockDcompSurface_.Reset();
    floatingDockCompWidth_ = 0;
    floatingDockCompHeight_ = 0;
}

HRESULT DesktopApp::EnsureFloatingDockDesktopCacheVisual()
{
    if (!dcompDevice_ || !dcompVisual_)
        return E_UNEXPECTED;
    if (!floatingDockDesktopCacheVisual_)
    {
        ComPtr<IDCompositionVisual2> visual;
        HRESULT hr = dcompDevice_->CreateVisual(&visual);
        if (FAILED(hr) || !visual)
            return FAILED(hr) ? hr : E_FAIL;

        ComPtr<IDCompositionEffectGroup> effect;
        hr = dcompDevice_->CreateEffectGroup(&effect);
        if (FAILED(hr) || !effect)
            return FAILED(hr) ? hr : E_FAIL;
        hr = effect->SetOpacity(0.0f);
        if (SUCCEEDED(hr))
            hr = visual->SetEffect(effect.Get());
        if (SUCCEEDED(hr))
            hr = dcompVisual_->AddVisual(
                visual.Get(), TRUE, nullptr);
        if (FAILED(hr))
            return hr;

        floatingDockDesktopCacheVisual_ = visual;
        floatingDockDesktopCacheEffect_ = effect;
    }

    HRESULT hr = floatingDockDesktopCacheVisual_->SetOffsetX(
        static_cast<float>(floatingDockSourceRect_.left));
    if (SUCCEEDED(hr))
    {
        hr = floatingDockDesktopCacheVisual_->SetOffsetY(
            static_cast<float>(floatingDockSourceRect_.top));
    }
    return hr;
}

void DesktopApp::FinalizeFloatingDockBackdropCleanup(
    UINT_PTR commitToken)
{
    if (commitToken != 0 &&
        floatingDockRevealCommitPending_ &&
        commitToken == floatingDockBackdropCommitToken_)
    {
        floatingDockRevealCommitPending_ = false;
        return;
    }
    if (commitToken != 0)
    {
        if (!floatingDockBackdropCleanupPending_ ||
            commitToken != floatingDockBackdropCommitToken_)
            return;
    }
    if (commitToken != 0 && floatingDockClosePending_)
    {
        floatingDockBackdropCleanupPending_ = false;
        floatingDockClosePending_ = false;
        CompleteFloatingDockCloseHandoff();
        return;
    }
    floatingDockBackdropCleanupPending_ = false;
    floatingDockBackdropCompositor_.SetVisible(false);
    floatingDockBackdropCompositor_.Reset();
    const bool hadCompositionSurface =
        floatingDockDcompSurface_ != nullptr;
    ResetFloatingDockCompositionResources();
    if (hadCompositionSurface && dcompDevice_)
    {
        // The target can release its last full-size surface only after the
        // backdrop ownership transaction has completed. Releasing it in the
        // close call raced WinComp and exposed a one-frame glass hole.
        CommitCompositionAnimationFrame();
        const HRESULT hr = FlushPendingCompositionCommit()
            ? S_OK : E_FAIL;
        if (FAILED(hr))
        {
            wchar_t message[160]{};
            wsprintfW(message,
                L"FloatingDock release surface commit FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteDiagnosticLogEntry(message);
        }
    }
}

void DesktopApp::DestroyFloatingDockWindow()
{
    if (floatingDockHoverTailToken_)
    {
        uiAnimationScheduler_.Cancel(
            floatingDockHoverTailToken_);
        floatingDockHoverTailToken_ = 0;
    }
    floatingDockHoverTargetOwner_ = nullptr;
    floatingDockHoverTargetIndex_ = 0;
    floatingDockHoverTargetKind_ = 0;
    EndFloatingDockKeyboardSession(
        FloatingDockCloseFocusPolicy::PreserveCurrent);
    ++floatingDockBackdropCommitToken_;
    floatingDockBackdropCleanupPending_ = false;
    floatingDockRevealCommitPending_ = false;
    floatingDockClosePending_ = false;
    floatingDockPointerPresentPending_ = false;
    shellPopupMenuLayerDepth_ = 0;
    floatingDockHoverHandoffPending_ = false;
    floatingDockHoverHandoffRect_ = {};
    floatingDockCloseDesktopRect_ = {};
    floatingDockVisible_ = false;
    floatingDockDesktopCopySuppressed_ = false;
    floatingDockRevealPending_ = false;
    floatingDockContainer_ = nullptr;
    floatingDockMonitor_ = nullptr;
    floatingDockSourceRect_ = {};
    floatingDockRect_ = {};
    floatingDockPopupRect_ = {};
    floatingDockTooltipRect_ = {};
    floatingDockDesktopBackdropHandoffRect_ = {};
    floatingDockBackdropCompositor_.Reset();
    if (floatingDockDropTargetRegistered_ &&
        floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
        RevokeDragDrop(floatingDockHwnd_);
    floatingDockDropTargetRegistered_ = false;
    ResetFloatingDockCompositionResources();
    if (dcompVisual_ && floatingDockDesktopCacheVisual_)
    {
        dcompVisual_->RemoveVisual(
            floatingDockDesktopCacheVisual_.Get());
    }
    floatingDockDesktopCacheEffect_.Reset();
    floatingDockDesktopCacheVisual_.Reset();
    floatingDockDcompEffect_.Reset();
    floatingDockDcompVisual_.Reset();
    floatingDockDcompTarget_.Reset();
    if (floatingDockHwnd_ && IsWindow(floatingDockHwnd_))
        DestroyWindow(floatingDockHwnd_);
    floatingDockHwnd_ = nullptr;
    if (floatingDockInputHwnd_ &&
        IsWindow(floatingDockInputHwnd_))
        DestroyWindow(floatingDockInputHwnd_);
    floatingDockInputHwnd_ = nullptr;
}

DockContainer*
DesktopApp::SelectFloatingDockContainerAtCursor() const
{
    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    const HMONITOR cursorMonitor =
        MonitorFromPoint(cursorScreen, MONITOR_DEFAULTTONEAREST);
    return SelectFloatingDockContainerForMonitor(
        cursorMonitor);
}

DockContainer*
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

RECT DesktopApp::
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
            itemCount = static_cast<int>(
                GetPopupItemKeys(
                    widgets_[widgetIndex]).size());
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

        int columns =
            snowdesktop::
                collection_popup_layout::
                    PreferredColumnCount(
                        static_cast<size_t>(
                            itemCount),
                        maxColumns);
        int rows =
            snowdesktop::
                collection_popup_layout::
                    RequiredRowCount(
                        static_cast<size_t>(
                            itemCount),
                        columns);
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
        if (itemCount > 0 &&
            height > maxHeight &&
            columns < maxColumns)
        {
            columns = maxColumns;
            rows =
                snowdesktop::
                    collection_popup_layout::
                        RequiredRowCount(
                            static_cast<size_t>(
                                itemCount),
                            columns);
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

void DesktopApp::UpdateFloatingDockWindowBounds(
    bool immediatePresent)
{
    if (!floatingDockVisible_ || !floatingDockHwnd_ ||
        !IsWindow(floatingDockHwnd_) ||
        !floatingDockContainer_)
        return;
    const bool floatingLayerTopmost =
        snowdesktop::floating_dock_rules::
            ShouldFloatingDockBeTopmost(
                true,
                shellPopupMenuLayerDepth_);

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
    // The host allocation must always contain the real popup envelope. Dock
    // entries can gain items, or the popup can anchor to a hovered slot, after
    // the initial reveal, so expand the stable source rect instead of keeping
    // a stale smaller host that clips the popup until the next refresh.
    RECT requiredSourceRect =
        CalculateFloatingDockStableSourceRect();
    if (!IsRectEmpty(&nextPopupRect))
    {
        requiredSourceRect =
            snowdesktop::floating_dock_rules::
                UnionNonEmptyRects(
                    requiredSourceRect,
                    nextPopupRect);
    }
    const RECT previousSourceRect =
        floatingDockSourceRect_;
    if (IsRectEmpty(&floatingDockSourceRect_))
        floatingDockSourceRect_ = requiredSourceRect;
    else
    {
        const RECT expandedSourceRect =
            snowdesktop::floating_dock_rules::
                UnionNonEmptyRects(
                    floatingDockSourceRect_,
                    requiredSourceRect);
        if (!EqualRect(&expandedSourceRect,
                &floatingDockSourceRect_))
            floatingDockSourceRect_ =
                expandedSourceRect;
    }
    if (IsRectEmpty(&floatingDockSourceRect_))
    {
        CloseFloatingDock(true, true);
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
    const bool sourceRectChanged =
        !EqualRect(&previousSourceRect,
            &floatingDockSourceRect_);
    if (!dockGeometryChanged &&
        !popupRegionChanged &&
        !titleRegionChanged &&
        !sourceRectChanged)
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
        !sourceRectChanged &&
        IsWindowVisible(floatingDockHwnd_))
    {
        InvalidateFloatingDockWindow(
            immediatePresent);
        return;
    }

    const bool firstReveal =
        floatingDockRevealPending_ ||
        !IsWindowVisible(floatingDockHwnd_);
    if (firstReveal)
    {
        // A no-activate popup can otherwise remain underneath the foreground
        // process. Keep the floating host topmost for its entire visible
        // lifetime so shell operations and app windows cannot bury it.
        SetWindowPos(
            floatingDockHwnd_,
            floatingLayerTopmost
                ? HWND_TOPMOST : HWND_NOTOPMOST,
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
        // Resize within the current Z band. Popup-only changes normally keep
        // the stable allocation, while a larger data set can expand it.
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
    bool renderedResizeFrame = false;
    if (!firstReveal && sourceRectChanged)
    {
        // The host is now resized to the expanded source rect. Submit the
        // replacement frame synchronously so the new region never presents
        // the stale pre-resize surface (blank/empty panel) before the next
        // paint. The frame renderer is reentrancy-guarded; on failure the
        // ordinary invalidation below remains the fallback.
        renderedResizeFrame =
            RenderFloatingDockCompositionFrame();
    }

    if (sourceRectChanged &&
        floatingDockBackdropCompositor_.IsAvailable())
    {
        // The native backdrop helper is a sibling sized to the content HWND.
        // Keep its window placement in sync after the host expands so glass
        // panels are not clipped to the old host bounds.
        floatingDockBackdropCompositor_.
            SetPopupTopmost(floatingLayerTopmost);
        floatingDockBackdropCompositor_.
            Reattach(floatingDockHwnd_);
        FlushPendingCompositionCommit();
    }

    if (!floatingDockBackdropCompositor_.IsAvailable())
    {
        if (!floatingDockBackdropCompositor_.
                InitializePopup(
                    floatingDockHwnd_,
                    floatingLayerTopmost,
                    !floatingDockRevealPending_))
        {
            std::wstring message =
                L"Floating Dock native backdrop unavailable: ";
            message += floatingDockBackdropCompositor_.
                LastError();
            WriteDiagnosticLogEntry(message.c_str());
        }
    }
    if (firstReveal)
    {
        SetWindowPos(
            floatingDockHwnd_,
            floatingLayerTopmost
                ? HWND_TOPMOST : HWND_NOTOPMOST,
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
    WriteDiagnosticLogEntry(message);
    if (!renderedResizeFrame)
        InvalidateFloatingDockWindow(
            immediatePresent);
}
