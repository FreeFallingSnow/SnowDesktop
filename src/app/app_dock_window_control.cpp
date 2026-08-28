#include "app.h"
#include "dock_platform_helpers.h"

// Dock window closing and hover-preview control.

void DesktopApp::PruneDockPendingCloseWindows()
{
    const ULONGLONG now = GetTickCount64();
    std::erase_if(dockPendingCloseWindows_,
        [now](const auto& entry) {
            return !entry.first ||
                !IsWindow(entry.first) ||
                now - entry.second >=
                    kDockWindowClosePendingTimeoutMs;
        });
}

bool DesktopApp::IsDockWindowClosePending(HWND window)
{
    PruneDockPendingCloseWindows();
    if (!window)
        return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root)
        root = window;
    return dockPendingCloseWindows_.contains(root);
}

bool DesktopApp::IsDockAppClosePending(
    const DockAppIdentity& identity)
{
    if (identity.kind == DockAppIdentityKind::None)
        return false;
    PruneDockPendingCloseWindows();
    return std::any_of(
        dockPendingCloseWindows_.begin(),
        dockPendingCloseWindows_.end(),
        [&identity](const auto& entry) {
            return DockWindowMatchesAppIdentity(
                entry.first, identity);
        });
}

bool DesktopApp::RequestTrackedDockWindowClose(
    HWND window)
{
    if (!window || !IsWindow(window))
        return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root)
        root = window;

    CancelDockWindowActivationObservation(root);
    dockPendingCloseWindows_[root] = GetTickCount64();
    if (RequestDockWindowClose(root))
        return true;

    dockPendingCloseWindows_.erase(root);
    return false;
}

std::vector<DockWindowPreviewItem>
DesktopApp::CollectDockWindowPreviewItems(
    const DockAppIdentity& identity,
    bool includeCloaked,
    bool preferTaskbarDocumentProxies)
{
    PruneDockPendingCloseWindows();
    struct TaskbarDocumentProxyCohort
    {
        DWORD processId = 0;
        std::wstring className;
        LONG_PTR windowStyle = 0;
        LONG_PTR extendedStyle = 0;
        std::vector<DockWindowPreviewItem> items;
        std::vector<std::wstring> distinctTitles;
    };
    struct PreviewEnumerationContext
    {
        DesktopApp* owner = nullptr;
        const DockAppIdentity* identity = nullptr;
        std::vector<DockWindowPreviewItem>* regularItems = nullptr;
        std::vector<TaskbarDocumentProxyCohort>* proxyCohorts = nullptr;
        const std::unordered_map<HWND, ULONGLONG>*
            pendingCloseWindows = nullptr;
        bool includeCloaked = false;
        bool preferTaskbarDocumentProxies = false;
    };

    std::vector<DockWindowPreviewItem> regularItems;
    std::vector<TaskbarDocumentProxyCohort> proxyCohorts;
    PreviewEnumerationContext context{
        this, &identity, &regularItems, &proxyCohorts,
        &dockPendingCloseWindows_, includeCloaked,
        preferTaskbarDocumentProxies
    };
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* context = reinterpret_cast<
            PreviewEnumerationContext*>(parameter);
        if (!context || !context->identity ||
            !context->regularItems || !context->proxyCohorts ||
            (context->pendingCloseWindows &&
             context->pendingCloseWindows->contains(window)))
            return TRUE;

        const bool taskWindow = IsDockTaskWindow(window);
        const bool taskbarDocumentProxyCandidate =
            context->preferTaskbarDocumentProxies &&
            IsDockTaskbarDocumentProxyCandidate(window);
        if (!taskWindow && !taskbarDocumentProxyCandidate)
            return TRUE;

        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        const bool ownedByCurrentProcess =
            processId == GetCurrentProcessId();
        const bool applicationLevelWindow =
            context->owner &&
            context->owner->IsSettingsApplicationWindow(window);
        if (!processId ||
            !snowdesktop::dock_window_rules::
                IsTaskWindowProcessEligible(
                    ownedByCurrentProcess,
                    applicationLevelWindow))
            return TRUE;

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(
                window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
            cloaked != 0 &&
            !context->includeCloaked)
            return TRUE;
        if (!DockWindowMatchesAppIdentity(window, *context->identity))
            return TRUE;

        wchar_t titleBuffer[512]{};
        GetWindowTextW(window, titleBuffer,
            static_cast<int>(std::size(titleBuffer)));
        std::wstring title = titleBuffer;
        if (taskbarDocumentProxyCandidate && title.empty())
            return TRUE;
        if (title.empty())
        {
            const std::wstring executablePath =
                QueryDockWindowExecutablePath(window);
            title = PathFindFileNameW(executablePath.c_str());
        }
        if (taskbarDocumentProxyCandidate)
        {
            wchar_t classNameBuffer[256]{};
            if (!GetClassNameW(
                    window, classNameBuffer,
                    static_cast<int>(std::size(classNameBuffer))))
                return TRUE;
            const std::wstring className = classNameBuffer;
            const LONG_PTR windowStyle =
                GetWindowLongPtrW(window, GWL_STYLE);
            const LONG_PTR extendedStyle =
                GetWindowLongPtrW(window, GWL_EXSTYLE);
            auto cohort = std::find_if(
                context->proxyCohorts->begin(),
                context->proxyCohorts->end(),
                [processId, &className,
                 windowStyle, extendedStyle](const auto& item) {
                    return item.processId == processId &&
                        item.windowStyle == windowStyle &&
                        item.extendedStyle == extendedStyle &&
                        _wcsicmp(
                            item.className.c_str(),
                            className.c_str()) == 0;
                });
            if (cohort == context->proxyCohorts->end())
            {
                context->proxyCohorts->push_back({
                    processId, className,
                    windowStyle, extendedStyle
                });
                cohort = context->proxyCohorts->end() - 1;
            }
            const bool distinctTitle = std::none_of(
                cohort->distinctTitles.begin(),
                cohort->distinctTitles.end(),
                [&title](const std::wstring& existing) {
                    return _wcsicmp(
                        existing.c_str(), title.c_str()) == 0;
                });
            if (distinctTitle)
                cohort->distinctTitles.push_back(title);
            cohort->items.push_back(
                { window, std::move(title) });
        }
        else
        {
            context->regularItems->push_back(
                { window, std::move(title) });
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));

    TaskbarDocumentProxyCohort* selectedCohort = nullptr;
    std::size_t qualifyingCohortCount = 0;
    for (TaskbarDocumentProxyCohort& cohort : proxyCohorts)
    {
        if (!snowdesktop::dock_window_rules::
                IsTaskbarDocumentProxyCohortEligible(
                    cohort.items.size(),
                    cohort.distinctTitles.size()))
        {
            continue;
        }
        ++qualifyingCohortCount;
        if (!selectedCohort)
            selectedCohort = &cohort;
    }
    if (selectedCohort &&
        snowdesktop::dock_window_rules::
            ShouldPreferTaskbarDocumentProxyCohort(
                qualifyingCohortCount))
    {
        return std::move(selectedCohort->items);
    }
    return regularItems;
}

