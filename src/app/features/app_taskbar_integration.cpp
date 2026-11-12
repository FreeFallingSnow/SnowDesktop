#include "app/app.h"
#include "dock/dock_settings_rules.h"
#include "platform/runtime_module_policy.h"
#include "diagnostics/diagnostic_log.h"
#include "shell/native_start_menu.h"
#include "theme/taskbar_appearance.h"
#include "dock/dock_external_minimize.h"
#include "taskbar_dynamic/search_visibility_detector.h"
#include "dock/dock_settings.h"
#include "app/dock/dock_taskbar_diagnostics.h"
#include "platform/taskbar_monitor.h"
#include "taskbar_hook/taskbar_native.h"
#include "diagnostics/performance_trace.h"
#include "shell/win_key_start_tap.h"

// Dock foreground monitoring and Windows taskbar appearance integration.

void DesktopApp::PrepareDockWindowMinimize(HWND window, const wchar_t* origin, DWORD deadline)
{
    snowdesktop::performance::Scope trace("dock.transition", "minimize.taskbar");
    snowdesktop::dock_taskbar_diagnostics::Begin(window, origin);
    if ((snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::Taskbar) && snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::WindowMinimize)) && ShouldProtectAutoHideTaskbar(dockSettings_, generalSettings_.dockEnabled,
            IsSystemTaskbarAutoHideEnabled()))
    {
        // CBT is already waiting. Use the published taskbar configuration
        // maintained by the normal observation path instead of scanning here.
        if (!deadline) RefreshSystemTaskbarAppearance(false);
        if (!BeginSystemTaskbarMinimizeProtection(window, false, deadline))
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Taskbar, L"[TaskbarMinimizeProtection] not all taskbars acknowledged temporary cloaking",
                DiagnosticLogLevel::Warning);
    }
    snowdesktop::dock_taskbar_diagnostics::Record(L"minimize-taskbar-protection-submitted", window);
}

void DesktopApp::StartDockForegroundMonitor()
{
    if (!dockShellHookWindow_)
    {
        dockShellHookMessage_ = RegisterWindowMessageW(L"SHELLHOOK");
        const HWND target = controlHwnd_ && IsWindow(controlHwnd_)
            ? controlHwnd_ : hwnd_;
        if (dockShellHookMessage_ && target && RegisterShellHookWindow(target))
            dockShellHookWindow_ = target;
        else
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Taskbar, L"Dock attention Shell hook registration failed", DiagnosticLogLevel::Warning);
    }
    dockForegroundNotificationWindow_.store(
        controlHwnd_ && IsWindow(controlHwnd_)
            ? controlHwnd_ : hwnd_);
    if (dockForegroundEventHook_) return;
    const HWND foreground = GetForegroundWindow();
    dockForegroundWindow_.store(foreground);
    dockPreviousForegroundWindow_.store(nullptr);
    dockForegroundChangedTick_.store(GetTickCount());
    dockSystemMinimizeStartedTick_.store(0);
    systemShowDesktopLayerGuardActive_ = false;
    systemShowDesktopLastProtectedForegroundTick_ = 0;
    // A monitor restart must not leave the bar in a stale protected band.
    if (statusBar_) statusBar_->SetSystemShowDesktopActive(false);
    dockForegroundEventHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND, nullptr, &DesktopApp::DockForegroundWinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT);

    const auto addHook = [this](DWORD first, DWORD last) {
        if (HWINEVENTHOOK hook = SetWinEventHook(first, last, nullptr,
            &DesktopApp::DockForegroundWinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT))
            systemTaskbarWindowEventHooks_.push_back(hook);
    };
    addHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    addHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE);
    addHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE);
#ifdef EVENT_OBJECT_CLOAKED
    addHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);
#endif
    RestartSystemTaskbarShellVisibilityDetectors();
    windowObservationRevision_.fetch_add(1,
        std::memory_order_relaxed);
    dockWindowListChangedTick_.fetch_add(
        1, std::memory_order_relaxed);
}

