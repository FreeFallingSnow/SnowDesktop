#include "app/app.h"
#include "dock_platform_helpers.h"
#include "dock_taskbar_diagnostics.h"
#include "app/shell/initial_icon_bitmap.h"
#include "drag_drop/drag_input_rules.h"
#include "settings/animation_settings.h"
#include "diagnostics/performance_trace.h"

// Running-window discovery, visual state and activation behavior.

namespace
{

constexpr UINT kDockWindowActivationObservationIntervalMs = 100;

bool IsDockConsentActivationProxy(HWND window)
{
    if (!window || !IsWindow(window)) return false;
    wchar_t className[128]{};
    return GetClassNameW(window, className, static_cast<int>(std::size(className))) &&
        wcscmp(className, L"$$$Secure UAP Dummy Window Class For Interim Dialog") == 0;
}

void LogDockWindowActivation(HWND target, const wchar_t* phase,
    int showCommand = 0, BOOL accepted = TRUE, DWORD error = ERROR_SUCCESS)
{
    DWORD process = 0, foregroundProcess = 0;
    const DWORD thread = GetWindowThreadProcessId(target, &process);
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground
        ? GetWindowThreadProcessId(foreground, &foregroundProcess) : 0;
    WINDOWPLACEMENT placement{sizeof(placement)};
    const BOOL placementKnown = GetWindowPlacement(target, &placement);
    wchar_t message[768]{};
    swprintf_s(message,
        L"DockActivation tick=%llu phase=%ls target=%p pid=%lu tid=%lu "
        L"foreground=%p foregroundPid=%lu foregroundTid=%lu "
        L"iconic=%d visible=%d hung=%d placementKnown=%d placement=%u flags=%u "
        L"show=%d accepted=%d error=%lu",
        GetTickCount64(), phase, target, process, thread,
        foreground, foregroundProcess, foregroundThread,
        IsIconic(target), IsWindowVisible(target), IsHungAppWindow(target),
        placementKnown, placement.showCmd, placement.flags, showCommand, accepted, error);
    WriteDiagnosticLogEntry(message, DiagnosticLogLevel::Debug);
}

HWND ResolveDockWindowActivationTarget(HWND target)
{
    if (!target || !IsWindow(target))
        return nullptr;
    HWND popup = GetLastActivePopup(target);
    const bool popupValid = popup && IsWindow(popup);
    if (!snowdesktop::dock_window_rules::
            IsDockWindowActivationPopupEligible(
                popupValid,
                popupValid && IsWindowVisible(popup),
                popupValid && IsIconic(popup),
                popupValid &&
                    (GetWindowLongPtrW(
                        popup, GWL_EXSTYLE) &
                        WS_EX_NOACTIVATE) != 0))
    {
        return target;
    }
    return popup;
}

bool IsDockWindowActivationForeground(
    HWND target, HWND activationTarget)
{
    const HWND foreground = GetForegroundWindow();
    return DockWindowsShareActivationGroup(
            target, foreground) ||
        DockWindowsShareActivationGroup(
            activationTarget, foreground);
}

bool ActivateDockWindowForeground(HWND target, HWND activationTarget)
{
    if (!target || !IsWindow(target) ||
        !activationTarget || !IsWindow(activationTarget))
        return false;
    return snowdesktop::dock_window_rules::
        ApplyDockWindowForegroundActivation(
            [target, activationTarget]() {
                return IsDockWindowActivationForeground(
                    target, activationTarget);
            },
            [activationTarget]() {
                SetForegroundWindow(activationTarget);
            });
}

void RequestDockWindowShow(HWND target, bool wasMinimized)
{
    if (!target || !IsWindow(target))
        return;
    const int showCommand = wasMinimized ? DockRestoreShowCommand(target) : SW_SHOW;
    LogDockWindowActivation(target, L"show-request", showCommand);
    SetLastError(ERROR_SUCCESS);
    const BOOL showAccepted = ShowWindowAsync(target, showCommand);
    const DWORD showError = showAccepted ? ERROR_SUCCESS : GetLastError();
    LogDockWindowActivation(target, L"show-result", showCommand, showAccepted, showError);
    const bool restoreFallbackRequired =
        snowdesktop::dock_window_rules::
            NeedsDockRestoreRequestFallback(
                wasMinimized,
                showAccepted != FALSE) &&
        !ShouldSkipSynchronousWindowActivation(target);
    snowdesktop::dock_window_rules::
        ApplyDockRestoreRequestFallback(
            restoreFallbackRequired,
            [target](WPARAM systemCommand) {
                // Elevated windows reject ShowWindowAsync through UIPI.
                // Unlike posting WM_SYSCOMMAND, the default window procedure
                // remains usable from a normal-integrity Dock process.
                LogDockWindowActivation(target, L"before-default-restore");
                DefWindowProcW(
                    target, WM_SYSCOMMAND,
                    systemCommand, 0);
                LogDockWindowActivation(target, L"after-default-restore");
            },
            [target]() {
                return IsIconic(target) != FALSE;
            },
            [target]() {
                LogDockWindowActivation(target, L"before-switch-fallback");
                SwitchToThisWindow(target, FALSE);
                LogDockWindowActivation(target, L"after-switch-fallback");
            });
}

} // namespace

DesktopApp::DockWindowActivationOutcome
DesktopApp::ActivateDockWindowAfterShow(
    HWND target, bool wasMinimized)
{
    DockWindowActivationOutcome outcome;
    if (!target || !IsWindow(target))
        return outcome;
    outcome.restored = !wasMinimized ||
        IsIconic(target) == FALSE;
    if (!outcome.restored || !IsWindow(target))
        return outcome;

    const HWND activationTarget =
        ResolveDockWindowActivationTarget(target);
    outcome.responsive =
        activationTarget &&
        snowdesktop::dock_window_rules::
            IsDockWindowActivationResponsive(
                !ShouldSkipSynchronousWindowActivation(target),
                !ShouldSkipSynchronousWindowActivation(
                    activationTarget));
    if (snowdesktop::dock_window_rules::
            ShouldSwitchDockWindowAfterShow(
                wasMinimized, outcome.restored))
    {
        outcome.foreground = ActivateDockWindowForeground(target, activationTarget);
    }
    return outcome;
}