void DesktopApp::CloseDockWindowFromPreview(
    HWND window)
{
    DismissDockWindowPreviewUntilLeave();
    RequestTrackedDockWindowClose(window);
    RefreshDockRunningWindows();
}

void DesktopApp::CloseDockApplicationWindows(
    const DockAppIdentity& identity)
{
    DismissDockWindowPreviewUntilLeave();
    const std::vector<DockWindowPreviewItem> windows =
        CollectDockWindowPreviewItems(
            identity, true, false);
    for (const DockWindowPreviewItem& item : windows)
        RequestTrackedDockWindowClose(item.window);
    RefreshDockRunningWindows();
}

bool DesktopApp::ResolveDockWindowPreviewTarget(
    POINT clientPoint, DockWindowPreviewTarget& target)
{
    target = {};

    DockContainer* dock = GetDockContainerAtPoint(clientPoint);
    if (!dock || !dock->ContainsInteractivePoint(clientPoint))
        return false;
    target.floatingLayer =
        IsDockContainerEffectivelyFloating(dock);

    RECT anchor{};
    bool found = false;
    if (DockRunningItem* running = dock->RunningItemAtPoint(clientPoint))
    {
        const size_t index = running->GetRunningIndex();
        if (index < dockUnpinnedRunningApps_.size())
        {
            const DockRunningAppInfo& app =
                dockUnpinnedRunningApps_[index];
            target.identity.executablePath = app.executablePath;
            target.identity.appUserModelId = app.appUserModelId;
            target.identity.kind =
                !target.identity.appUserModelId.empty()
                ? DockAppIdentityKind::Applications
                : DockAppIdentityKind::Executable;
            anchor = running->GetBounds();
            found = true;
        }
    }
    else if (DockEntryItem* entry = dock->EntryAtPoint(clientPoint))
    {
        const size_t entryIndex = entry->GetEntryIndex();
        if (entryIndex < dockEntries_.size() &&
            dockEntries_[entryIndex].type ==
                DockEntryType::DesktopItem)
        {
            const size_t itemIndex = FindItemIndexByKey(
                dockEntries_[entryIndex].reference);
            if (itemIndex < items_.size() &&
                GetDockWindowVisualState(itemIndex) !=
                    DockWindowVisualState::Closed)
            {
                target.identity = ResolveDockAppIdentity(itemIndex);
                anchor = entry->GetBounds();
                found = target.identity.kind !=
                    DockAppIdentityKind::None;
            }
        }
    }
    else if (DockFrequentItem* frequent =
        dock->FrequentItemAtPoint(clientPoint))
    {
        const size_t itemIndex = frequent->GetItemIndex();
        if (itemIndex < items_.size() &&
            GetDockWindowVisualState(itemIndex) !=
                DockWindowVisualState::Closed)
        {
            target.identity = ResolveDockAppIdentity(itemIndex);
            anchor = frequent->GetBounds();
            found = target.identity.kind !=
                DockAppIdentityKind::None;
        }
    }

    if (!found)
        return false;
    anchor = dock->GetElementVisualRect(anchor, clientPoint);
    target.identityKey =
        DockWindowPreviewIdentityKey(target.identity);
    if (target.identityKey.empty())
        return false;

    target.anchorScreen = anchor;
    MapWindowPoints(hwnd_, nullptr,
        reinterpret_cast<POINT*>(&target.anchorScreen), 2);
    target.targetToken = DockWindowPreviewTargetToken(
        target.identityKey);
    return !target.targetToken.empty();
}