void DesktopApp::RestartSystemTaskbarShellVisibilityDetectors()
{
    taskbarAppVisibility_.Reset();
    taskbarAppVisibilityAttempted_ = false;
    taskbarSearchVisibility_.reset();
    taskbarSearchVisibility_ = std::make_unique<SearchVisibilityDetector>([] {
        windowObservationRevision_.fetch_add(1,
            std::memory_order_relaxed);
    });
}

void DesktopApp::StopDockForegroundMonitor()
{
    if (dockStartMenu_) dockStartMenu_->Cancel();
    if (dockShellHookWindow_)
        DeregisterShellHookWindow(dockShellHookWindow_);
    dockShellHookWindow_ = nullptr;
    dockShellHookMessage_ = 0;
    // Desktop/Explorer recovery also stops this monitor. Preserve pending
    // requests across that rebind; the normal refresh prunes expired windows.
    dockExternalMinimize_.reset();
    snowdesktop::dock_taskbar_diagnostics::Stop();
    dockForegroundNotificationWindow_.store(nullptr);
    if (dockForegroundEventHook_)
    {
        UnhookWinEvent(dockForegroundEventHook_);
        dockForegroundEventHook_ = nullptr;
    }
    for (HWINEVENTHOOK hook : systemTaskbarWindowEventHooks_)
        if (hook) UnhookWinEvent(hook);
    systemTaskbarWindowEventHooks_.clear();
    dockForegroundWindow_.store(nullptr);
    dockPreviousForegroundWindow_.store(nullptr);
    dockForegroundChangedTick_.store(0);
    dockSystemMinimizeStartedTick_.store(0);
    systemShowDesktopLayerGuardActive_ = false;
    systemShowDesktopLastProtectedForegroundTick_ = 0;
    // The bar outlives this monitor across Explorer/topology recovery; drop its
    // protection instead of leaving a stale TOPMOST/visible guard behind.
    if (statusBar_) statusBar_->SetSystemShowDesktopActive(false);
    windowObservationRevision_.store(0);
    dockWindowListChangedTick_.store(0);
    observedWindowRevision_ = 0;
    windowObservationTick_ = 0;
    taskbarObservedShellRevision_ = 0;
    {
        std::scoped_lock lock(
            windowPlacementMutex_);
        observedWindowPlacements_.clear();
    }
    dockRunningWindowsForegroundTick_ = 0;
    desktopHoverForegroundObservedTick_ = 0;
    dockRunningWindowsStateTick_ = 0;
    dockRunningWindowsRefreshTick_ = 0;
    observedMonitorWindows_.clear();
    ++monitorSceneRevision_;
    UpdateDesktopWidgetCoverage({});
    systemTaskbarWindows_.clear();
    taskbarAppVisibility_.Reset();
    taskbarAppVisibilityAttempted_ = false;
    taskbarSearchVisibility_.reset();
}

bool DesktopApp::IsShellDesktopForegroundWindow(HWND window) const
{
    if (!window || !IsWindow(window))
        return false;
    if (window == desktopWindows_.progman ||
        window == desktopWindows_.host)
        return true;

    wchar_t className[96]{};
    GetClassNameW(window, className,
        static_cast<int>(std::size(className)));
    return _wcsicmp(className, L"Progman") == 0 ||
        _wcsicmp(className, L"WorkerW") == 0;
}

void DesktopApp::HandleDockForegroundInteractionChanged(HWND window, DWORD eventTime)
{
    if (window && statusBar_) statusBar_->ObserveForeground(window, eventTime);
    ClearSelectionForExternalWindow(GetForegroundWindow());
    ReconcileDesktopHoverState();
    UpdateSystemShowDesktopLayerGuard();
    RefreshDockForegroundState();
}

