#include "app.h"
#include "../drag_input_rules.h"
#include "../ole_drag_rules.h"
#include "../collection_titleless_rules.h"

// Desktop animation, dwell and maintenance timer dispatch.

void DesktopApp::PollSteamWorkshopSubscriptions(bool bypassThrottle)
{
    if (!widgetEngine_) return;

    std::optional<SteamWorkshopSubscriptionPollState::ReadyQuery> ready;
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
        const bool hostReloadWouldDefer =
            shellFileOperationInFlight_ > 0 || reloading_ ||
            snowdesktop::drag_input_rules::ShouldDeferModelReload(
                dragSession_.HasContext(),
                dragDropController_.IsTransportActive());
        bool settingsCompletionReady = false;
        if (hostReloadWouldDefer)
        {
            std::lock_guard lock(
                steamWorkshopSubscriptionPollState_->mutex);
            settingsCompletionReady = std::any_of(
                steamWorkshopSubscriptionPollState_->settingsCompletions.
                    begin(),
                steamWorkshopSubscriptionPollState_->settingsCompletions.
                    end(),
                [&ready](const auto& completion) {
                    return completion.queryId <= ready->queryId;
                });
            if (settingsCompletionReady &&
                (!steamWorkshopSubscriptionPollState_->ready ||
                    steamWorkshopSubscriptionPollState_->ready->queryId <
                        ready->queryId))
            {
                steamWorkshopSubscriptionPollState_->ready =
                    std::move(*ready);
            }
        }
        if (settingsCompletionReady)
        {
            // A settings operation is complete only after ReloadItems has
            // actually reconciled the persistent desktop instance table.
            // Retain the authoritative query while drag/Shell/reload state
            // would make ReloadItems defer or return early.
            return;
        }

        bool synchronizationSucceeded = false;
        bool hostReloaded = false;
        std::wstring synchronizationMessage;
        if (ready->snapshot.authoritative)
        {
            const auto result =
                widgetEngine_->ApplySteamWorkshopSubscriptions(
                    ready->snapshot);
            if (result.Changed())
            {
                ReloadItems(false);
                hostReloaded = true;
            }
            if (result.errors.empty())
            {
                steamWorkshopSubscriptionLastError_.clear();
                synchronizationSucceeded = true;
                synchronizationMessage = _LW(
                    "settings.widgets.source.syncCompleted");
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
                synchronizationMessage = Utf8ToWide(combined);
            }
        }
        else if (!ready->snapshot.error.empty())
        {
            synchronizationMessage = Utf8ToWide(ready->snapshot.error);
            if (ready->snapshot.error !=
                steamWorkshopSubscriptionLastError_)
            {
                steamWorkshopSubscriptionLastError_ =
                    ready->snapshot.error;
                const std::wstring message = Utf8ToWide(
                    "Steam Workshop subscription query: " +
                    ready->snapshot.error);
                WriteDiagnosticLogEntry(message.c_str());
            }
        }
        if (synchronizationMessage.empty())
            synchronizationMessage = _LW(
                "settings.widgets.source.syncFailed");
        std::vector<SteamWorkshopSubscriptionPollState::
            SettingsCompletion> completions;
        {
            std::lock_guard lock(
                steamWorkshopSubscriptionPollState_->mutex);
            auto& pending = steamWorkshopSubscriptionPollState_->
                settingsCompletions;
            auto item = pending.begin();
            while (item != pending.end())
            {
                if (item->queryId <= ready->queryId)
                {
                    completions.push_back(std::move(*item));
                    item = pending.erase(item);
                }
                else
                {
                    ++item;
                }
            }
        }
        const bool hasUnsubscribeCompletion = std::any_of(
            completions.begin(), completions.end(), [](const auto& value) {
                return !value.expectedUnsubscribedPublishedFileId.empty();
            });
        if (synchronizationSucceeded && hasUnsubscribeCompletion &&
            !hostReloaded)
        {
            // Run the same host reconciliation pass even if the managed
            // package was already absent. Completion means both the package
            // registry have been coordinated. Persisted desktop instances are
            // intentionally retained as placeholders, matching local package
            // uninstall semantics so reinstalling can restore their layout.
            ReloadItems(false);
            hostReloaded = true;
        }
        if (settingsWindow_)
            settingsWindow_->RefreshWidgetsPage();

        const auto reconciledPackages = WidgetEngine::ListWidgetPackages();
        for (auto& completion : completions)
        {
            if (!completion.done)
                continue;
            bool completionSucceeded = synchronizationSucceeded;
            std::wstring completionMessage = synchronizationMessage;
            if (completionSucceeded &&
                !completion.expectedUnsubscribedPublishedFileId.empty())
            {
                const std::string& expectedPublishedFileId =
                    completion.expectedUnsubscribedPublishedFileId;
                const bool stillSubscribed = std::any_of(
                    ready->snapshot.subscribedPublishedFileIds.begin(),
                    ready->snapshot.subscribedPublishedFileIds.end(),
                    [&expectedPublishedFileId](const auto& value) {
                        return snowdesktop::widget::SteamPublishedFileId(
                                   value) == expectedPublishedFileId;
                    });
                const bool managedPackageRemains = std::any_of(
                    reconciledPackages.begin(), reconciledPackages.end(),
                    [&expectedPublishedFileId](const auto& package) {
                        return !package.builtin && !package.development &&
                            package.source.providerId == "steam-workshop" &&
                            snowdesktop::widget::SteamPublishedFileId(
                                package.source.externalItemId) ==
                                expectedPublishedFileId;
                    });
                const bool expectedPackageRemains = std::any_of(
                    reconciledPackages.begin(), reconciledPackages.end(),
                    [&completion](const auto& package) {
                        if (package.builtin || package.development)
                            return false;
                        return std::find(
                            completion.expectedRemovedPackageIds.begin(),
                            completion.expectedRemovedPackageIds.end(),
                            package.manifest.id) !=
                            completion.expectedRemovedPackageIds.end();
                    });
                completionSucceeded = !stillSubscribed &&
                    !managedPackageRemains && !expectedPackageRemains;
                if (!completionSucceeded)
                {
                    completionMessage = _LW(
                        "settings.widgets.workshop.unsubscribeNotReconciled");
                }
            }
            completion.done(completionSucceeded
                    ? snowdesktop::winui::WidgetsPageHostOperationResult::
                          Success(true, completionMessage)
                    : snowdesktop::winui::WidgetsPageHostOperationResult::
                          Failure(completionMessage));
        }
    }

    if (steamWorkshopSubscriptionPollState_->queryInFlight.load())
    {
        if (bypassThrottle)
            steamWorkshopSubscriptionPollState_->refreshPending.store(
                true);
        return;
    }
    bypassThrottle = bypassThrottle || steamWorkshopSubscriptionPollState_->
        refreshPending.exchange(false);
    const DWORD now = GetTickCount();
    if (!bypassThrottle && steamWorkshopSubscriptionLastQueryTick_ != 0 &&
        now - steamWorkshopSubscriptionLastQueryTick_ <
            kSteamWorkshopSubscriptionFallbackPollIntervalMs)
        return;
    steamWorkshopSubscriptionLastQueryTick_ = now;
    const std::string locale = Locale::Instance().GetEffectiveLanguage();
    const auto installedPackages = widgetEngine_->ListWidgetPackages();
    const auto subscriptionHistory =
        WidgetEngine::GetSteamWorkshopSubscriptionHistory();
    const auto packageStaging =
        WidgetEngine::GetWidgetPackagePaths().staging;
    const auto state = steamWorkshopSubscriptionPollState_;
    const HWND notifyWindow = hwnd_;
    std::uint64_t queryId = 0;
    {
        std::lock_guard lock(state->mutex);
        queryId = state->nextQueryId++;
        state->activeQueryId = queryId;
        state->queryInFlight.store(true);
    }
    std::thread([state, locale, notifyWindow, queryId,
        installedPackages, subscriptionHistory, packageStaging]
    {
        auto snapshot =
            WidgetEngine::QuerySteamWorkshopSubscriptions(locale);
        snowdesktop::widget::ResolveSteamWorkshopSubscriptionRemovals(
            snapshot, subscriptionHistory);
        WidgetEngine::PrepareSteamWorkshopSubscriptionArtifacts(snapshot,
            installedPackages, packageStaging);
        {
            std::lock_guard lock(state->mutex);
            state->ready = SteamWorkshopSubscriptionPollState::ReadyQuery{
                queryId, std::move(snapshot)};
            if (state->activeQueryId == queryId)
                state->activeQueryId = 0;
            state->queryInFlight.store(false);
        }
        if (notifyWindow)
            PostMessageW(notifyWindow,
                kSteamWorkshopSubscriptionReadyMessage, 0, 0);
    }).detach();
}