void DesktopApp::UpdateDockWindowPreview(POINT clientPoint)
{
    if (!dockWindowPreview_)
        return;
    if (!generalSettings_.dockEnabled || dragSession_.IsActive())
    {
        HideDockWindowPreview();
        return;
    }

    DockWindowPreviewTarget target;
    const bool hasTarget =
        ResolveDockWindowPreviewTarget(clientPoint, target);
    const bool previewVisible =
        dockWindowPreview_->IsVisible();
    const bool previewMatchesTarget =
        hasTarget && dockWindowPreviewKey_ == target.targetToken;
    // The target token covers only the app identity. Dock magnification
    // keeps shifting the icon's visual rect while the preview is open, so a
    // matching identity with a moved anchor must follow in place instead of
    // tearing the preview down and re-showing it.
    if (previewMatchesTarget && previewVisible)
    {
        const bool anchorChanged =
            EqualRect(&dockWindowPreviewAnchorScreen_,
                &target.anchorScreen) == FALSE;
        if (snowdesktop::dock_window_rules::
                ShouldFollowDockPreviewAnchor(
                    previewVisible, previewMatchesTarget,
                    anchorChanged))
        {
            dockWindowPreview_->UpdateAnchor(
                target.anchorScreen, dockSettings_.position);
            dockWindowPreviewAnchorScreen_ =
                target.anchorScreen;
        }
    }
    const DockPreviewHoverTransition transition =
        dockWindowPreviewHover_.UpdateTarget(
            hasTarget ? target.targetToken : std::wstring{},
            previewVisible, previewMatchesTarget);

    if (transition.cancelTimer && hwnd_)
        KillTimer(hwnd_, kDockWindowPreviewHoverTimerId);
    if (transition.keepPreviewVisible)
        dockWindowPreview_->KeepVisible();
    if (transition.schedulePreviewHide)
        dockWindowPreview_->ScheduleHide();
    if (transition.armTimer && hwnd_)
    {
        SetTimer(hwnd_, kDockWindowPreviewHoverTimerId,
            QueryDockWindowPreviewHoverTime(), nullptr);
    }
}

void DesktopApp::OnDockWindowPreviewHoverTimer()
{
    if (!hwnd_ || !dockWindowPreview_)
        return;
    KillTimer(hwnd_, kDockWindowPreviewHoverTimerId);

    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    POINT cursorClient = cursorScreen;
    ScreenToClient(hwnd_, &cursorClient);

    DockWindowPreviewTarget target;
    const bool hasTarget =
        generalSettings_.dockEnabled &&
        !dragSession_.IsActive() &&
        ResolveDockWindowPreviewTarget(cursorClient, target);
    const std::wstring observedToken =
        hasTarget ? target.targetToken : std::wstring{};
    if (!dockWindowPreviewHover_.ConsumeTimer(observedToken))
    {
        UpdateDockWindowPreview(cursorClient);
        return;
    }

    std::vector<DockWindowPreviewItem> previewItems =
        CollectDockWindowPreviewItems(target.identity);
    if (previewItems.empty())
        return;

    if (!target.floatingLayer)
    {
        const POINT anchorCenter{
            (target.anchorScreen.left +
                target.anchorScreen.right) / 2,
            (target.anchorScreen.top +
                target.anchorScreen.bottom) / 2
        };
        if (EnsureFloatingDockVisibleForAssociatedSurface(
                anchorCenter))
        {
            DockWindowPreviewTarget floatingTarget;
            if (ResolveDockWindowPreviewTarget(
                    cursorClient, floatingTarget) &&
                floatingTarget.targetToken == observedToken)
            {
                target = std::move(floatingTarget);
            }
        }
    }
    dockWindowPreviewKey_ = target.targetToken;
    dockWindowPreviewAnchorScreen_ = target.anchorScreen;
    dockWindowPreview_->Show(
        previewItems, target.anchorScreen, dockSettings_.position,
        IsLightContentTheme(),
        target.floatingLayer
            ? floatingDockHwnd_ : nullptr);
    if (dockWindowPreview_->IsVisible())
        dockWindowPreviewHover_.MarkPreviewShown(
            target.targetToken);
}