void DesktopApp::UpdateSystemShowDesktopLayerGuard()
{
    using GuardAction = snowdesktop::floating_dock_rules::
        SystemShowDesktopLayerGuardAction;
    using Foreground = snowdesktop::floating_dock_rules::
        SystemShowDesktopForeground;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG evidenceTick = dockSystemMinimizeStartedTick_.load();
    // Also called by the periodic guard timer: sample Windows directly so a
    // missed/delayed WinEvent cannot keep a stale desktop foreground alive.
    const HWND foreground = GetForegroundWindow();
    Foreground foregroundKind = Foreground::Unavailable;
    if (foreground && IsWindow(foreground) &&
        IsWindowVisible(foreground) && !IsIconic(foreground))
    {
        if (IsShellDesktopForegroundWindow(foreground))
            foregroundKind = Foreground::ShellDesktop;
        else if (IsDesktopInteractionSurfaceWindow(foreground) ||
            foreground == floatingDockInputHwnd_ ||
            foreground == snowdesktop::modern_menu::ActiveRootWindow() ||
            (shellPopupMenuLayerDepth_ > 0 &&
                foreground == shellPopupTrackerOwnerHwnd_.load()))
            foregroundKind = Foreground::DesktopSurface;
        else
            foregroundKind = Foreground::Application;
    }
    if (foregroundKind == Foreground::ShellDesktop ||
        foregroundKind == Foreground::DesktopSurface)
        systemShowDesktopLastProtectedForegroundTick_ = now;
    // One Shell transition protects every presented surface: the Dock layer and
    // an independent status bar that Explorer would otherwise take with the
    // other top-level windows.
    const bool statusBarPresentation =
        statusBar_ && statusBar_->HasIndependentPresentation();
    const bool protectedSurface =
        (generalSettings_.dockEnabled && floatingDockHostActive_) ||
        statusBarPresentation;
    const GuardAction action =
        snowdesktop::floating_dock_rules::
            ResolveSystemShowDesktopLayerGuardAction(
                systemShowDesktopLayerGuardActive_,
                protectedSurface,
                foregroundKind,
                evidenceTick,
                now,
                systemShowDesktopLastProtectedForegroundTick_);
    if (action == GuardAction::None)
        return;

    systemShowDesktopLayerGuardActive_ =
        action == GuardAction::Start;
    if (action == GuardAction::Stop)
    {
        systemShowDesktopLastProtectedForegroundTick_ = 0;
        // Returning to the desktop after an application switch must require
        // new minimize evidence, not reuse the start that owned this guard.
        ULONGLONG consumedEvidence = evidenceTick;
        dockSystemMinimizeStartedTick_.compare_exchange_strong(
            consumedEvidence, 0);
    }
    wchar_t guardTrace[256]{};
    swprintf_s(guardTrace,
        L"System Show Desktop layer guard %ls foreground=%p "
        L"foregroundKind=%d evidenceTick=%llu now=%llu",
        systemShowDesktopLayerGuardActive_ ? L"started" : L"completed",
        static_cast<void*>(foreground),
        static_cast<int>(foregroundKind),
        evidenceTick, now);
    snowdesktop::dock_taskbar_diagnostics::Record(
        guardTrace, foreground);
    ApplyFloatingDockLayerPolicy();
    if (statusBar_)
        statusBar_->SetSystemShowDesktopActive(
            systemShowDesktopLayerGuardActive_);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Taskbar, guardTrace);
}

namespace
{
bool AppearanceRequiresTaskbarHook(const DockSettings& settings)
{
    const auto ruleNeedsHook = [](const SystemTaskbarDynamicRule& rule) {
        return rule.enabled &&
            rule.themeMode != SystemTaskbarThemeMode::Native;
    };
    return settings.systemTaskbarBackdropEnabled ||
        ruleNeedsHook(settings.systemTaskbarVisibleWindow) ||
        ruleNeedsHook(settings.systemTaskbarMaximizedWindow) ||
        ruleNeedsHook(settings.systemTaskbarShellUi);
}
}

bool DesktopApp::IsSystemTaskbarHookRequired(const DockSettings& settings) const
{
    namespace modules = snowdesktop::runtime_modules;
    return modules::Allowed(modules::Module::Taskbar) &&
        (ShouldSuppressSystemTaskbar(settings, generalSettings_.dockEnabled) ||
         AppearanceRequiresTaskbarHook(settings) ||
         (modules::Allowed(modules::Module::StartMenu) &&
          snowdesktop::dock_settings_rules::ProtectDockStart(generalSettings_.dockEnabled,
             settings.startMenuKeepTaskbarHidden, settings.systemTaskbarAutoHide || settings.suppressSystemTaskbar)) ||
         (modules::Allowed(modules::Module::WindowMinimize) &&
          ShouldProtectAutoHideTaskbar(settings, generalSettings_.dockEnabled, settings.systemTaskbarAutoHide)));
}

