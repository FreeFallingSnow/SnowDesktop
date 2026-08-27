#include "app.h"
#include "dock_platform_helpers.h"
#include "../drag_input_rules.h"

// Running-window discovery, visual state and activation behavior.

namespace
{

constexpr UINT kDockWindowActivationObservationIntervalMs = 24;
constexpr ULONGLONG kDockWindowActivationRetryDurationMs = 1000;

class ScopedDockInputQueueAttachment
{
public:
    ScopedDockInputQueueAttachment(
        DWORD firstThread, DWORD secondThread)
        : firstThread_(firstThread),
          secondThread_(secondThread)
    {
        attached_ = firstThread_ != 0 &&
            secondThread_ != 0 &&
            firstThread_ != secondThread_ &&
            AttachThreadInput(
                firstThread_, secondThread_, TRUE) != FALSE;
    }

    ~ScopedDockInputQueueAttachment()
    {
        if (attached_)
        {
            AttachThreadInput(
                firstThread_, secondThread_, FALSE);
        }
    }

    ScopedDockInputQueueAttachment(
        const ScopedDockInputQueueAttachment&) = delete;
    ScopedDockInputQueueAttachment& operator=(
        const ScopedDockInputQueueAttachment&) = delete;

private:
    DWORD firstThread_ = 0;
    DWORD secondThread_ = 0;
    bool attached_ = false;
};

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

bool ActivateDockWindowForeground(
    HWND target, HWND activationTarget,
    bool synchronousActivationSafe)
{
    if (!target || !IsWindow(target) ||
        !activationTarget || !IsWindow(activationTarget))
        return false;
    return snowdesktop::dock_window_rules::
        ApplyDockWindowForegroundActivation(
            synchronousActivationSafe,
            [target, activationTarget]() {
                return IsDockWindowActivationForeground(
                    target, activationTarget);
            },
            [activationTarget]() {
                SetForegroundWindow(activationTarget);
            },
            [activationTarget]() {
                // A desktop-layer/no-activate Dock is not always the
                // foreground process. Share only the input queues needed to
                // retry the same final target, then detach on every exit.
                const DWORD currentThread = GetCurrentThreadId();
                const HWND currentForeground = GetForegroundWindow();
                const DWORD foregroundThread = currentForeground
                    ? GetWindowThreadProcessId(
                        currentForeground, nullptr)
                    : 0;
                const DWORD targetThread =
                    GetWindowThreadProcessId(
                        activationTarget, nullptr);
                ScopedDockInputQueueAttachment foregroundAttachment(
                    currentThread, foregroundThread);
                ScopedDockInputQueueAttachment targetAttachment(
                    currentThread, targetThread);
                SetForegroundWindow(activationTarget);
            });
}

void RequestDockWindowShow(HWND target, bool wasMinimized)
{
    if (!target || !IsWindow(target))
        return;
    const BOOL showAccepted = ShowWindowAsync(
        target,
        wasMinimized
            ? DockRestoreShowCommand(target)
            : SW_SHOW);
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
                DefWindowProcW(
                    target, WM_SYSCOMMAND,
                    systemCommand, 0);
            },
            [target]() {
                return IsIconic(target) != FALSE;
            },
            [target]() {
                SwitchToThisWindow(target, FALSE);
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
    outcome.synchronousActivationSafe =
        activationTarget &&
        snowdesktop::dock_window_rules::
            IsDockWindowSynchronousActivationSafe(
                !ShouldSkipSynchronousWindowActivation(target),
                !ShouldSkipSynchronousWindowActivation(
                    activationTarget));
    if (snowdesktop::dock_window_rules::
            ShouldSwitchDockWindowAfterShow(
                wasMinimized, outcome.restored))
    {
        outcome.foreground = ActivateDockWindowForeground(
            target, activationTarget,
            outcome.synchronousActivationSafe);
    }
    return outcome;
}

DesktopApp::DockWindowActivationOutcome
DesktopApp::RequestDockWindowActivation(
    HWND target, bool wasMinimized)
{
    CancelAllDockWindowActivationObservations();
    RequestDockWindowShow(target, wasMinimized);
    BeginDockWindowActivationObservation(
        target, wasMinimized);
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
    dockWindowActivationObservations_[target] = {
        awaitingRestore, 0
    };
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
        !outcome.synchronousActivationSafe)
    {
        CancelDockWindowActivationObservation(target);
        return;
    }
    found->second.awaitingRestore = false;
    if (!found->second.activationRetryDeadline)
    {
        found->second.activationRetryDeadline =
            GetTickCount64() +
            kDockWindowActivationRetryDurationMs;
    }
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
        const bool valid = target && IsWindow(target);
        const bool foreground = valid &&
            IsDockWindowActivationForeground(target, target);
        const bool rootWindowSafe = valid &&
            !ShouldSkipSynchronousWindowActivation(target);
        const bool retryExpired =
            found->second.activationRetryDeadline != 0 &&
            now >= found->second.activationRetryDeadline;
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
            CancelDockWindowActivationObservation(target);
            continue;
        }
        if (action == snowdesktop::dock_window_rules::
                DockWindowActivationObservationAction::WaitForRestore)
        {
            continue;
        }

        found->second.awaitingRestore = false;
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

