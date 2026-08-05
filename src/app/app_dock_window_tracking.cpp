#include "app.h"
#include "dock_platform_helpers.h"

// Running-window discovery, visual state and activation behavior.

DockWindowVisualState DesktopApp::GetDockWindowVisualState(size_t itemIndex) const
{
    if (itemIndex >= items_.size()) return DockWindowVisualState::Closed;
    const auto found = dockRunningWindows_.find(DockItemWindowKey(items_[itemIndex]));
    if (found == dockRunningWindows_.end() || !found->second.running)
        return DockWindowVisualState::Closed;
    if (found->second.window && IsWindow(found->second.window))
    {
        if (IsIconic(found->second.window))
            return DockWindowVisualState::Minimized;
        // A click on a non-maximized window can move foreground ownership to
        // the desktop/Dock before this handler runs. Keep using the indicator
        // state captured by the window refresh instead of reclassifying that
        // click as Activate.
        if (found->second.foreground)
            return DockWindowVisualState::Foreground;
    }
    return DockWindowVisualState::Running;
}

void DesktopApp::RefreshDockRunningWindows(
    bool invalidateChanged, HWND preferredWindow)
{
    PruneDockPendingCloseWindows();
    struct DockWindowTarget
    {
        std::wstring key;
        DockAppIdentity identity;
        DockWindowInfo best;
        int score = -1;
    };
    struct RunningWindowCandidate
    {
        std::wstring identityKey;
        std::wstring title;
        std::wstring executablePath;
        std::wstring appUserModelId;
        HWND window = nullptr;
        bool minimized = false;
        bool foreground = false;
        int score = -1;
    };

    std::unordered_set<size_t> itemIndices;
    for (const DockEntry& entry : dockEntries_)
    {
        if (entry.type != DockEntryType::DesktopItem) continue;
        const size_t itemIndex = FindItemIndexByKey(entry.reference);
        if (itemIndex < items_.size()) itemIndices.insert(itemIndex);
    }
    for (const size_t itemIndex : GetFrequentDockItemIndices())
        if (itemIndex < items_.size()) itemIndices.insert(itemIndex);

    std::vector<DockWindowTarget> targets;
    targets.reserve(itemIndices.size());
    for (const size_t itemIndex : itemIndices)
    {
        DockAppIdentity identity = ResolveDockAppIdentity(itemIndex);
        if (identity.kind == DockAppIdentityKind::None)
            continue;
        targets.push_back({ DockItemWindowKey(items_[itemIndex]), std::move(identity) });
    }

    std::vector<DockAppIdentity> fixedIdentities;
    for (const DockEntry& entry : dockEntries_)
    {
        if (entry.type != DockEntryType::DesktopItem) continue;
        const size_t itemIndex = FindItemIndexByKey(entry.reference);
        if (itemIndex >= items_.size()) continue;
        DockAppIdentity identity = ResolveDockAppIdentity(itemIndex);
        if (identity.kind != DockAppIdentityKind::None)
            fixedIdentities.push_back(std::move(identity));
    }
    std::vector<RunningWindowCandidate> runningCandidates;
    std::unordered_map<std::wstring, size_t> runningCandidateIndices;

    const HWND preferredRoot = preferredWindow && IsWindow(preferredWindow)
        ? GetAncestor(preferredWindow, GA_ROOT) : nullptr;
    const HWND actualForeground = GetAncestor(GetForegroundWindow(), GA_ROOT);
    const HWND scoringForeground = preferredRoot ? preferredRoot : actualForeground;
    struct EnumContext
    {
        std::vector<DockWindowTarget>* targets;
        HWND scoringForeground;
        HWND actualForeground;
        const std::vector<DockAppIdentity>* fixedIdentities;
        std::vector<RunningWindowCandidate>* runningCandidates;
        std::unordered_map<std::wstring, size_t>* runningCandidateIndices;
        const std::unordered_map<HWND, ULONGLONG>*
            pendingCloseWindows;
        std::unordered_map<DWORD, std::wstring> processPaths;
    } context{ &targets, scoringForeground, actualForeground, &fixedIdentities,
        &runningCandidates, &runningCandidateIndices,
        &dockPendingCloseWindows_ };

    if (generalSettings_.dockEnabled)
    {
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto* context = reinterpret_cast<EnumContext*>(parameter);
            if (!IsDockTaskWindow(window) ||
                (context->pendingCloseWindows &&
                 context->pendingCloseWindows->contains(window)))
                return TRUE;

            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (!processId || processId == GetCurrentProcessId()) return TRUE;
            auto [pathIt, inserted] = context->processPaths.try_emplace(processId);
            if (inserted)
                pathIt->second = QueryDockWindowExecutablePath(window);
            const std::wstring appUserModelId = QueryDockWindowAppUserModelId(window);

            DWORD cloaked = 0;
            const bool isCloaked = SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED,
                &cloaked, sizeof(cloaked))) && cloaked != 0;
            int score = DockWindowsShareActivationGroup(
                window, context->scoringForeground) ? 1000 : 0;
            if (!isCloaked) score += 100;
            if (!IsIconic(window)) score += 20;
            if (!GetWindow(window, GW_OWNER)) score += 10;

            for (DockWindowTarget& target : *context->targets)
            {
                const bool executableMatches = !target.identity.executablePath.empty() &&
                    pathIt->second == target.identity.executablePath;
                const bool appIdMatches = !target.identity.appUserModelId.empty() &&
                    appUserModelId == target.identity.appUserModelId;
                const bool steamPathMatches =
                    target.identity.kind == DockAppIdentityKind::Steam &&
                    IsDockPathInsideDirectory(pathIt->second,
                        target.identity.steamInstallDirectory);
                bool identityMatches = false;
                switch (target.identity.kind)
                {
                case DockAppIdentityKind::Executable:
                    identityMatches = executableMatches;
                    break;
                case DockAppIdentityKind::Applications:
                    identityMatches = appIdMatches;
                    break;
                case DockAppIdentityKind::Steam:
                    identityMatches = appIdMatches || steamPathMatches;
                    break;
                default:
                    break;
                }
                if (!identityMatches || score <= target.score)
                    continue;
                target.best = { window, IsIconic(window) != FALSE, true,
                    DockWindowsShareActivationGroup(window, context->actualForeground) };
                target.score = score;
            }

            if (isCloaked || pathIt->second.empty()) return TRUE;
            bool fixed = false;
            for (const DockAppIdentity& identity : *context->fixedIdentities)
            {
                const bool executableMatches = !identity.executablePath.empty() &&
                    pathIt->second == identity.executablePath;
                const bool appIdMatches = !identity.appUserModelId.empty() &&
                    appUserModelId == identity.appUserModelId;
                const bool steamPathMatches = identity.kind == DockAppIdentityKind::Steam &&
                    IsDockPathInsideDirectory(pathIt->second,
                        identity.steamInstallDirectory);
                fixed = identity.kind == DockAppIdentityKind::Executable
                    ? executableMatches
                    : (identity.kind == DockAppIdentityKind::Applications
                        ? appIdMatches
                        : (identity.kind == DockAppIdentityKind::Steam &&
                            (appIdMatches || steamPathMatches)));
                if (fixed) break;
            }
            if (fixed) return TRUE;

            const std::wstring identityKey = !appUserModelId.empty()
                ? L"AUMID:" + appUserModelId : L"EXE:" + pathIt->second;
            wchar_t titleBuffer[512]{};
            GetWindowTextW(window, titleBuffer, static_cast<int>(std::size(titleBuffer)));
            std::wstring title = titleBuffer;
            if (title.empty())
                title = PathFindFileNameW(pathIt->second.c_str());

            auto [candidateIt, candidateInserted] =
                context->runningCandidateIndices->try_emplace(
                    identityKey, context->runningCandidates->size());
            if (candidateInserted)
            {
                context->runningCandidates->push_back({ identityKey, std::move(title),
                    pathIt->second, appUserModelId, window, IsIconic(window) != FALSE,
                    DockWindowsShareActivationGroup(window, context->actualForeground), score });
            }
            else
            {
                RunningWindowCandidate& candidate =
                    (*context->runningCandidates)[candidateIt->second];
                if (score > candidate.score)
                {
                    candidate.title = std::move(title);
                    candidate.window = window;
                    candidate.minimized = IsIconic(window) != FALSE;
                    candidate.foreground = DockWindowsShareActivationGroup(
                        window, context->actualForeground);
                    candidate.score = score;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
    }

    std::unordered_map<std::wstring, DockWindowInfo> updated;
    for (const DockWindowTarget& target : targets)
    {
        if (target.best.window)
            updated[target.key] = target.best;
        else if (target.identity.kind == DockAppIdentityKind::Steam &&
            IsDockSteamAppRunning(target.identity.steamAppId))
            updated[target.key] = { nullptr, false, true, false };
    }

    bool changed = updated.size() != dockRunningWindows_.size();
    if (!changed)
    {
        for (const auto& [key, state] : updated)
        {
            const auto old = dockRunningWindows_.find(key);
            if (old == dockRunningWindows_.end() || old->second.window != state.window ||
                old->second.minimized != state.minimized ||
                old->second.running != state.running ||
                old->second.foreground != state.foreground)
            {
                changed = true;
                break;
            }
        }
    }
    dockRunningWindows_ = std::move(updated);

    // EnumWindows does not promise a stable order. Keep surviving applications
    // in their existing Dock positions and append only genuinely new ones.
    if (runningCandidates.size() > 1 && !dockUnpinnedRunningApps_.empty())
    {
        std::vector<RunningWindowCandidate> stableCandidates;
        stableCandidates.reserve(runningCandidates.size());
        std::vector<bool> consumed(runningCandidates.size(), false);
        for (const DockRunningAppInfo& existing : dockUnpinnedRunningApps_)
        {
            const auto found = runningCandidateIndices.find(existing.identityKey);
            if (found == runningCandidateIndices.end() ||
                found->second >= runningCandidates.size() || consumed[found->second])
                continue;
            consumed[found->second] = true;
            stableCandidates.push_back(std::move(runningCandidates[found->second]));
        }
        for (size_t i = 0; i < runningCandidates.size(); ++i)
        {
            if (!consumed[i])
                stableCandidates.push_back(std::move(runningCandidates[i]));
        }
        runningCandidates = std::move(stableCandidates);
    }

    std::vector<DockRunningAppInfo> runningApps;
    runningApps.reserve(runningCandidates.size());
    std::vector<bool> reused(dockUnpinnedRunningApps_.size(), false);
    for (RunningWindowCandidate& candidate : runningCandidates)
    {
        DockRunningAppInfo info;
        info.identityKey = std::move(candidate.identityKey);
        info.title = std::move(candidate.title);
        info.executablePath = std::move(candidate.executablePath);
        info.appUserModelId = std::move(candidate.appUserModelId);
        info.window = candidate.window;
        info.minimized = candidate.minimized;
        info.foreground = candidate.foreground;
        for (size_t i = 0; i < dockUnpinnedRunningApps_.size(); ++i)
        {
            DockRunningAppInfo& old = dockUnpinnedRunningApps_[i];
            if (reused[i] || old.identityKey != info.identityKey) continue;
            info.iconBitmap = old.iconBitmap;
            info.iconBitmapSize = old.iconBitmapSize;
            info.selected = old.selected;
            old.iconBitmap = nullptr;
            reused[i] = true;
            break;
        }
        if (!info.iconBitmap)
            info.iconBitmap = CreateDockWindowIconBitmap(
                info.window, info.executablePath, info.appUserModelId,
                info.iconBitmapSize);
        runningApps.push_back(std::move(info));
    }

    bool runningLayoutChanged = runningApps.size() != dockUnpinnedRunningApps_.size();
    bool runningVisualChanged = runningLayoutChanged;
    if (!runningLayoutChanged)
    {
        for (size_t i = 0; i < runningApps.size(); ++i)
        {
            const DockRunningAppInfo& old = dockUnpinnedRunningApps_[i];
            const DockRunningAppInfo& current = runningApps[i];
            if (old.identityKey != current.identityKey)
                runningLayoutChanged = true;
            if (old.window != current.window || old.minimized != current.minimized ||
                old.foreground != current.foreground || old.title != current.title)
                runningVisualChanged = true;
        }
    }
    for (DockRunningAppInfo& old : dockUnpinnedRunningApps_)
    {
        if (!old.iconBitmap) continue;
        EraseD2DIconCacheForBitmap(old.iconBitmap);
        DeleteObject(old.iconBitmap);
        old.iconBitmap = nullptr;
    }
    dockUnpinnedRunningApps_ = std::move(runningApps);

    if (runningLayoutChanged)
    {
        InvalidateDockContainers();
        InvalidateDragStaticScene();
    }

    if ((changed || runningVisualChanged) && invalidateChanged && hwnd_)
    {
        InvalidateRect(hwnd_, nullptr, runningLayoutChanged ? TRUE : FALSE);
    }
    dockRunningWindowsForegroundTick_ =
        dockForegroundChangedTick_.load();
    dockRunningWindowsStateTick_ =
        dockWindowListChangedTick_.load();
    dockRunningWindowsRefreshTick_ = GetTickCount();
}