PersonalizationSettings DesktopApp::ResolveSystemTaskbarDynamicAppearance(
    const SystemTaskbarDynamicRule& rule) const
{
    return snowdesktop::ResolveTaskbarRuleAppearance(rule, CurrentPersonalization());
}

bool IsSystemTaskbarShellUiWindow(HWND window)
{
    if (!window) return false;
    wchar_t className[96]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"ControlCenterWindow") == 0 ||
        _wcsicmp(className, L"WindowsDashboard") == 0)
        return true;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!processId) return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
        processId);
    if (!process) return false;
    wchar_t path[MAX_PATH]{};
    DWORD pathLength = static_cast<DWORD>(std::size(path));
    const bool queried = QueryFullProcessImageNameW(process, 0, path,
        &pathLength) != FALSE;
    CloseHandle(process);
    if (!queried) return false;

    const wchar_t* fileName = wcsrchr(path, L'\\');
    fileName = fileName ? fileName + 1 : path;
    return _wcsicmp(fileName, L"SearchHost.exe") == 0 ||
        _wcsicmp(fileName, L"SearchApp.exe") == 0 ||
        _wcsicmp(fileName, L"WidgetBoard.exe") == 0 ||
        (_wcsicmp(fileName, L"ShellExperienceHost.exe") == 0 &&
            _wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0);
}

bool DesktopApp::RefreshSystemTaskbarShellUiState()
{
    const bool previousShellUiActive = systemTaskbarShellUiActive_;
    const HMONITOR previousShellUiMonitor = systemTaskbarShellUiMonitor_;
    {
        snowdesktop::performance::Scope shellTrace("shared.taskbar", "scan.shell_ui");
        bool launcherVisible = false;
        if (!taskbarAppVisibilityAttempted_)
        {
            taskbarAppVisibilityAttempted_ = true;
            CoCreateInstance(CLSID_AppVisibility, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&taskbarAppVisibility_));
        }
        if (taskbarAppVisibility_)
        {
            BOOL visible = FALSE;
            if (SUCCEEDED(taskbarAppVisibility_->IsLauncherVisible(&visible)))
                launcherVisible = visible != FALSE;
        }

        const HWND foreground = GetForegroundWindow();
        // ShellViewCoordinator is the preferred event source. On newer Windows
        // 11 builds it may keep reporting Hidden for Search and the system
        // sidebars, so retain their foreground identities as a narrow fallback.
        const bool shellViewVisible =
            (taskbarSearchVisibility_ &&
                taskbarSearchVisibility_->IsVisible()) ||
            IsSystemTaskbarShellUiWindow(foreground);
        // A Dock-origin Start session owns its taskbar guard. Do not cache Start
        // as a reason to unhide the bar immediately after that guard is released.
        systemTaskbarShellUiActive_ = snowdesktop::dock_settings_rules::ShellPanelVisibility(
            launcherVisible, IsDockStartMenuActive(), shellViewVisible);
        systemTaskbarShellUiMonitor_ = systemTaskbarShellUiActive_
            ? MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY)
            : nullptr;
    }
    return previousShellUiActive != systemTaskbarShellUiActive_ ||
        previousShellUiMonitor != systemTaskbarShellUiMonitor_;
}