void DesktopApp::HideDockWindowPreview()
{
    const bool previewCleared =
        !dockWindowPreview_ ||
        dockWindowPreview_->IsCleared();
    if (previewCleared &&
        dockWindowPreviewHover_.IsIdle() &&
        dockWindowPreviewKey_.empty() &&
        IsRectEmpty(&dockWindowPreviewAnchorScreen_))
        return;

    if (hwnd_)
        KillTimer(hwnd_, kDockWindowPreviewHoverTimerId);
    if (dockWindowPreview_)
        dockWindowPreview_->Hide();
    dockWindowPreviewHover_.Reset();
    dockWindowPreviewKey_.clear();
    dockWindowPreviewAnchorScreen_ = {};
}

void DesktopApp::DismissDockWindowPreviewUntilLeave()
{
    if (hwnd_)
        KillTimer(hwnd_, kDockWindowPreviewHoverTimerId);
    dockWindowPreviewHover_.SuppressForActivation();
    if (dockWindowPreview_)
        dockWindowPreview_->Hide();
    dockWindowPreviewKey_.clear();
    dockWindowPreviewAnchorScreen_ = {};
}

std::wstring DockItemWindowKey(const DesktopItem& item)
{
    return ToUpperInvariant(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
}

void CALLBACK DesktopApp::DockForegroundWinEventProc(HWINEVENTHOOK,
    DWORD event, HWND window, LONG objectId, LONG childId, DWORD, DWORD)
{
    if (event == EVENT_SYSTEM_FOREGROUND && window)
    {
        const HWND previous = dockForegroundWindow_.exchange(window);
        if (previous != window)
        {
            dockPreviousForegroundWindow_.store(previous);
            dockForegroundChangedTick_.store(GetTickCount());
            if (const HWND target =
                    dockForegroundNotificationWindow_.load())
            {
                PostMessageW(
                    target,
                    kForegroundInteractionChangedMessage,
                    0,
                    0);
            }
        }
    }

    if (event >= EVENT_OBJECT_CREATE &&
        (objectId != OBJID_WINDOW || childId != CHILDID_SELF))
        return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE)
    {
        if (!window || GetAncestor(window, GA_ROOT) != window)
            return;
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (!processId || processId == GetCurrentProcessId())
            return;
        wchar_t className[96]{};
        GetClassNameW(window, className,
            static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"Progman") == 0 ||
            _wcsicmp(className, L"WorkerW") == 0 ||
            _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
            _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
            return;
        const LONG_PTR exStyle =
            GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) != 0 ||
            (GetWindow(window, GW_OWNER) &&
                (exStyle & WS_EX_APPWINDOW) == 0))
            return;
        if (!IsWindowVisible(window) || IsIconic(window))
            return;

        const SystemTaskbarWindowObservation observation{
            MonitorFromWindow(window, MONITOR_DEFAULTTONULL),
            IsZoomed(window) != FALSE
        };
        std::scoped_lock lock(
            systemTaskbarWindowObservationMutex_);
        const auto found =
            systemTaskbarWindowObservations_.find(window);
        if (found !=
                systemTaskbarWindowObservations_.end() &&
            found->second.monitor == observation.monitor &&
            found->second.maximized == observation.maximized)
            return;
        systemTaskbarWindowObservations_[window] = observation;
    }
    systemTaskbarWindowStateChangedTick_.fetch_add(1,
        std::memory_order_relaxed);
    bool dockWindowListChanged =
        event == EVENT_SYSTEM_FOREGROUND ||
        event == EVENT_SYSTEM_MINIMIZESTART ||
        event == EVENT_SYSTEM_MINIMIZEEND ||
        (event >= EVENT_OBJECT_CREATE &&
            event <= EVENT_OBJECT_HIDE);
#ifdef EVENT_OBJECT_CLOAKED
    dockWindowListChanged = dockWindowListChanged ||
        event == EVENT_OBJECT_CLOAKED ||
        event == EVENT_OBJECT_UNCLOAKED;
#endif
    if (dockWindowListChanged)
        dockWindowListChangedTick_.fetch_add(
            1, std::memory_order_relaxed);
}