void DesktopApp::RefreshDockRunningWindows(
    bool invalidateChanged, HWND preferredWindow)
{
    // Dock slots own the DockEntryItem/DockRunningItem wrappers retained by a
    // DragSession. The OLE nested loop continues to dispatch maintenance
    // timers after capture and mouseDown_ are cleared, so rebuilding the
    // running-app model here would invalidate those source pointers before
    // native hand-back or synchronous drop completion.
    if (snowdesktop::drag_input_rules::ShouldDeferModelReload(
            dragSession_.HasContext(),
            dragDropController_.IsTransportActive()))
    {
        return;
    }
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
        std::unordered_map<DWORD, std::wstring> processPaths;
    } context{ this, &targets, scoringForeground, actualForeground, &fixedIdentities,
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
    const int requiredIconSize = GetMaximumShellIconBitmapSize();
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
        if (!info.iconBitmap)
        {
            info.iconRequestedSize = requiredIconSize;
            info.iconBitmap = CreateDockWindowIconBitmap(
                info.window, info.executablePath, info.appUserModelId,
                info.iconBitmapSize, requiredIconSize);
        }
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
        floatingDockVisible_ && floatingDockHwnd_ &&
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
        bool transitionStarted = false;
        if (shouldMinimize &&
            dockWindowTransition_ &&
            pressedAnchorScreen)
        {
            transitionStarted =
                dockWindowTransition_->StartMinimize(
                    target, *pressedAnchorScreen,
                    minimizeCapturePolicy,
                    transitionKeepBelowWindow);
        }
        if (shouldMinimize &&
            minimizeCapturePolicy ==
                DockWindowTransitionCapturePolicy::LiveThumbnailOnly &&
            !transitionStarted)
        {
            return false;
        }
        if (shouldMinimize)
        {
            CancelDockWindowActivationObservation(target);
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
        floatingDockVisible_ && floatingDockHwnd_ &&
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
        bool transitionStarted = false;
        if (shouldMinimize &&
            dockWindowTransition_ &&
            pressedAnchorScreen)
        {
            transitionStarted =
                dockWindowTransition_->StartMinimize(
                    target, *pressedAnchorScreen,
                    minimizeCapturePolicy,
                    transitionKeepBelowWindow);
        }
        if (shouldMinimize &&
            minimizeCapturePolicy ==
                DockWindowTransitionCapturePolicy::LiveThumbnailOnly &&
            !transitionStarted)
        {
            return false;
        }
        if (shouldMinimize)
        {
            CancelDockWindowActivationObservation(target);
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
            ActivateDockWindowFromPreview(window);
            return true;
        };
    const bool requiresFloatingDockClose =
        snowdesktop::dock_window_rules::
            RequiresFloatingDockMinimizeCaptureIsolation(
                floatingDockVisible_, action);
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
