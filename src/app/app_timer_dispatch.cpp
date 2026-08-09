#include "app.h"

// Desktop animation, dwell and maintenance timer dispatch.

void DesktopApp::PollSteamWorkshopSubscriptions()
{
    if (!widgetEngine_ || !WidgetEngine::IsSteamWorkshopBridgeAvailable())
        return;

    std::optional<snowdesktop::widget::SteamWorkshopSubscriptionSnapshot>
        ready;
    {
        std::lock_guard lock(steamWorkshopSubscriptionPollState_->mutex);
        if (steamWorkshopSubscriptionPollState_->ready)
        {
            ready = std::move(steamWorkshopSubscriptionPollState_->ready);
            steamWorkshopSubscriptionPollState_->ready.reset();
        }
    }
    if (ready)
    {
        if (ready->authoritative)
        {
            const auto result =
                widgetEngine_->ApplySteamWorkshopSubscriptions(*ready);
            if (result.Changed()) ReloadItems(false);
            if (result.errors.empty())
            {
                steamWorkshopSubscriptionLastError_.clear();
            }
            else
            {
                std::string combined;
                for (const auto& error : result.errors)
                {
                    if (!combined.empty()) combined += " | ";
                    combined += error;
                }
                if (combined != steamWorkshopSubscriptionLastError_)
                {
                    steamWorkshopSubscriptionLastError_ = combined;
                    const std::wstring message = Utf8ToWide(
                        "Steam Workshop subscription sync: " + combined);
                    WriteDiagnosticLogEntry(message.c_str());
                }
            }
        }
        else if (!ready->error.empty() &&
            ready->error != steamWorkshopSubscriptionLastError_)
        {
            steamWorkshopSubscriptionLastError_ = ready->error;
            const std::wstring message = Utf8ToWide(
                "Steam Workshop subscription query: " + ready->error);
            WriteDiagnosticLogEntry(message.c_str());
        }
    }

    const DWORD now = GetTickCount();
    if (steamWorkshopSubscriptionPollState_->queryInFlight.load() ||
        (steamWorkshopSubscriptionLastQueryTick_ != 0 &&
            now - steamWorkshopSubscriptionLastQueryTick_ <
                kSteamWorkshopSubscriptionPollIntervalMs))
        return;
    steamWorkshopSubscriptionLastQueryTick_ = now;
    const std::string locale = Locale::Instance().GetEffectiveLanguage();
    const auto state = steamWorkshopSubscriptionPollState_;
    state->queryInFlight.store(true);
    std::thread([state, locale]
    {
        auto snapshot =
            WidgetEngine::QuerySteamWorkshopSubscriptions(locale);
        {
            std::lock_guard lock(state->mutex);
            state->ready = std::move(snapshot);
        }
        state->queryInFlight.store(false);
    }).detach();
}

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
        // 兜底轮询。当前轻量检测为毫秒级，间隔保持 2s；
        // 自适应退避为防御性兜底：若未来检测变慢（如恢复
        // SHQueryRecycleBinW 全量统计），间隔按耗时自动拉长，
        // 保证每秒 CPU 占用率不随回收站内容膨胀而增长。
        const DWORD duration =
            recycleBinPollState_->lastQueryDurationMs.load();
        UINT nextInterval = kRecycleBinPollIntervalMs;
        if (duration >= 30000)
            nextInterval = kRecycleBinPollHugeIntervalMs;
        else if (duration >= 5000)
            nextInterval = kRecycleBinPollVeryLongIntervalMs;
        else if (duration >= 1000)
            nextInterval = kRecycleBinPollLongIntervalMs;
        else if (duration >= 200)
            nextInterval = kRecycleBinPollMediumIntervalMs;
        if (nextInterval != recycleBinPollIntervalMs_ &&
            hwnd_ && IsWindow(hwnd_))
            SetTimer(hwnd_, kRecycleBinPollTimerId,
                nextInterval, nullptr);
        recycleBinPollIntervalMs_ = nextInterval;
        CheckRecycleBinStatus();
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
        // Foreground WinEvent delivery is normally immediate. This periodic
        // reconciliation also repairs hover state after a missed/late Shell
        // transition without waiting for another mouse message.
        ReconcileDesktopHoverState();
        // Keep display hot-plug recovery independent from the hidden control
        // window's host-watch timer. Some display-driver paths leave that
        // timer alive but do not deliver its low-priority WM_TIMER promptly.
        PollDisplayTopology();
        if (widgetEngine_)
            widgetEngine_->TickRuntime();
        PollSteamWorkshopSubscriptions();
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
            PresentPointerInteractionFrame();
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
            PresentPointerInteractionFrame();
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
                {
                    OnMouseMove(
                        0,
                        MAKELPARAM(
                            dwellPoint.x,
                            dwellPoint.y));
                }
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                PresentPointerInteractionFrame();
                PresentDesktopPointerUpdate();
                InvalidateFloatingDockWindow(
                    true);
                return;
            }

            dockHandoffDwellReady_ = true;
            KillTimer(hwnd_, kDockHandoffDwellTimerId);
            if (!dragDropController_.IsExternalDragActive())
            {
                OnMouseMove(0, MAKELPARAM(
                    dragSession_.CurrentPoint().x, dragSession_.CurrentPoint().y));
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            PresentPointerInteractionFrame();
            PresentDesktopPointerUpdate();
            InvalidateFloatingDockWindow(true);
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