DesktopApp::DockWindowActivationOutcome
DesktopApp::RequestDockWindowActivation(
    HWND target, bool wasMinimized)
{
    CancelAllDockWindowActivationObservations();
    if (IsDockConsentActivationProxy(target))
    {
        // Restoring this normal-desktop proxy through DefWindowProc only
        // foregrounds an empty window. The task-switch request lets Windows
        // reveal its consent UI. Issue it once for the exact clicked window,
        // outside the UI thread; never treat the proxy's foreground as proof
        // that the secure-desktop dialog was shown.
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(target, &process);
        const HWND requestForeground = GetForegroundWindow();
        const ULONGLONG queuedAt = GetTickCount64();
        const std::wstring key = L"dock-consent-activate:" + std::to_wstring(process) + L":" +
            std::to_wstring(thread) + L":" + std::to_wstring(reinterpret_cast<UINT_PTR>(target));
        const bool queued = shellModelWork_.Submit(key,
            [target, process, thread, requestForeground, queuedAt] {
                DWORD currentProcess = 0;
                const DWORD currentThread = GetWindowThreadProcessId(target, &currentProcess);
                if (!process || !thread || currentProcess != process || currentThread != thread ||
                    !IsDockConsentActivationProxy(target) || !IsWindowVisible(target) ||
                    IsHungAppWindow(target) || GetTickCount64() - queuedAt > 1000 ||
                    GetForegroundWindow() != requestForeground)
                    return false;
                LogDockWindowActivation(target, L"consent-task-switch-request");
                SwitchToThisWindow(target, TRUE);
                LogDockWindowActivation(target, L"consent-task-switch-returned");
                return true;
            }, [](bool) {}, hwnd_, kBackgroundShellReadyMessage);
        LogDockWindowActivation(target, queued ? L"consent-task-switch-queued" : L"consent-task-switch-not-queued");
        DockWindowActivationOutcome outcome;
        outcome.restored = !wasMinimized;
        return outcome;
    }
    BeginDockWindowActivationObservation(
        target, wasMinimized);
    RequestDockWindowShow(target, wasMinimized);
    const DockWindowActivationOutcome outcome =
        ActivateDockWindowAfterShow(target, wasMinimized);
    UpdateDockWindowActivationObservation(target, outcome);
    return outcome;
}

void DesktopApp::BeginDockWindowActivationObservation(
    HWND target, bool awaitingRestore)
{
    if (!target || !IsWindow(target))
        return;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(target, &process);
    if (!process || !thread)
        return;
    dockWindowActivationObservations_[target].Begin(
        awaitingRestore, GetTickCount64(), process, thread);
    if (dockWindowActivationObservationToken_)
        return;
    dockWindowActivationObservationToken_ =
        uiAnimationScheduler_.ScheduleInterval(
            kDockWindowActivationObservationIntervalMs,
            [this](snowdesktop::UiScheduleToken token) {
                OnDockWindowActivationObservationTimer(token);
            });
    if (!dockWindowActivationObservationToken_)
        dockWindowActivationObservations_.clear();
}

void DesktopApp::UpdateDockWindowActivationObservation(
    HWND target,
    const DockWindowActivationOutcome& outcome)
{
    const auto found =
        dockWindowActivationObservations_.find(target);
    if (found == dockWindowActivationObservations_.end() ||
        !outcome.restored)
    {
        return;
    }
    if (outcome.foreground ||
        !outcome.responsive)
    {
        LogDockWindowActivation(target,
            outcome.foreground ? L"foreground-observed" : L"unresponsive");
        CancelDockWindowActivationObservation(target);
        return;
    }
    found->second.Restored(GetTickCount64());
}

void DesktopApp::CancelDockWindowActivationObservation(
    HWND target)
{
    if (target)
    {
        HWND root = IsWindow(target)
            ? GetAncestor(target, GA_ROOT) : nullptr;
        dockWindowActivationObservations_.erase(
            root ? root : target);
    }
    if (!dockWindowActivationObservations_.empty() ||
        !dockWindowActivationObservationToken_)
    {
        return;
    }
    const auto token =
        dockWindowActivationObservationToken_;
    dockWindowActivationObservationToken_ = 0;
    uiAnimationScheduler_.Cancel(token);
}

void DesktopApp::CancelAllDockWindowActivationObservations()
{
    dockWindowActivationObservations_.clear();
    if (!dockWindowActivationObservationToken_)
        return;
    const auto token =
        dockWindowActivationObservationToken_;
    dockWindowActivationObservationToken_ = 0;
    uiAnimationScheduler_.Cancel(token);
}