void DesktopApp::RefreshDwellDragTarget(POINT clientPoint)
{
    using snowdesktop::ole_drag_rules::DwellTargetRefreshRoute;
    const DwellTargetRefreshRoute route =
        snowdesktop::ole_drag_rules::SelectDwellTargetRefreshRoute(
            dragDropController_.IsSelfDragActive(),
            dragDropController_.IsExternalDragActive());
    if (route == DwellTargetRefreshRoute::NativePointer)
    {
        OnMouseMoveAt(0, clientPoint);
        return;
    }
    if (route != DwellTargetRefreshRoute::SelfOleDragOver)
        return;

    DWORD keyState = MK_LBUTTON;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        keyState |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)
        keyState |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        keyState |= MK_SHIFT;
    POINT screenPoint = clientPoint;
    ClientToScreen(hwnd_, &screenPoint);
    DWORD effect = DROPEFFECT_COPY |
        DROPEFFECT_MOVE | DROPEFFECT_LINK;
    HandleOleDragOver(
        keyState,
        POINTL{ screenPoint.x, screenPoint.y },
        &effect);
}

void DesktopApp::OnTimer(WPARAM timerId)
{
    if (timerId == kSettingsWindowRetryTimerId)
    {
        if (controlHwnd_ && IsWindow(controlHwnd_))
            KillTimer(controlHwnd_, kSettingsWindowRetryTimerId);
        TryShowPendingSettingsWindow();
        return;
    }

    if (timerId == kOleDragUiPumpTimerId)
    {
        if (!dragDropController_.IsSelfDragActive())
        {
            if (hwnd_ && IsWindow(hwnd_))
                KillTimer(hwnd_, kOleDragUiPumpTimerId);
            return;
        }

        // DoDragDrop dispatches window messages but does not return control to
        // DesktopApp::Run. Bridge its nested loop to the waitable scheduler so
        // popup/hit animations and their completion callbacks keep advancing.
        HANDLE animationWait = uiAnimationScheduler_.WaitHandle();
        if (animationWait &&
            WaitForSingleObject(animationWait, 0) == WAIT_OBJECT_0)
        {
            uiAnimationScheduler_.DispatchDue();
        }
        FlushPendingCompositionCommit();
        FlushPendingQuickNavigationCompositionCommit();
        return;
    }

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
        if (shellFileOperationInFlight_ > 0)
            return;
        const bool deferForDrag =
            snowdesktop::drag_input_rules::ShouldDeferModelReload(
                dragSession_.HasContext(),
                dragDropController_.IsTransportActive());
        if (mouseDown_ || reloading_ || deferForDrag)
        {
            SetTimer(hwnd_, kShellChangeTimerId,
                kShellChangeDebounceMs, nullptr);
            return;
        }
        if (shellReloadPending_)
        {
            const bool reloadLayoutFromDisk =
                shellReloadLayoutFromDiskPending_;
            shellReloadPending_ = false;
            shellReloadLayoutFromDiskPending_ = false;
            ReloadItems(reloadLayoutFromDisk);
        }
        if (shellDockFolderPopupRefreshPending_)
        {
            shellDockFolderPopupRefreshPending_ = false;
            RefreshDockFolderPopup();
        }
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
        // This periodic sample may restore hover after a missed mouse message,
        // but its reconcile mode still enforces the foreground-settle delay in
        // case the free-running timer lands inside the WindowFromPoint race.
        // The immediate foreground callback remains deactivation-only.
        ReconcileDesktopHoverState(
            snowdesktop::desktop_hover_rules::
                ReconcileMode::AllowActivationAfterForegroundSettle);
        // Keep display hot-plug recovery independent from the hidden control
        // window's host-watch timer. Some display-driver paths leave that
        // timer alive but do not deliver its low-priority WM_TIMER promptly.
        PollDisplayTopology();
        if (widgetEngine_)
            widgetEngine_->TickRuntime();
        PollSettingsUpdateCheck();
        if (uiAnimationScheduler_.DiagnosticsEnabled())
            PublishSettingsUpdateStatus();
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
        if (!collectionPopupDwellTimerArmed_)
            return;

        if (TryOpenDwellCollectionPopup(GetTickCount()))
        {
            OnMouseMoveAt(0, lastMousePoint_);
            PresentPointerInteractionFrame();
            InvalidateFloatingDockWindow(true);
        }
    }
    else if (timerId == kCollectionGroupTabDwellTimerId)
    {
        if (!collectionGroupTabDwellTimerArmed_)
            return;

        if (TryActivateCollectionGroupTab(GetTickCount()))
        {
            OnMouseMoveAt(0, lastMousePoint_);
            PresentPointerInteractionFrame();
            InvalidateFloatingDockWindow(true);
        }
    }
    else if (timerId == kDockHandoffDwellTimerId)
    {
        if (!dragSession_.IsActive() || dockHandoffDwellIndex_ == static_cast<size_t>(-1))
        {
            ResetDockHandoffDwell();
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
                ResetDockHandoffDwell();
                OpenDockFolderPopupAt(
                    entryIndex, dwellPoint);
                RefreshDwellDragTarget(dwellPoint);
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
            RefreshDwellDragTarget(
                dragSession_.CurrentPoint());
            InvalidateRect(hwnd_, nullptr, FALSE);
            PresentPointerInteractionFrame();
            PresentDesktopPointerUpdate();
            InvalidateFloatingDockWindow(true);
        }
    }
    else if (timerId == kCompactCollectionHandoffDwellTimerId)
    {
        auto* collection = dynamic_cast<Collection*>(
            dragSession_.TargetContainer());
        DesktopWidget* widget = collection
            ? collection->GetWidgetData() : nullptr;
        Slot* targetSlot = dragSession_.TargetSlot();
        const bool targetMatches =
            dragSession_.IsActive() && widget && targetSlot &&
            widget->id == compactCollectionHandoffWidgetId_ &&
            targetSlot->GetIndex() ==
                compactCollectionHandoffIndex_ &&
            snowdesktop::collection_titleless_rules::IsActive(
                widget->largeFolderTitleless,
                widget->scrollContainerMode,
                widget->gridSpan.columns,
                widget->gridSpan.rows);
        if (!targetMatches)
        {
            ResetCompactCollectionHandoffDwell();
            return;
        }
        if (snowdesktop::collection_titleless_rules::
                IsHandoffDwellReady(
                    true, compactCollectionHandoffReady_,
                    GetTickCount() -
                        compactCollectionHandoffStartTick_,
                    kCompactCollectionHandoffDwellDelayMs))
        {
            compactCollectionHandoffReady_ = true;
            KillTimer(hwnd_,
                kCompactCollectionHandoffDwellTimerId);
            RefreshDwellDragTarget(
                dragSession_.CurrentPoint());
            InvalidateRect(hwnd_, nullptr, FALSE);
            PresentPointerInteractionFrame();
            PresentDesktopPointerUpdate();
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
