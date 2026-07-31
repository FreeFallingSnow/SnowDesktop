#include "app.h"

// Desktop animation, dwell and maintenance timer dispatch.

void DesktopApp::OnTimer(WPARAM timerId)
{
    if (timerId == kDesktopPassthroughHoldTimerId)
    {
        if (!desktopPassthroughHoldActive_)
        {
            if (desktopPassthroughHotkeyHwnd_ &&
                IsWindow(desktopPassthroughHotkeyHwnd_))
            {
                KillTimer(desktopPassthroughHotkeyHwnd_,
                    kDesktopPassthroughHoldTimerId);
            }
        }
        else if (!IsDesktopPassthroughHotkeyDown() &&
            !IsDesktopPassthroughPointerDown())
        {
            EndDesktopPassthroughHold();
        }
        return;
    }

    if (timerId >= kWidgetTimerIdBase)
    {
        auto it = widgetTimerIds_.find(static_cast<UINT_PTR>(timerId));
        if (it != widgetTimerIds_.end())
        {
            if (widgetEngine_)
                widgetEngine_->OnWidgetTimer(
                    it->second, static_cast<UINT_PTR>(timerId));
            return;
        }
    }

    if (timerId == kDisplayTopologyRefreshTimerId)
    {
        if (controlHwnd_ && IsWindow(controlHwnd_))
            KillTimer(controlHwnd_, kDisplayTopologyRefreshTimerId);
        if (hwnd_ && IsWindow(hwnd_))
            KillTimer(hwnd_, kDisplayTopologyRefreshTimerId);
        RefreshDisplayTopologyIfChanged();
    }
    else if (timerId == kShellChangeTimerId)
    {
        KillTimer(hwnd_, kShellChangeTimerId);
        if (!mouseDown_ && !reloading_)
            ReloadItems();
    }
    else if (timerId == kRecycleBinPollTimerId)
    {
        const auto pollState = recycleBinPollState_;
        if (pollState->queryInFlight.exchange(true))
            return;
        const HWND target = hwnd_;
        pollState->targetWindow = target;
        std::thread([target, pollState] {
            SHQUERYRBINFO info{};
            info.cbSize = sizeof(info);
            const HRESULT result = SHQueryRecycleBinW(nullptr, &info);
            if (SUCCEEDED(result))
            {
                const int64_t previousCount = pollState->itemCount.exchange(info.i64NumItems);
                if (previousCount >= 0 && previousCount != info.i64NumItems &&
                    pollState->targetWindow.load() == target)
                    PostMessageW(target, kShellChangeMessage, 0, 0);
            }
            pollState->queryInFlight = false;
        }).detach();
    }
    else if (timerId == kDesktopHostWatchTimerId)
    {
        // Restore the Explorer-owned desktop host first. Hook injection can
        // take time while the new taskbar XAML tree is still starting up.
        WatchDesktopHost();
        const DWORD now = GetTickCount();
        if (IsSystemTaskbarHookRequired(dockSettings_) &&
            (systemTaskbarBackdropRefreshTick_ == 0 ||
                now - systemTaskbarBackdropRefreshTick_ >= 1500))
        {
            RefreshSystemTaskbarAppearance(true);
        }
    }
    else if (timerId == kWidgetRefreshTimerId)
    {
        // Keep display hot-plug recovery independent from the hidden control
        // window's host-watch timer. Some display-driver paths leave that
        // timer alive but do not deliver its low-priority WM_TIMER promptly.
        PollDisplayTopology();
        if (widgetEngine_)
            widgetEngine_->TickRuntime();
        const DWORD now = GetTickCount();
        const DWORD foregroundTick =
            dockForegroundChangedTick_.load();
        const DWORD windowStateTick =
            dockWindowListChangedTick_.load();
        const bool dockStateChanged =
            foregroundTick != dockRunningWindowsForegroundTick_ ||
            windowStateTick != dockRunningWindowsStateTick_;
        const bool fallbackRefreshDue =
            dockRunningWindowsRefreshTick_ == 0 ||
            now - dockRunningWindowsRefreshTick_ >= 10000;
        if (generalSettings_.dockEnabled &&
            (dockStateChanged || fallbackRefreshDue))
            RefreshDockRunningWindows();
    }
    else if (timerId == kDockWindowPreviewHoverTimerId)
    {
        OnDockWindowPreviewHoverTimer();
    }
    else if (timerId == kDockLaunchBounceTimerId)
    {
        OnDockLaunchBounceTimer();
    }
    else if (timerId == kFloatingDockEdgeSwipeTimerId)
    {
        UpdateFloatingDockEdgeSwipe();
    }
    else if (timerId == kTaskbarRevealGuardTimerId)
    {
        UpdateSystemTaskbarRevealGuard();
        const DWORD now = GetTickCount();
        const DWORD foregroundTick = dockForegroundChangedTick_.load();
        const DWORD windowStateTick =
            systemTaskbarWindowStateChangedTick_.load();
        const bool foregroundChanged = foregroundTick != 0 &&
            foregroundTick != systemTaskbarBackdropForegroundTick_;
        const bool windowStateChanged = windowStateTick !=
            systemTaskbarWindowStateObservedTick_;
        const bool windowStateRefreshDue =
            windowStateChanged &&
            (systemTaskbarWindowScanTick_ == 0 ||
                now - systemTaskbarWindowScanTick_ >= 250);
        const bool foregroundSettling = foregroundTick != 0 &&
            now - foregroundTick <= 400 &&
            now - systemTaskbarBackdropRefreshTick_ >= 100;
        if (IsSystemTaskbarHookRequired(dockSettings_) &&
            (foregroundChanged || windowStateRefreshDue ||
                foregroundSettling))
        {
            const bool taskbarAppearanceApplied =
                RefreshSystemTaskbarAppearance(true, true);
            systemTaskbarBackdropForegroundTick_ = foregroundTick;
            if (taskbarAppearanceApplied &&
                hwnd_ && IsWindow(hwnd_))
                InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    else if (timerId ==
        kCollectionPopupAnimationTimerId)
    {
        popupAnimation_.Advance(GetTickCount64());
        if (!popupAnimation_.IsAnimating())
        {
            KillTimer(
                hwnd_,
                kCollectionPopupAnimationTimerId);
            if (popupAnimation_.IsHidden())
            {
                FinalizeCloseCollectionPopup();
                return;
            }
        }
        InvalidateCollectionPopupAnimation();
    }
    else if (timerId ==
        kLuaWidgetPanelAnimationTimerId)
    {
        luaWidgetPanelAnimation_.Advance(
            GetTickCount64());
        if (!luaWidgetPanelAnimation_.IsAnimating())
        {
            KillTimer(
                hwnd_,
                kLuaWidgetPanelAnimationTimerId);
            if (luaWidgetPanelAnimation_.IsHidden())
            {
                FinalizeCloseLuaWidgetPanel();
                return;
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (timerId == kCollectionPopupDwellTimerId)
    {
        const size_t candidate =
            popupDwellController_.Candidate();
        if (!dragSession_.IsActive() ||
            candidate >= widgets_.size() ||
            candidate == popupWidgetIndex_)
        {
            popupDwellController_.Reset();
            KillTimer(hwnd_, kCollectionPopupDwellTimerId);
            return;
        }

        if (TryOpenDwellCollectionPopup(GetTickCount()))
        {
            KillTimer(hwnd_, kCollectionPopupDwellTimerId);
            OnMouseMove(0, MAKELPARAM(lastMousePoint_.x, lastMousePoint_.y));
            InvalidateFloatingDockWindow(true);
        }
    }
    else if (timerId == kCollectionGroupTabDwellTimerId)
    {
        if (!dragSession_.IsActive() ||
            collectionGroupTabDwellWidgetIndex_ >=
                widgets_.size() ||
            collectionGroupTabDwellId_.empty())
        {
            collectionGroupTabDwellWidgetIndex_ =
                static_cast<size_t>(-1);
            collectionGroupTabDwellId_.clear();
            KillTimer(
                hwnd_, kCollectionGroupTabDwellTimerId);
            return;
        }

        if (TryActivateCollectionGroupTab(GetTickCount()))
        {
            KillTimer(
                hwnd_, kCollectionGroupTabDwellTimerId);
            OnMouseMove(
                0, MAKELPARAM(
                    lastMousePoint_.x,
                    lastMousePoint_.y));
            InvalidateFloatingDockWindow(true);
        }
    }
    else if (timerId == kDockHandoffDwellTimerId)
    {
        if (!dragSession_.IsActive() || dockHandoffDwellIndex_ == static_cast<size_t>(-1))
        {
            KillTimer(hwnd_, kDockHandoffDwellTimerId);
            dockHandoffDwellReady_ = false;
            return;
        }
        if (GetTickCount() - dockHandoffDwellStartTick_ >= kDockHandoffDwellDelayMs)
        {
            const POINT dwellPoint =
                dragSession_.CurrentPoint();
            DockContainer* dock =
                GetDockContainerAtPoint(
                    dwellPoint);
            DockEntryItem* dockItem =
                dock
                    ? dock->EntryAtPoint(
                        dwellPoint)
                    : nullptr;
            const size_t entryIndex =
                dockItem
                    ? dockItem->
                        GetEntryIndex()
                    : static_cast<size_t>(-1);
            if (entryIndex <
                    dockEntries_.size() &&
                IsFolderDockEntry(
                    dockEntries_[entryIndex]))
            {
                dockHandoffDwellIndex_ =
                    static_cast<size_t>(-1);
                dockHandoffDwellStartTick_ = 0;
                dockHandoffDwellReady_ = false;
                KillTimer(
                    hwnd_,
                    kDockHandoffDwellTimerId);
                OpenDockFolderPopupAt(
                    entryIndex, dwellPoint);
                if (!dragDropController_.IsExternalDragActive())
                    OnMouseMove(
                        0,
                        MAKELPARAM(
                            dwellPoint.x,
                            dwellPoint.y));
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                InvalidateFloatingDockWindow(
                    true);
                return;
            }

            dockHandoffDwellReady_ = true;
            KillTimer(hwnd_, kDockHandoffDwellTimerId);
            if (!dragDropController_.IsExternalDragActive())
                OnMouseMove(0, MAKELPARAM(
                    dragSession_.CurrentPoint().x, dragSession_.CurrentPoint().y));
            InvalidateRect(hwnd_, nullptr, FALSE);
            InvalidateFloatingDockWindow(true);
        }
    }
    else if (timerId == kPageNotifyTimerId)
    {
        // 换页通知覆盖层：定期触发重绘以驱动淡入淡出动画
        if (pageNotifyActive_)
        {
            const DWORD elapsed = GetTickCount() - pageNotifyStartTick_;
            if (elapsed >= kPageNotifyVisibleMs)
            {
                pageNotifyActive_ = false;
                pageNotifyText_.clear();
                KillTimer(hwnd_, kPageNotifyTimerId);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else
        {
            KillTimer(hwnd_, kPageNotifyTimerId);
        }
    }
    else if (timerId == kHiddenHintTimerId)
    {
        const DWORD elapsed = GetTickCount() - hiddenHintStartTick_;
        if (elapsed >= kHiddenHintVisibleMs)
        {
            ClearHiddenHint();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    else if (timerId == kWidgetAddedHintTimerId)
    {
        const DWORD elapsed = GetTickCount() - widgetAddedHintStartTick_;
        if (elapsed >= kWidgetAddedHintVisibleMs)
        {
            ClearWidgetAddedHint();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
}

/**
 * @brief 更新集合弹窗的停留检测逻辑
 * @param point 当前鼠标位置
 */