void DesktopApp::OnDockWindowActivationObservationTimer(
    snowdesktop::UiScheduleToken token)
{
    if (!token ||
        token != dockWindowActivationObservationToken_)
    {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    std::vector<HWND> targets;
    targets.reserve(dockWindowActivationObservations_.size());
    for (const auto& [target, observation] :
         dockWindowActivationObservations_)
    {
        (void)observation;
        targets.push_back(target);
    }

    for (HWND target : targets)
    {
        const auto found =
            dockWindowActivationObservations_.find(target);
        if (found == dockWindowActivationObservations_.end())
            continue;
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(target, &process);
        const bool valid = target && IsWindow(target) &&
            found->second.Matches(process, thread);
        const bool foreground = valid &&
            IsDockWindowActivationForeground(target, target);
        const bool rootWindowSafe = valid &&
            !ShouldSkipSynchronousWindowActivation(target);
        const bool retryExpired = found->second.Expired(now);
        const auto action =
            snowdesktop::dock_window_rules::
                ResolveDockWindowActivationObservationAction(
                    valid,
                    valid && IsDockWindowClosePending(target),
                    rootWindowSafe,
                    found->second.awaitingRestore,
                    valid && IsIconic(target),
                    foreground,
                    retryExpired);
        if (action == snowdesktop::dock_window_rules::
                DockWindowActivationObservationAction::Stop)
        {
            LogDockWindowActivation(target, retryExpired ? L"observation-timeout" :
                foreground ? L"foreground-observed" :
                !valid ? L"target-changed" : L"observation-cancelled");
            CancelDockWindowActivationObservation(target);
            continue;
        }
        if (action == snowdesktop::dock_window_rules::
                DockWindowActivationObservationAction::WaitForRestore)
        {
            continue;
        }

        const DockWindowActivationOutcome outcome =
            ActivateDockWindowAfterShow(target, true);
        UpdateDockWindowActivationState(
            target, outcome.restored, outcome.foreground);
        UpdateDockWindowActivationObservation(target, outcome);
    }
}

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

void DesktopApp::RefreshDockForegroundState()
{
    if (!generalSettings_.dockEnabled)
        return;
    const HWND foreground = ResolveDockSemanticForegroundWindow();
    const int primaryButton = GetSystemMetrics(SM_SWAPBUTTON)
        ? VK_RBUTTON : VK_LBUTTON;
    if (snowdesktop::dock_window_rules::ShouldDeferDockForegroundFeedback(
            IsDesktopInteractionSurfaceWindow(foreground),
            (GetAsyncKeyState(primaryButton) & 0x8000) != 0))
        return;

    PruneDockPendingCloseWindows();
    const auto matchesForeground = [this](HWND window, HWND active) {
        return window && IsWindow(window) && IsWindowVisible(window) &&
            !IsIconic(window) &&
            !dockPendingCloseWindows_.contains(GetAncestor(window, GA_ROOT)) &&
            DockWindowsShareActivationGroup(window, active);
    };
    const auto isMinimized = [](HWND window) {
        return window && IsWindow(window) && IsIconic(window) != FALSE;
    };
    bool changed = false;
    for (auto& [key, state] : dockRunningWindows_)
    {
        (void)key;
        changed |= snowdesktop::dock_window_rules::
            RefreshTrackedDockForegroundState(
                state, foreground, matchesForeground, isMinimized);
    }
    for (DockRunningAppInfo& app : dockUnpinnedRunningApps_)
    {
        changed |= snowdesktop::dock_window_rules::
            RefreshTrackedDockForegroundState(
                app, foreground, matchesForeground, isMinimized);
    }
    // This path only mutates window state: retained drag wrappers, item order,
    // identities and bitmaps remain owned by the periodic discovery model.
    if (changed)
        InvalidateDockRects();
}

void DesktopApp::HandleDockWindowListChanged(HWND window, DWORD event)
{
    snowdesktop::performance::Scope performanceScope("dock", "running.notification");
    if (!window) return;
    const auto tracks = [window](const auto& state) {
        return state.window == window ||
            std::find(state.trackedWindows.begin(), state.trackedWindows.end(), window) !=
                state.trackedWindows.end();
    };
    const bool tracked = std::any_of(dockRunningWindows_.begin(), dockRunningWindows_.end(),
            [&](const auto& pair) { return tracks(pair.second); }) ||
        std::any_of(dockUnpinnedRunningApps_.begin(), dockUnpinnedRunningApps_.end(), tracks);
    // Focus/minimize feedback already has a cheap tracked-state path. A new
    // foreground window still gets immediate discovery if its show was missed.
    const bool stateOnly = event == EVENT_SYSTEM_FOREGROUND ||
        event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_SYSTEM_MINIMIZEEND;
    if (tracked && stateOnly)
    {
        return;
    }
    if (!tracked && !IsDockTaskWindow(window)) return;

    if (tracked && (event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW))
    {
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        const auto identity = dockWindowAppIds_.PeekValue(window,
            std::to_wstring(process) + L":" + std::to_wstring(thread));
        // Repeated show events for an already identified task do not change
        // its membership. Unknown metadata can still receive a targeted retry.
        if (identity && !identity->empty())
        {
            RefreshDockForegroundState();
            return;
        }
    }
    dockWindowAppIds_.Invalidate(window);
    dockWindowListChangedTick_.fetch_add(1, std::memory_order_relaxed);
    RefreshDockRunningWindows();
}

void DesktopApp::RefreshDockRunningWindows(
    bool invalidateChanged, HWND preferredWindow)
{
    // Dock slots own the DockEntryItem/DockRunningItem wrappers retained by a
    // DragSession. The OLE nested loop continues to dispatch maintenance
    // timers after capture and mouseDown_ are cleared, so rebuilding the
    // running-app model here would invalidate those source pointers before
    // native hand-back or synchronous drop completion.
    if (mouseDown_ || snowdesktop::drag_input_rules::ShouldDeferModelReload(
            dragSession_.HasContext(),
            dragDropController_.IsTransportActive()))
    {
        return;
    }
    snowdesktop::performance::Scope performanceScope("dock", "running.discovery");
    PruneDockPendingCloseWindows();
    const DWORD observedWindowStateTick = dockWindowListChangedTick_.load();
    dockWindowAppIds_.Retain([](HWND window) { return IsWindow(window) != FALSE; });
    struct DockWindowTarget
    {
        std::wstring key;
        DockAppIdentity identity;
        DockWindowInfo best;
        int score = -1;
        std::vector<HWND> trackedWindows;
    };
    struct RunningWindowCandidate
    {
        std::wstring identityKey;
        std::wstring title;
        std::wstring executablePath;
        std::wstring appUserModelId;
        std::vector<std::wstring>
            ancestorExecutablePaths;
        HWND window = nullptr;
        bool minimized = false;
        bool foreground = false;
        int score = -1;
        std::vector<HWND> trackedWindows;
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
    bool fixedIdentityPending = false;
    for (const DockEntry& entry : dockEntries_)
    {
        if (entry.type != DockEntryType::DesktopItem) continue;
        const size_t itemIndex = FindItemIndexByKey(entry.reference);
        if (itemIndex >= items_.size()) continue;
        bool pending = false;
        DockAppIdentity identity = ResolveDockAppIdentity(itemIndex, &pending);
        fixedIdentityPending |= pending;
        if (identity.kind != DockAppIdentityKind::None)
            fixedIdentities.push_back(std::move(identity));
    }
    std::vector<RunningWindowCandidate> runningCandidates;
    std::unordered_map<std::wstring, size_t> runningCandidateIndices;
    const bool hasApplicationPins = std::any_of(fixedIdentities.begin(), fixedIdentities.end(),
        [](const auto& identity) { return identity.kind == DockAppIdentityKind::Applications; });

    const HWND preferredRoot = preferredWindow && IsWindow(preferredWindow)
        ? GetAncestor(preferredWindow, GA_ROOT) : nullptr;
    const HWND actualForeground =
        ResolveDockSemanticForegroundWindow();
    const HWND scoringForeground = preferredRoot ? preferredRoot : actualForeground;
    struct EnumContext
    {
        DesktopApp* owner;
        std::vector<DockWindowTarget>* targets;
        HWND scoringForeground;
        HWND actualForeground;
        const std::vector<DockAppIdentity>* fixedIdentities;
        std::vector<RunningWindowCandidate>* runningCandidates;
        std::unordered_map<std::wstring, size_t>* runningCandidateIndices;
        const std::unordered_map<HWND, ULONGLONG>*
            pendingCloseWindows;
        const DockProcessParentMap* processParents;
        std::unordered_map<DWORD, std::wstring> processPaths;
        std::unordered_map<DWORD,
            std::vector<std::wstring>> processAncestors;
        bool fixedIdentityPending = false;
        bool hasApplicationPins = false;
        std::unordered_map<DWORD, std::wstring> processAppIds;
    };
    const DockProcessParentMap processParents =
        generalSettings_.dockEnabled
        ? QueryDockProcessParentMap()
        : DockProcessParentMap{};
    EnumContext context{ this, &targets, scoringForeground, actualForeground, &fixedIdentities,
        &runningCandidates, &runningCandidateIndices,
        &dockPendingCloseWindows_, &processParents };
    context.fixedIdentityPending = fixedIdentityPending;
    context.hasApplicationPins = hasApplicationPins;

    if (generalSettings_.dockEnabled)
    {
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto* context = reinterpret_cast<EnumContext*>(parameter);
            if (!IsDockTaskWindow(window) ||
                (context->pendingCloseWindows &&
                 context->pendingCloseWindows->contains(window)))
                return TRUE;

            DWORD processId = 0;
            const DWORD threadId = GetWindowThreadProcessId(window, &processId);
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
            {
                return TRUE;
            }
            auto [pathIt, inserted] = context->processPaths.try_emplace(processId);
            if (inserted)
                pathIt->second = QueryDockWindowExecutablePath(window);
            auto [ancestorIt, ancestorsInserted] =
                context->processAncestors.try_emplace(
                    processId);
            if (ancestorsInserted &&
                context->processParents)
            {
                ancestorIt->second =
                    QueryDockProcessAncestorExecutablePaths(
                        processId,
                        *context->processParents);
            }
            std::wstring appUserModelId = context->owner->GetDockWindowAppUserModelIdAsync(window);
            if (appUserModelId.empty() &&
                (context->hasApplicationPins || context->fixedIdentityPending))
            {
                // Package identity is available directly from the process;
                // do not wait for the Shell window property query to pin it.
                auto [idIt, newProcess] = context->processAppIds.try_emplace(processId);
                if (newProcess) idIt->second = QueryDockProcessAppUserModelId(processId);
                appUserModelId = idIt->second;
            }

            int score = DockWindowsShareActivationGroup(
                window, context->scoringForeground) ? 1000 : 0;
            score += 100;
            if (!IsIconic(window)) score += 20;
            if (!GetWindow(window, GW_OWNER)) score += 10;

            for (DockWindowTarget& target : *context->targets)
            {
                const bool identityMatches =
                    snowdesktop::dock_app_identity_rules::
                        MatchesRunningApp(
                            target.identity.kind,
                            target.identity.executablePath,
                            target.identity.appUserModelId,
                            target.identity.steamInstallDirectory,
                            pathIt->second,
                            appUserModelId,
                            ancestorIt->second);
                if (!identityMatches)
                    continue;
                target.trackedWindows.push_back(window);
                if (score <= target.score)
                    continue;
                target.best = { window, IsIconic(window) != FALSE, true,
                    DockWindowsShareActivationGroup(window, context->actualForeground) };
                target.score = score;
            }

            bool fixed = false;
            for (const DockAppIdentity& identity : *context->fixedIdentities)
            {
                fixed = snowdesktop::
                    dock_app_identity_rules::
                        MatchesRunningApp(
                            identity.kind,
                            identity.executablePath,
                            identity.appUserModelId,
                            identity.steamInstallDirectory,
                            pathIt->second,
                            appUserModelId,
                            ancestorIt->second);
                if (fixed) break;
            }
            if (fixed) return TRUE;

            // The first running presentation must not wait for a Shell query.
            // Executable identity is usable now; refine its ID/artwork later
            // without restarting the same window's entrance animation.
            const std::wstring identityKey = snowdesktop::dock_app_identity_rules::RunningWindowIdentity(
                pathIt->second, appUserModelId, reinterpret_cast<UINT_PTR>(window), processId, threadId);
            if (identityKey.empty()) return TRUE;
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
                    pathIt->second, appUserModelId,
                    ancestorIt->second,
                    window, IsIconic(window) != FALSE,
                    DockWindowsShareActivationGroup(window, context->actualForeground), score,
                    { window } });
            }
            else
            {
                RunningWindowCandidate& candidate =
                    (*context->runningCandidates)[candidateIt->second];
                candidate.trackedWindows.push_back(window);
                if (score > candidate.score)
                {
                    candidate.title = std::move(title);
                    candidate.ancestorExecutablePaths =
                        ancestorIt->second;
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
    for (DockWindowTarget& target : targets)
    {
        if (target.best.window)
        {
            target.best.trackedWindows = std::move(target.trackedWindows);
            updated[target.key] = std::move(target.best);
        }
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
            size_t index = found != runningCandidateIndices.end() ? found->second : runningCandidates.size();
            if (index == runningCandidates.size())
                for (size_t candidateIndex = 0; candidateIndex < runningCandidates.size(); ++candidateIndex)
                {
                    const auto& candidate = runningCandidates[candidateIndex];
                    if (!consumed[candidateIndex] && candidate.executablePath == existing.executablePath &&
                        std::find(existing.trackedWindows.begin(), existing.trackedWindows.end(), candidate.window) !=
                            existing.trackedWindows.end())
                    {
                        index = candidateIndex;
                        break;
                    }
                }
            if (index >= runningCandidates.size() || consumed[index]) continue;
            consumed[index] = true;
            stableCandidates.push_back(std::move(runningCandidates[index]));
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
    const int requiredIconSize = GetMaximumShellIconBitmapSize();
    const double animationNow = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    const bool animatePresence = dockRunningAppsInitialized_ &&
        snowdesktop::animation::RuntimeAnimationsEnabled();
    std::vector<bool> reused(dockUnpinnedRunningApps_.size(), false);
    for (RunningWindowCandidate& candidate : runningCandidates)
    {
        DockRunningAppInfo info;
        info.identityKey = std::move(candidate.identityKey);
        info.title = std::move(candidate.title);
        info.executablePath = std::move(candidate.executablePath);
        info.appUserModelId = std::move(candidate.appUserModelId);
        info.ancestorExecutablePaths =
            std::move(candidate.ancestorExecutablePaths);
        info.window = candidate.window;
        info.minimized = candidate.minimized;
        info.foreground = candidate.foreground;
        info.trackedWindows = std::move(candidate.trackedWindows);
        bool refineIdentityArtwork = false;
        for (size_t i = 0; i < dockUnpinnedRunningApps_.size(); ++i)
        {
            DockRunningAppInfo& old = dockUnpinnedRunningApps_[i];
            const bool sameWindow = (old.executablePath == info.executablePath ||
                old.executablePath.empty() || info.executablePath.empty()) &&
                std::find(old.trackedWindows.begin(), old.trackedWindows.end(), info.window) !=
                    old.trackedWindows.end();
            if (reused[i] || (old.identityKey != info.identityKey && !sameWindow)) continue;
            info.presence = old.presence;
            refineIdentityArtwork = (old.appUserModelId != info.appUserModelId &&
                !info.appUserModelId.empty()) ||
                (old.executablePath.empty() && !info.executablePath.empty());
            if (old.iconBitmap &&
                (old.iconRequestedSize >= requiredIconSize ||
                 snowdesktop::icon_render_rules::
                    SourceLongEdgeCoversTarget(
                        old.iconBitmapSize.cx,
                        old.iconBitmapSize.cy,
                        requiredIconSize)))
            {
                info.iconBitmap = old.iconBitmap;
                info.iconBitmapSize = old.iconBitmapSize;
                info.iconRequestedSize =
                    old.iconRequestedSize;
                old.iconBitmap = nullptr;
            }
            info.selected = old.selected;
            reused[i] = true;
            break;
        }
        info.presence.SetVisible(true, animationNow, animatePresence,
            snowdesktop::animation::RuntimeDurationScale());
        if (!info.iconBitmap || refineIdentityArtwork)
        {
            const int requestSize = std::max(requiredIconSize, info.iconRequestedSize);
            info.iconRequestedSize = requestSize;
            const auto key = info.identityKey;
            const auto path = info.executablePath;
            const auto appId = info.appUserModelId;
            const auto window = info.window;
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            const auto queuedAt = GetTickCount64();
            dockIconWork_.Submit(L"dock-running:" + key + L"\n" +
                std::to_wstring(requestSize), [path, appId, window, process, requestSize, queuedAt] {
                const auto started = GetTickCount64();
                auto result = std::make_shared<snowdesktop::BackgroundBitmap>();
                result->bitmap = snowdesktop::initial_icon_bitmap::ReadRunning(!appId.empty(),
                    [&] { return GetLocalIconResourceBitmap(path, result->size, requestSize); },
                    [&]() -> HBITMAP {
                        DWORD current = 0;
                        GetWindowThreadProcessId(window, &current);
                        if (!process || current != process) return nullptr;
                        return CreateDockWindowProvidedIconBitmap(window, result->size, requestSize);
                    });
                WriteDiagnosticLogEntry((L"Dock local running icon: queueMs=" + std::to_wstring(started - queuedAt) +
                    L" readMs=" + std::to_wstring(GetTickCount64() - started) + L" bitmap=" +
                    std::to_wstring(result->bitmap ? 1 : 0) + L" path=" + path).c_str());
                return result;
            }, [window, process, path, appId, requestSize, queuedAt] {
                const auto started = GetTickCount64();
                auto result = std::make_shared<snowdesktop::BackgroundBitmap>();
                DWORD current = 0;
                GetWindowThreadProcessId(window, &current);
                if (current == process)
                    result->bitmap = CreateDockWindowIconBitmap(window, path, appId,
                        result->size, requestSize);
                WriteDiagnosticLogEntry((L"Dock Shell running icon: queueMs=" + std::to_wstring(started - queuedAt) +
                    L" readMs=" + std::to_wstring(GetTickCount64() - started) + L" bitmap=" +
                    std::to_wstring(result->bitmap ? 1 : 0) + L" path=" + path).c_str());
                return result;
            }, [this, key, path, appId, requestSize](auto result, bool refined) {
                if (!result || !result->bitmap) return;
                for (auto& app : dockUnpinnedRunningApps_)
                {
                    if (!app.presence.Visible()) continue;
                    if (app.identityKey != key || app.executablePath != path ||
                        app.appUserModelId != appId || app.iconRequestedSize > requestSize) continue;
                    // A slow local completion must not replace a refined icon.
                    if (!refined && app.iconBitmap) return;
                    if (app.iconBitmap) { EraseD2DIconCacheForBitmap(app.iconBitmap); DeleteObject(app.iconBitmap); }
                    app.iconBitmap = std::exchange(result->bitmap, nullptr);
                    app.iconBitmapSize = result->size;
                    app.iconRequestedSize = requestSize;
                    InvalidateDragStaticScene();
                    InvalidateDockRects();
                    break;
                }
            }, hwnd_, kBackgroundShellReadyMessage);
        }
        runningApps.push_back(std::move(info));
    }

    // Keep an exiting presentation at its old position until its axis share is
    // zero. It owns the bitmap but no longer owns an actionable window.
    for (size_t i = 0; i < dockUnpinnedRunningApps_.size(); ++i)
    {
        if (reused[i]) continue;
        auto& old = dockUnpinnedRunningApps_[i];
        DockRunningAppInfo exiting = old;
        exiting.presence.SetVisible(false, animationNow, animatePresence,
            snowdesktop::animation::RuntimeDurationScale());
        if (exiting.presence.IsHidden()) continue;
        old.iconBitmap = nullptr;
        exiting.window = nullptr;
        exiting.trackedWindows.clear();
        exiting.foreground = exiting.selected = false;
        runningApps.insert(runningApps.begin() + static_cast<std::ptrdiff_t>(
            std::min(i, runningApps.size())), std::move(exiting));
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
            if (old.presence.Amount() != current.presence.Amount())
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
    dockRunningAppsInitialized_ = true;
    EnsureUiAnimationFrame();

    if (runningLayoutChanged)
    {
        InvalidateDockContainers();
        InvalidateDragStaticScene();
        // Startup discovers running apps after the persistent hosts are laid
        // out. Refresh their HWND regions before repainting the wider Dock;
        // painting alone updates the glass but retains the old content clip.
        // This also applies to reloads that suppress desktop invalidation.
        UpdateFloatingDockWindowBounds(false);
    }

    if ((changed || runningVisualChanged) && invalidateChanged && hwnd_)
    {
        InvalidateRect(hwnd_, nullptr, runningLayoutChanged ? TRUE : FALSE);
    }
    dockRunningWindowsForegroundTick_ =
        dockForegroundChangedTick_.load();
    dockRunningWindowsStateTick_ = observedWindowStateTick;
    dockRunningWindowsRefreshTick_ = GetTickCount();
    UpdateDockExternalMinimizeTargets();
}

bool DesktopApp::ActivateOrToggleDockItem(
    size_t itemIndex,
    std::optional<snowdesktop::dock_window_rules::DockClickAction>
        pressedAction,
    HWND pressedTarget,
    std::optional<RECT> pressedAnchorScreen,
    DockWindowTransitionCapturePolicy minimizeCapturePolicy)
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
    const HWND transitionKeepBelowWindow =
        floatingDockHost_ &&
            IsPersistentDockHostEffectivelyFloating(
                *floatingDockHost_) &&
            floatingDockHwnd_ &&
            IsWindowVisible(floatingDockHwnd_)
        ? floatingDockHwnd_ : nullptr;

    // The action comes from the indicator under the pointer at button-down.
    // Do not infer it again from GetForegroundWindow() during button-up.
    if (action == DockClickAction::Minimize)
    {
        const bool reverseRestore =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Restore;
        const bool shouldMinimize =
            !IsIconic(target) || reverseRestore;
        if (shouldMinimize)
            PrepareDockWindowMinimize(target, L"dock-minimize");
        bool transitionStarted = false;
        bool nativeFallbackRequested = false;
        if (shouldMinimize &&
            dockWindowTransition_ &&
            pressedAnchorScreen)
        {
            transitionStarted =
                dockWindowTransition_->StartMinimize(
                    target, *pressedAnchorScreen,
                    minimizeCapturePolicy,
                    transitionKeepBelowWindow);
            nativeFallbackRequested = dockWindowTransition_->RequiresNativeAnimationFallback();
        }
        if (shouldMinimize && snowdesktop::dock_window_rules::ShouldAbortDockMinimizeOnAnimationFailure(
                minimizeCapturePolicy == DockWindowTransitionCapturePolicy::LiveThumbnailOnly,
                transitionStarted,
                nativeFallbackRequested))
        {
            return false;
        }
        if (shouldMinimize)
        {
            CancelDockWindowActivationObservation(target);
            snowdesktop::dock_taskbar_diagnostics::Record(L"before-native-minimize", target);
            snowdesktop::dock_window_rules::DockWindowMinimizeRequestRoute minimizeRoute{};
            const bool minimizeAccepted = RequestDockWindowMinimize(target, &minimizeRoute);
            wchar_t minimizeDiagnostic[128]{};
            swprintf_s(minimizeDiagnostic, L"native-minimize accepted=%d route=%ls",
                minimizeAccepted ? 1 : 0,
                snowdesktop::dock_window_rules::DockWindowMinimizeRequestRouteName(minimizeRoute));
            snowdesktop::dock_taskbar_diagnostics::Record(
                minimizeDiagnostic, target);
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
                [this](
                    HWND restoreTarget,
                    DockWindowRestoreTransitionPhase phase) {
                    HandleDockWindowRestoreTransition(
                        restoreTarget, phase);
                },
                transitionKeepBelowWindow))
        {
            InvalidateDockRects();
            return true;
        }
        const DockWindowActivationOutcome outcome =
            RequestDockWindowActivation(target, minimized);
        found->second.minimized = !outcome.restored;
        found->second.foreground = outcome.foreground;
    }

    InvalidateDockRects();
    return true;
}

bool DesktopApp::ActivateOrToggleDockWindow(
    HWND window,
    std::optional<snowdesktop::dock_window_rules::DockClickAction>
        pressedAction,
    HWND pressedTarget,
    std::optional<RECT> pressedAnchorScreen,
    DockWindowTransitionCapturePolicy minimizeCapturePolicy)
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
    if (IsDockConsentActivationProxy(target))
    {
        RequestDockWindowActivation(target, IsIconic(target) != FALSE);
        return true;
    }
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
        const HWND foreground =
            ResolveDockSemanticForegroundWindow();
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
    const HWND transitionKeepBelowWindow =
        floatingDockHost_ &&
            IsPersistentDockHostEffectivelyFloating(
                *floatingDockHost_) &&
            floatingDockHwnd_ &&
            IsWindowVisible(floatingDockHwnd_)
        ? floatingDockHwnd_ : nullptr;
    bool nowMinimized = false;
    bool nowForeground = false;
    if (action == DockClickAction::Minimize)
    {
        const bool reverseRestore =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Restore;
        const bool shouldMinimize =
            !minimized || reverseRestore;
        if (shouldMinimize)
            PrepareDockWindowMinimize(target, L"dock-minimize");
        bool transitionStarted = false;
        bool nativeFallbackRequested = false;
        if (shouldMinimize &&
            dockWindowTransition_ &&
            pressedAnchorScreen)
        {
            transitionStarted =
                dockWindowTransition_->StartMinimize(
                    target, *pressedAnchorScreen,
                    minimizeCapturePolicy,
                    transitionKeepBelowWindow);
            nativeFallbackRequested = dockWindowTransition_->RequiresNativeAnimationFallback();
        }
        if (shouldMinimize && snowdesktop::dock_window_rules::ShouldAbortDockMinimizeOnAnimationFailure(
                minimizeCapturePolicy == DockWindowTransitionCapturePolicy::LiveThumbnailOnly,
                transitionStarted,
                nativeFallbackRequested))
        {
            return false;
        }
        if (shouldMinimize)
        {
            CancelDockWindowActivationObservation(target);
            snowdesktop::dock_taskbar_diagnostics::Record(L"before-native-minimize", target);
            snowdesktop::dock_window_rules::DockWindowMinimizeRequestRoute minimizeRoute{};
            const bool minimizeAccepted = RequestDockWindowMinimize(target, &minimizeRoute);
            wchar_t minimizeDiagnostic[128]{};
            swprintf_s(minimizeDiagnostic, L"native-minimize accepted=%d route=%ls",
                minimizeAccepted ? 1 : 0,
                snowdesktop::dock_window_rules::DockWindowMinimizeRequestRouteName(minimizeRoute));
            snowdesktop::dock_taskbar_diagnostics::Record(
                minimizeDiagnostic, target);
        }
        nowMinimized = true;
    }
    else
    {
        const bool reverseMinimize =
            transitionActiveForTarget &&
            activeTransitionDirection ==
                DockWindowTransitionDirection::Minimize;
        const bool animateRestore =
            snowdesktop::dock_window_rules::
                ShouldAnimateDockWindowRestore(
                    minimized || reverseMinimize,
                    dockWindowTransition_ != nullptr,
                    pressedAnchorScreen.has_value());
        if (action == DockClickAction::Restore &&
            animateRestore &&
            dockWindowTransition_->StartRestore(
                target, *pressedAnchorScreen,
                [this](
                    HWND restoreTarget,
                    DockWindowRestoreTransitionPhase phase) {
                    HandleDockWindowRestoreTransition(
                        restoreTarget, phase);
                },
                transitionKeepBelowWindow))
        {
            InvalidateDockRects();
            return true;
        }
        const DockWindowActivationOutcome outcome =
            RequestDockWindowActivation(target, minimized);
        nowMinimized = !outcome.restored;
        nowForeground = outcome.foreground;
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
    if (IsDockTaskbarDocumentProxyCandidate(window))
    {
        // Registered MDI/TDI tab proxies are hidden by design. Activating the
        // proxy asks its owner application to select and reveal the matching
        // document; the ordinary path would incorrectly show the 0x0 helper
        // window before trying to foreground it.
        ActivateDockWindowFromPreview(window);
        return;
    }
    // Reuse the exact Dock-icon command path: restoring plays the icon-to-
    // window transition, activating moves the window to the foreground and
    // clicking a foreground window minimizes it back into the Dock. The
    // preview keeps the icon anchor fresh while visible; read it before the
    // dismissal inside the command path clears it.
    //
    // Unlike a Dock icon, a preview card targets one concrete window, so the
    // action must be resolved per-window: a background card of a multi-window
    // app must activate its window instead of being read as app-foreground
    // and minimized.
    HWND target = GetAncestor(window, GA_ROOT);
    if (!target)
        target = window;
    const bool windowForeground =
        ResolveDockSemanticForegroundWindow() == target;
    const auto action =
        snowdesktop::dock_window_rules::
            ResolveDockWindowPreviewClickAction(
                IsIconic(target) != FALSE,
                windowForeground);
    // Mirror the Dock-icon command path: minimize first tries a target-only
    // DWM thumbnail while keeping the floating layer visible, and closes it
    // only when that path is unavailable. Restore and plain foreground
    // activation deliberately keep it visible. Closing the host dismisses
    // the preview and clears the stored anchor, so read the anchor first.
    const RECT anchor = dockWindowPreviewAnchorScreen_;
    std::function<bool(DockWindowTransitionCapturePolicy)> command =
        [this, window, action, anchor](
            DockWindowTransitionCapturePolicy capturePolicy) {
            if (!window || !IsWindow(window))
                return false;
            if (!IsRectEmpty(&anchor) &&
                ActivateOrToggleDockWindow(
                    window, action, nullptr, anchor,
                    capturePolicy))
                return true;
            // Propagate an isolated live attempt's failure so the caller can
            // retry after removing the floating layer. Native activation here
            // used to consume that signal and could invert a minimize click.
            if (capturePolicy ==
                DockWindowTransitionCapturePolicy::LiveThumbnailOnly)
                return false;
            ActivateDockWindowFromPreview(window);
            return true;
        };
    const bool requiresFloatingDockClose =
        snowdesktop::dock_window_rules::
            RequiresFloatingDockMinimizeCaptureIsolation(
                IsSelectedPersistentDockHostPromoted(),
                action);
    if (requiresFloatingDockClose &&
        command(DockWindowTransitionCapturePolicy::
            LiveThumbnailOnly))
    {
        return;
    }
    if (requiresFloatingDockClose)
    {
        CloseFloatingDockThen(
            [command = std::move(command)]() mutable {
                command(DockWindowTransitionCapturePolicy::
                    SnapshotPreferred);
            },
            FloatingDockCloseFocusPolicy::PreserveCurrent);
        return;
    }
    command(DockWindowTransitionCapturePolicy::
        SnapshotPreferred);
}

void DesktopApp::ActivateDockWindowFromPreview(HWND window)
{
    if (!window || !IsWindow(window))
        return;
    HWND target = GetAncestor(window, GA_ROOT);
    if (!target)
        target = window;
    const bool restoring = IsIconic(target) != FALSE;
    DismissDockWindowPreviewUntilLeave();
    if (snowdesktop::dock_window_rules::
            ShouldSuppressDockWindowCommand(
                IsDockWindowClosePending(window)))
        return;

    if (IsDockTaskbarDocumentProxyCandidate(target))
    {
        SetForegroundWindow(target);
        return;
    }

    const bool minimized = restoring;
    const DockWindowActivationOutcome outcome =
        RequestDockWindowActivation(target, minimized);
    UpdateDockWindowActivationState(
        target, outcome.restored, outcome.foreground);
}

void DesktopApp::HandleDockWindowRestoreTransition(
    HWND window,
    DockWindowRestoreTransitionPhase phase)
{
    if (!window || !IsWindow(window))
        return;
    HWND target = GetAncestor(window, GA_ROOT);
    if (!target)
        target = window;

    if (phase == DockWindowRestoreTransitionPhase::
            FallbackWithoutAnimation)
    {
        ActivateDockWindowFromPreview(target);
        return;
    }
    if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
    {
        if (!snowdesktop::dock_window_rules::
                ShouldSuppressDockWindowCommand(
                    IsDockWindowClosePending(target)))
        {
            // Only submit the maximized/normal restore here. The transition
            // keeps its final snapshot visible while the target processes the
            // request, so the UI thread never waits inside the animation end
            // frame and no second restore can alter the placement.
            const DockWindowActivationOutcome outcome =
                RequestDockWindowActivation(target, true);
            UpdateDockWindowActivationState(
                target, outcome.restored,
                outcome.foreground);
        }
        return;
    }

    const DockWindowActivationOutcome outcome =
        ActivateDockWindowAfterShow(target, true);
    UpdateDockWindowActivationState(
        target, outcome.restored, outcome.foreground);
    UpdateDockWindowActivationObservation(target, outcome);
}

void DesktopApp::UpdateDockWindowActivationState(
    HWND target, bool restored, bool foreground)
{
    if (!target || !IsWindow(target))
        return;
    for (auto& [key, state] : dockRunningWindows_)
    {
        (void)key;
        if (!state.window || !IsWindow(state.window))
            continue;
        const bool matchesTarget =
            state.window == target ||
            DockWindowsShareApplicationIdentity(
                state.window, target);
        if (foreground)
            state.foreground = matchesTarget;
        else if (matchesTarget)
            state.foreground = false;
        if (matchesTarget)
        {
            state.window = target;
            state.running = true;
            state.minimized = !restored;
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
        if (foreground)
            app.foreground = matchesTarget;
        else if (matchesTarget)
            app.foreground = false;
        if (matchesTarget)
        {
            app.window = target;
            app.minimized = !restored;
        }
    }
    InvalidateDockRects();
}

std::wstring DesktopApp::GetDockWindowAppUserModelIdAsync(HWND window, bool* pending)
{
    if (pending) *pending = false;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (!process || !thread) return {};
    const auto version = std::to_wstring(process) + L":" + std::to_wstring(thread);
    const auto request = dockWindowAppIds_.ReadOrSubmit(window, version,
        [this, window, process, thread, version](std::uint64_t ticket) {
            shellVisualWork_.Submit(L"window-appid:" + std::to_wstring(reinterpret_cast<UINT_PTR>(window)) +
            L":" + version + L":" + std::to_wstring(ticket), [window, process, thread] {
                DWORD current = 0;
                const DWORD currentThread = GetWindowThreadProcessId(window, &current);
                return current == process && currentThread == thread
                    ? QueryDockWindowAppUserModelId(window) : std::wstring{};
            }, [this, window, process, thread, ticket](std::wstring id) {
                DWORD current = 0;
                const DWORD currentThread = GetWindowThreadProcessId(window, &current);
                if (current != process || currentThread != thread) return;
                const bool firstCompletion = !dockWindowAppIds_.PeekValue(
                    window, std::to_wstring(process) + L":" + std::to_wstring(thread)).has_value();
                const auto lifetime = id.empty() ? std::chrono::seconds(5)
                    : std::chrono::seconds(30);
                const auto changed = dockWindowAppIds_.PublishWithLifetimeChanged(
                    window, ticket, std::move(id), lifetime);
                // Identical completions must not invalidate the ten-second
                // fallback clock and form an enumerate/query/enumerate loop.
                if (changed && (*changed || firstCompletion)) dockRunningWindowsRefreshTick_ = 0;
            }, hwnd_, kBackgroundShellReadyMessage);
        });
    if (pending) *pending = !request.sameSourceVersion && !request.fresh;
    return request.sameSourceVersion ? request.value.value_or(L"") : std::wstring{};
}

bool DesktopApp::AdvanceDockRunningAnimations(double nowMilliseconds)
{
    snowdesktop::performance::Scope performanceScope("dock", "running.animation");
    // Item wrappers may also be retained between button down and the drag
    // threshold. Rebuilding animated slots in that interval invalidates them.
    if (mouseDown_ || snowdesktop::drag_input_rules::ShouldDeferModelReload(
            dragSession_.HasContext(), dragDropController_.IsTransportActive()))
        return true;

    RECT desktopDirty{};
    for (const auto& container : containers_)
        if (auto* dock = dynamic_cast<DockContainer*>(container.get());
            dock && !FindPersistentDockHost(dock))
        {
            RECT bounds = snowdesktop::floating_dock_rules::ExpandHostForTitleLayer(
                dock->GetAnimationVisualBounds(), dockSettings_.position);
            UnionRect(&desktopDirty, &desktopDirty, &bounds);
        }
    bool keep = false;
    for (auto& app : dockUnpinnedRunningApps_)
    {
        if (snowdesktop::animation::RuntimeAnimationsEnabled())
            app.presence.Advance(nowMilliseconds);
        else
            app.presence.SetVisible(app.presence.Visible(), nowMilliseconds, false);
        keep |= app.presence.IsAnimating();
        if (app.presence.IsHidden() && app.iconBitmap)
        {
            EraseD2DIconCacheForBitmap(app.iconBitmap);
            DeleteObject(app.iconBitmap);
            app.iconBitmap = nullptr;
        }
    }
    std::erase_if(dockUnpinnedRunningApps_, [](const auto& app) { return app.presence.IsHidden(); });
    InvalidateDockContainers();
    InvalidateDragStaticScene();
    for (const auto& container : containers_)
        if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
        {
            if (FindPersistentDockHost(dock))
                RequestDockAnimationPresentation(*dock);
            else
            {
                RECT bounds = snowdesktop::floating_dock_rules::ExpandHostForTitleLayer(
                    dock->GetAnimationVisualBounds(), dockSettings_.position);
                UnionRect(&desktopDirty, &desktopDirty, &bounds);
            }
        }
    if (!IsRectEmpty(&desktopDirty) && hwnd_ && IsWindow(hwnd_))
    {
        InflateRect(&desktopDirty, 4, 4);
        RequestDesktopDockAnimationPresentation(desktopDirty);
    }
    return keep;
}