bool DesktopApp::RefreshSystemTaskbarAppearance(
    bool forceWindowScan, bool skipUnchangedWindowState)
{
    snowdesktop::performance::Scope trace("shared.taskbar", "refresh");
    {
        namespace modules = snowdesktop::runtime_modules;
        const bool protectWinKey = modules::Allowed(modules::Module::StartMenu) &&
            modules::Allowed(modules::Module::Taskbar) &&
            snowdesktop::dock_settings_rules::ProtectWinKeyStart(
                generalSettings_.dockEnabled,
                ShowDockWindowsButton(dockSettings_),
                dockSettings_.startMenuKeepTaskbarHidden,
                dockSettings_.startMenuKeepTaskbarHiddenOnWinKey,
                dockSettings_.suppressSystemTaskbar || IsSystemTaskbarAutoHideEnabled(),
                desktopPassthroughActive_);
        const HWND target = controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : nullptr;
        snowdesktop::SyncWinKeyStartTap(target, protectWinKey);
    }
    const auto applyClassicSystemTheme = [this](bool enabled,
        const PersonalizationSettings& appearance) {
        if (!IsClassicSystemTaskbar()) return;
        const auto light = snowdesktop::dock_settings_rules::
            ResolveClassicTaskbarSystemLightTheme(
                dockSettings_.classicTaskbarSystemTheme, enabled, appearance.contentTheme);
        if (light && IsWindowsSystemLightThemeEnabled() != *light)
            (void)RequestWindowsSystemLightThemeEnabled(*light);
    };
    const bool appearanceRequired = AppearanceRequiresTaskbarHook(dockSettings_);
    const bool autoHide = IsSystemTaskbarAutoHideEnabled();
    const bool protectActivation = snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::Taskbar) &&
        ((snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::WindowMinimize) &&
            ShouldProtectAutoHideTaskbar(dockSettings_, generalSettings_.dockEnabled, autoHide)) ||
        (snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::StartMenu) &&
         snowdesktop::dock_settings_rules::ProtectDockStart(generalSettings_.dockEnabled,
            dockSettings_.startMenuKeepTaskbarHidden, autoHide || dockSettings_.suppressSystemTaskbar)));
    const bool suppressionRequested = ShouldSuppressSystemTaskbar(dockSettings_, generalSettings_.dockEnabled);
    const bool hookRequired = snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::Taskbar) && (appearanceRequired || protectActivation || suppressionRequested);
    if (!hookRequired)
    {
        snowdesktop::performance::Value("shared.taskbar", "refresh.disabled", {}, 1);
        applyClassicSystemTheme(false, dockSettings_.systemTaskbarAppearance);
        ApplySystemTaskbarBackdrop(false, false,
            ResolveSystemTaskbarAppearance(dockSettings_));
        systemTaskbarBackdropRefreshTick_ = GetTickCount();
        return true;
    }

    if (forceWindowScan || windowObservationTick_ == 0 ||
        windowObservationRevision_.load() != observedWindowRevision_)
        RefreshWindowObservations();
    bool shellChanged = false;
    if (forceWindowScan || taskbarObservedShellRevision_ != observedWindowRevision_)
    {
        shellChanged = RefreshSystemTaskbarShellUiState();
        taskbarObservedShellRevision_ = observedWindowRevision_;
    }
    if (skipUnchangedWindowState && !shellChanged &&
        taskbarAppliedMonitorSceneRevision_ == monitorSceneRevision_)
    {
        snowdesktop::performance::Value("shared.taskbar", "refresh.window_state_unchanged", {}, 1);
        return false;
    }

    struct TaskbarEnumerationContext
    {
        std::vector<HWND> windows;
    } context;
    const auto appendTaskbar = [&context](HWND window) {
        if (!window || !IsWindow(window)) return;
        wchar_t className[64]{};
        GetClassNameW(window, className,
            static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"Shell_TrayWnd") != 0 &&
            _wcsicmp(className, L"Shell_SecondaryTrayWnd") != 0)
            return;
        if (std::find(context.windows.begin(), context.windows.end(),
            window) == context.windows.end())
            context.windows.push_back(window);
    };
    // Keep valid handles discovered before a shell surface temporarily
    // reparents every taskbar (Task View does this on all monitors).
    for (HWND taskbar : systemTaskbarWindows_)
        appendTaskbar(taskbar);
    // Shell surfaces temporarily reparent the primary taskbar while Start,
    // Search or Task View is open. It then disappears from EnumWindows even
    // though Shell_TrayWnd remains valid and visible. Always seed it directly.
    appendTaskbar(FindWindowW(L"Shell_TrayWnd", nullptr));
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        wchar_t className[64]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
            _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
        {
            auto& windows =
                reinterpret_cast<TaskbarEnumerationContext*>(value)->windows;
            if (std::find(windows.begin(), windows.end(), window) ==
                windows.end())
                windows.push_back(window);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    systemTaskbarWindows_ = context.windows;

    const PersonalizationSettings defaultAppearance =
        ResolveSystemTaskbarAppearance(dockSettings_);
    std::vector<HMONITOR> dockMonitors;
    if (suppressionRequested && !dockSettings_.suppressSystemTaskbarAllMonitors && hwnd_)
    {
        for (const auto& container : containers_)
        {
            const auto* dock = dynamic_cast<DockContainer*>(container.get());
            if (!dock) continue;
            const RECT bounds = dock->GetBounds();
            if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) continue;
            // Use the same virtual-desktop origin as SyncPersistentDockHosts.
            POINT center{bounds.left + (bounds.right - bounds.left) / 2 + virtualLeft_,
                bounds.top + (bounds.bottom - bounds.top) / 2 + virtualTop_};
            if (const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONULL))
                dockMonitors.push_back(monitor);
        }
    }
    std::vector<SystemTaskbarTargetAppearance> targets;
    targets.reserve(context.windows.size());
    for (HWND taskbar : context.windows)
    {
        const HMONITOR monitor = snowdesktop::taskbar_monitor::Resolve(taskbar);
        const bool hasDock = monitor &&
            std::find(dockMonitors.begin(), dockMonitors.end(), monitor) != dockMonitors.end();
        const auto stateIt = observedMonitorWindows_.find(monitor);
        const snowdesktop::window_observation::MonitorState state =
            stateIt == observedMonitorWindows_.end()
            ? snowdesktop::window_observation::MonitorState{} : stateIt->second;

        const bool shellPanelVisible = snowdesktop::dock_settings_rules::
            ShouldRevealTaskbarForShellPanel(systemTaskbarTaskViewActive_,
                systemTaskbarShellUiActive_, monitor == systemTaskbarShellUiMonitor_) ||
            GetPropW(taskbar, snowdesktop::taskbar_hook::native::kContextMenuProperty) != nullptr;
        const SystemTaskbarDynamicRule* selectedRule = nullptr;
        if (dockSettings_.systemTaskbarShellUi.enabled && shellPanelVisible)
            selectedRule = &dockSettings_.systemTaskbarShellUi;
        else if (dockSettings_.systemTaskbarMaximizedWindow.enabled &&
            state.maximized)
            selectedRule = &dockSettings_.systemTaskbarMaximizedWindow;
        else if (dockSettings_.systemTaskbarVisibleWindow.enabled &&
            state.visible)
            selectedRule = &dockSettings_.systemTaskbarVisibleWindow;

        SystemTaskbarTargetAppearance target;
        target.taskbar = taskbar;
        // Panel access remains available even with all appearance rules off.
        target.shellPanelVisible = shellPanelVisible;
        ConfigureSystemTaskbarTargetProtection(
            target, suppressionRequested, protectActivation, hasDock,
            dockSettings_.suppressSystemTaskbarAllMonitors);
        if (selectedRule)
        {
            target.enabled =
                selectedRule->themeMode != SystemTaskbarThemeMode::Native;
            target.appearance =
                ResolveSystemTaskbarDynamicAppearance(*selectedRule);
        }
        else
        {
            target.enabled = dockSettings_.systemTaskbarBackdropEnabled;
            target.appearance = defaultAppearance;
        }
        targets.push_back(std::move(target));
    }

    // Windows 10 has one global shell theme. Resolve it once from the primary
    // taskbar's active scene, never let differently styled monitors compete.
    const HWND primaryTaskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const auto primary = std::find_if(targets.begin(), targets.end(),
        [primaryTaskbar](const auto& target) { return target.taskbar == primaryTaskbar; });
    if (primary != targets.end())
        applyClassicSystemTheme(primary->enabled, primary->appearance);

    const bool suppressTaskbar = std::any_of(targets.begin(), targets.end(),
        [](const auto& target) { return target.suppressTaskbar; });
    ApplySystemTaskbarBackdrop(true,
        dockSettings_.systemTaskbarBackdropEnabled, defaultAppearance, targets, appearanceRequired, suppressTaskbar);
    taskbarAppliedMonitorSceneRevision_ = monitorSceneRevision_;
    systemTaskbarBackdropRefreshTick_ = GetTickCount();
    return true;
}