bool DesktopApp::ActivateOrToggleDockItem(
    size_t itemIndex,
    std::optional<snowdesktop::dock_window_rules::DockClickAction>
        pressedAction,
    HWND pressedTarget,
    std::optional<RECT> pressedAnchorScreen)
{
    DismissDockWindowPreviewUntilLeave();
    if (itemIndex >= items_.size()) return false;
    const DockAppIdentity requestedIdentity =
        ResolveDockAppIdentity(itemIndex);
    if (snowdesktop::dock_window_rules::
            ShouldSuppressDockWindowCommand(
                IsDockAppClosePending(requestedIdentity)))
        return true;

    using snowdesktop::dock_window_rules::DockClickAction;
    DockClickAction action =
        pressedAction.value_or(DockClickAction::None);
    if (action == DockClickAction::Launch)
        return LaunchDesktopItem(itemIndex);

    HWND preferredWindow = nullptr;
    if (pressedTarget && IsWindow(pressedTarget))
    {
        preferredWindow = GetAncestor(pressedTarget, GA_ROOT);
        if (!preferredWindow)
            preferredWindow = pressedTarget;
    }
    if (!preferredWindow)
    {
        const DockAppIdentity identity =
            ResolveDockAppIdentity(itemIndex);
        if (identity.kind == DockAppIdentityKind::None)
            return LaunchDesktopItem(itemIndex);
    }

    const std::wstring key = DockItemWindowKey(items_[itemIndex]);
    auto found = dockRunningWindows_.find(key);
    if (preferredWindow)
    {
        if (found == dockRunningWindows_.end())
        {
            found = dockRunningWindows_.emplace(
                key, DockWindowInfo{
                    preferredWindow,
                    IsIconic(preferredWindow) != FALSE,
                    true,
                    false }).first;
        }
        else
        {
            found->second.window = preferredWindow;
            found->second.running = true;
        }
    }
    else if (found == dockRunningWindows_.end() ||
        !IsWindow(found->second.window))
    {
        // Button-down already records the exact window for normal Dock
        // clicks. Only fall back to the expensive all-window scan when that
        // cached target is genuinely unavailable or stale.
        RefreshDockRunningWindows(false);
        found = dockRunningWindows_.find(key);
    }
    if (found == dockRunningWindows_.end() || !IsWindow(found->second.window))
    {
        if (action == DockClickAction::Minimize)
            return false;
        return LaunchDesktopItem(itemIndex);
    }

    HWND target = preferredWindow
        ? preferredWindow : found->second.window;
    if (dockWindowTransition_ &&
        dockWindowTransition_->IsActive() &&
        !dockWindowTransition_->IsActiveFor(
            target))
    {
        dockWindowTransition_->Cancel();
    }
    found->second.window = target;
    found->second.minimized = IsIconic(target) != FALSE;
    if (action == DockClickAction::None)
    {
        action =
            snowdesktop::dock_window_rules::ResolveDockClickAction(
                found->second.running,
                found->second.minimized,
                found->second.foreground);
    }
    const bool transitionActiveForTarget =
        dockWindowTransition_ &&
        dockWindowTransition_->IsActiveFor(
            target);
    const auto activeTransitionDirection =
        transitionActiveForTarget
        ? dockWindowTransition_->GetDirection()
        : DockWindowTransitionDirection::Minimize;
    if (transitionActiveForTarget)
    {
        action = activeTransitionDirection ==
                DockWindowTransitionDirection::Minimize
            ? DockClickAction::Restore
            : DockClickAction::Minimize;
    }

    // The action comes from the indicator under the pointer at button-down.
    // Do not infer it again from GetForegroundWindow() during button-up.
    if (action == DockClickAction::Minimize)
    {
        const bool reverseRestore =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Restore;
        if ((!IsIconic(target) ||
                reverseRestore) &&
            dockWindowTransition_ &&
            pressedAnchorScreen)
        {
            dockWindowTransition_->StartMinimize(
                target, *pressedAnchorScreen);
        }
        if (!IsIconic(target) ||
            reverseRestore)
        {
            RequestDockWindowMinimize(target);
        }
        found->second.minimized = true;
        found->second.foreground = false;
    }
    else
    {
        const bool minimized = IsIconic(target) != FALSE;
        const bool reverseMinimize =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Minimize;
        if (action == DockClickAction::Restore &&
            (minimized || reverseMinimize) &&
            dockWindowTransition_ &&
            pressedAnchorScreen &&
            dockWindowTransition_->StartRestore(
                target, *pressedAnchorScreen,
                [this](HWND restoreTarget) {
                    ActivateDockWindowFromPreview(
                        restoreTarget);
                }))
        {
            InvalidateDockRects();
            return true;
        }
        BOOL showAccepted = FALSE;
        if (minimized)
        {
            showAccepted = ShowWindowAsync(
                target,
                DockRestoreShowCommand(target));
        }
        else
        {
            showAccepted =
                ShowWindowAsync(target, SW_SHOW);
        }
        HWND activationTarget = GetLastActivePopup(target);
        if (!activationTarget || !IsWindow(activationTarget))
            activationTarget = target;
        if (snowdesktop::dock_window_rules::
                NeedsDockWindowSwitchFallback(
                    minimized,
                    showAccepted != FALSE))
        {
            SwitchToThisWindow(target, TRUE);
            if (activationTarget != target)
                SwitchToThisWindow(
                    activationTarget, TRUE);
        }
        BringWindowToTop(activationTarget);
        SetForegroundWindow(activationTarget);
        found->second.minimized = false;
        found->second.foreground = true;
    }

    InvalidateDockRects();
    return true;
}

bool DesktopApp::ActivateOrToggleDockWindow(
    HWND window,
    std::optional<snowdesktop::dock_window_rules::DockClickAction>
        pressedAction,
    HWND pressedTarget,
    std::optional<RECT> pressedAnchorScreen)
{
    DismissDockWindowPreviewUntilLeave();
    HWND requestedTarget =
        pressedTarget && IsWindow(pressedTarget)
            ? pressedTarget : window;
    if (!requestedTarget || !IsWindow(requestedTarget))
        return false;
    if (snowdesktop::dock_window_rules::
            ShouldSuppressDockWindowCommand(
                IsDockWindowClosePending(requestedTarget)))
        return true;
    HWND target = GetAncestor(requestedTarget, GA_ROOT);
    if (!target) target = requestedTarget;
    if (dockWindowTransition_ &&
        dockWindowTransition_->IsActive() &&
        !dockWindowTransition_->IsActiveFor(
            target))
    {
        dockWindowTransition_->Cancel();
    }

    using snowdesktop::dock_window_rules::DockClickAction;
    const bool minimized = IsIconic(target) != FALSE;
    DockClickAction action =
        pressedAction.value_or(DockClickAction::None);
    if (action == DockClickAction::None)
    {
        const HWND foreground = GetForegroundWindow();
        action =
            snowdesktop::dock_window_rules::ResolveDockClickAction(
                true, minimized,
                DockWindowsShareApplicationIdentity(
                    target, foreground));
    }
    const bool transitionActiveForTarget =
        dockWindowTransition_ &&
        dockWindowTransition_->IsActiveFor(
            target);
    const auto activeTransitionDirection =
        transitionActiveForTarget
        ? dockWindowTransition_->GetDirection()
        : DockWindowTransitionDirection::Minimize;
    if (transitionActiveForTarget)
    {
        action = activeTransitionDirection ==
                DockWindowTransitionDirection::Minimize
            ? DockClickAction::Restore
            : DockClickAction::Minimize;
    }
    bool nowMinimized = false;
    bool nowForeground = false;
    if (action == DockClickAction::Minimize)
    {
        const bool reverseRestore =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Restore;
        if ((!minimized || reverseRestore) &&
            dockWindowTransition_ &&
            pressedAnchorScreen)
        {
            dockWindowTransition_->StartMinimize(
                target, *pressedAnchorScreen);
        }
        if (!minimized ||
            reverseRestore)
        {
            RequestDockWindowMinimize(target);
        }
        nowMinimized = true;
    }
    else
    {
        const bool reverseMinimize =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Minimize;
        if (action == DockClickAction::Restore &&
            (minimized || reverseMinimize) &&
            dockWindowTransition_ &&
            pressedAnchorScreen &&
            dockWindowTransition_->StartRestore(
                target, *pressedAnchorScreen,
                [this](HWND restoreTarget) {
                    ActivateDockWindowFromPreview(
                        restoreTarget);
                }))
        {
            InvalidateDockRects();
            return true;
        }
        BOOL showAccepted = FALSE;
        if (minimized)
        {
            showAccepted = ShowWindowAsync(
                target,
                DockRestoreShowCommand(target));
        }
        else
        {
            showAccepted =
                ShowWindowAsync(target, SW_SHOW);
        }
        HWND activationTarget = GetLastActivePopup(target);
        if (!activationTarget || !IsWindow(activationTarget))
            activationTarget = target;
        if (snowdesktop::dock_window_rules::
                NeedsDockWindowSwitchFallback(
                    minimized,
                    showAccepted != FALSE))
        {
            SwitchToThisWindow(target, TRUE);
            if (activationTarget != target)
                SwitchToThisWindow(
                    activationTarget, TRUE);
        }
        BringWindowToTop(activationTarget);
        SetForegroundWindow(activationTarget);
        nowForeground = true;
    }

    for (DockRunningAppInfo& app : dockUnpinnedRunningApps_)
    {
        const bool matchesTarget =
            DockWindowsShareApplicationIdentity(
                app.window, target);
        if (nowForeground) app.foreground = matchesTarget;
        if (matchesTarget)
        {
            app.minimized = nowMinimized;
            app.foreground = nowForeground;
        }
    }
    InvalidateDockRects();
    return true;
}

void DesktopApp::ActivateDockWindowFromPreviewAnimated(HWND window)
{
    if (!window || !IsWindow(window))
        return;
    if (snowdesktop::dock_window_rules::
            ShouldSuppressDockWindowCommand(
                IsDockWindowClosePending(window)))
        return;
    HWND target = GetAncestor(window, GA_ROOT);
    if (!target)
        target = window;

    // The preview stores the icon anchor at show time and keeps it while
    // hovering. Read it before any dismissal path clears it.
    const RECT anchor = dockWindowPreviewAnchorScreen_;
    const bool minimized = IsIconic(target) != FALSE;
    const bool anchorAvailable = !IsRectEmpty(&anchor);
    if (snowdesktop::dock_window_rules::
            ShouldAnimateDockPreviewRestore(
                minimized,
                dockWindowTransition_ != nullptr,
                anchorAvailable) &&
        dockWindowTransition_->StartRestore(
            target, anchor,
            [this](HWND restoreTarget) {
                ActivateDockWindowFromPreview(
                    restoreTarget);
            }))
    {
        InvalidateDockRects();
        return;
    }
    ActivateDockWindowFromPreview(window);
}

void DesktopApp::ActivateDockWindowFromPreview(HWND window)
{
    DismissDockWindowPreviewUntilLeave();
    if (!window || !IsWindow(window))
        return;
    if (snowdesktop::dock_window_rules::
            ShouldSuppressDockWindowCommand(
                IsDockWindowClosePending(window)))
        return;
    HWND target = GetAncestor(window, GA_ROOT);
    if (!target)
        target = window;

    const bool minimized = IsIconic(target) != FALSE;
    BOOL showAccepted = FALSE;
    if (minimized)
    {
        showAccepted = ShowWindowAsync(
            target,
            DockRestoreShowCommand(target));
    }
    else
    {
        showAccepted =
            ShowWindowAsync(target, SW_SHOW);
    }
    HWND activationTarget = GetLastActivePopup(target);
    if (!activationTarget || !IsWindow(activationTarget))
        activationTarget = target;
    if (snowdesktop::dock_window_rules::
            NeedsDockWindowSwitchFallback(
                minimized,
                showAccepted != FALSE))
    {
        SwitchToThisWindow(target, TRUE);
        if (activationTarget != target)
            SwitchToThisWindow(activationTarget, TRUE);
    }
    if (floatingDockVisible_)
        CloseFloatingDock();
    BringWindowToTop(activationTarget);
    SetForegroundWindow(activationTarget);

    // ShowWindowAsync has not necessarily updated IsIconic yet. Update the
    // known target optimistically instead of doing a synchronous EnumWindows
    // scan that can both block the animation handoff and write the old state
    // straight back into the cache.
    for (auto& [key, state] : dockRunningWindows_)
    {
        (void)key;
        if (!state.window || !IsWindow(state.window))
            continue;
        const bool matchesTarget =
            state.window == target ||
            DockWindowsShareApplicationIdentity(
                state.window, target);
        state.foreground = matchesTarget;
        if (matchesTarget)
        {
            state.window = target;
            state.running = true;
            state.minimized = false;
        }
    }
    for (DockRunningAppInfo& app :
         dockUnpinnedRunningApps_)
    {
        if (!app.window || !IsWindow(app.window))
            continue;
        const bool matchesTarget =
            app.window == target ||
            DockWindowsShareApplicationIdentity(
                app.window, target);
        app.foreground = matchesTarget;
        if (matchesTarget)
        {
            app.window = target;
            app.minimized = false;
        }
    }
    InvalidateDockRects();
}
