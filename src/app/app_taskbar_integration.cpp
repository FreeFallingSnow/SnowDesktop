#include "app.h"

// Dock foreground monitoring and Windows taskbar appearance integration.

void DesktopApp::StartDockForegroundMonitor()
{
    dockForegroundNotificationWindow_.store(
        controlHwnd_ && IsWindow(controlHwnd_)
            ? controlHwnd_ : hwnd_);
    if (dockForegroundEventHook_) return;
    const HWND foreground = GetForegroundWindow();
    dockForegroundWindow_.store(foreground);
    dockPreviousForegroundWindow_.store(nullptr);
    dockForegroundChangedTick_.store(GetTickCount());
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
    systemTaskbarWindowStateChangedTick_.fetch_add(1,
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
        systemTaskbarWindowStateChangedTick_.fetch_add(1,
            std::memory_order_relaxed);
    });
}

void DesktopApp::StopDockForegroundMonitor()
{
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
    systemTaskbarWindowStateChangedTick_.store(0);
    dockWindowListChangedTick_.store(0);
    systemTaskbarWindowStateObservedTick_ = 0;
    systemTaskbarWindowScanTick_ = 0;
    {
        std::scoped_lock lock(
            systemTaskbarWindowObservationMutex_);
        systemTaskbarWindowObservations_.clear();
    }
    dockRunningWindowsForegroundTick_ = 0;
    desktopHoverForegroundObservedTick_ = 0;
    dockRunningWindowsStateTick_ = 0;
    dockRunningWindowsRefreshTick_ = 0;
    systemTaskbarMonitorWindowStates_.clear();
    systemTaskbarWindows_.clear();
    taskbarAppVisibility_.Reset();
    taskbarAppVisibilityAttempted_ = false;
    taskbarSearchVisibility_.reset();
}

bool DesktopApp::IsSystemTaskbarHookRequired(
    const DockSettings& settings) const
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

PersonalizationSettings DesktopApp::ResolveSystemTaskbarDynamicAppearance(
    const SystemTaskbarDynamicRule& rule) const
{
    PersonalizationSettings result;
    switch (rule.themeMode)
    {
    case SystemTaskbarThemeMode::FollowGlobal:
        result = CurrentPersonalization();
        break;
    case SystemTaskbarThemeMode::Dark:
        result = MakeAppearancePreset(kAppearancePresetDark);
        break;
    case SystemTaskbarThemeMode::Light:
        result = MakeAppearancePreset(kAppearancePresetLight);
        break;
    case SystemTaskbarThemeMode::GlassDark:
        result = MakeAppearancePreset(kAppearancePresetGlassDark);
        break;
    case SystemTaskbarThemeMode::GlassLight:
        result = MakeAppearancePreset(kAppearancePresetGlassLight);
        break;
    case SystemTaskbarThemeMode::AcrylicDark:
        result = MakeAppearancePreset(kAppearancePresetAcrylicDark);
        break;
    case SystemTaskbarThemeMode::AcrylicLight:
        result = MakeAppearancePreset(kAppearancePresetAcrylicLight);
        break;
    case SystemTaskbarThemeMode::Transparent:
        result = MakeTransparentTaskbarAppearance();
        break;
    case SystemTaskbarThemeMode::Custom:
        result = rule.appearance;
        break;
    case SystemTaskbarThemeMode::Native:
    default:
        result = PersonalizationSettings::DarkPreset();
        break;
    }
    if (rule.contentTheme >= 0)
        result.contentTheme = rule.contentTheme;
    return result;
}

bool IsSystemTaskbarCandidateWindow(HWND window,
    IVirtualDesktopManager* virtualDesktopManager)
{
    if (!window || GetAncestor(window, GA_ROOT) != window ||
        !IsWindowVisible(window) || IsIconic(window))
        return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!processId || processId == GetCurrentProcessId())
        return false;

    wchar_t className[96]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"Progman") == 0 ||
        _wcsicmp(className, L"WorkerW") == 0 ||
        _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
        return false;

    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 ||
        (GetWindow(window, GW_OWNER) && (exStyle & WS_EX_APPWINDOW) == 0))
        return false;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
        sizeof(cloaked))) && cloaked != 0)
        return false;

    if (virtualDesktopManager)
    {
        BOOL onCurrentDesktop = TRUE;
        if (SUCCEEDED(virtualDesktopManager->IsWindowOnCurrentVirtualDesktop(
            window, &onCurrentDesktop)) && !onCurrentDesktop)
            return false;
    }
    return true;
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

bool DesktopApp::RefreshSystemTaskbarWindowState()
{
    const auto previousStates = systemTaskbarMonitorWindowStates_;
    const bool previousShellUiActive =
        systemTaskbarShellUiActive_;
    const HMONITOR previousShellUiMonitor =
        systemTaskbarShellUiMonitor_;
    systemTaskbarMonitorWindowStates_.clear();

    ComPtr<IVirtualDesktopManager> virtualDesktopManager;
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&virtualDesktopManager));

    struct EnumerationContext
    {
        IVirtualDesktopManager* virtualDesktopManager = nullptr;
        std::unordered_map<HMONITOR,
            SystemTaskbarMonitorWindowState>* states = nullptr;
        std::unordered_map<HWND,
            SystemTaskbarWindowObservation>* observations = nullptr;
    } context{ virtualDesktopManager.Get(),
        &systemTaskbarMonitorWindowStates_ };
    std::unordered_map<HWND,
        SystemTaskbarWindowObservation> observations;
    context.observations = &observations;

    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        auto* context = reinterpret_cast<EnumerationContext*>(value);
        if (!IsSystemTaskbarCandidateWindow(window,
            context->virtualDesktopManager))
            return TRUE;
        const HMONITOR monitor = MonitorFromWindow(window,
            MONITOR_DEFAULTTONULL);
        if (!monitor) return TRUE;
        (*context->observations)[window] = {
            monitor, IsZoomed(window) != FALSE
        };
        auto& state = (*context->states)[monitor];
        state.visible = true;
        if (IsZoomed(window))
            state.maximized = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    {
        std::scoped_lock lock(
            systemTaskbarWindowObservationMutex_);
        systemTaskbarWindowObservations_ =
            std::move(observations);
    }

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
    systemTaskbarShellUiActive_ = launcherVisible || shellViewVisible;
    systemTaskbarShellUiMonitor_ = systemTaskbarShellUiActive_
        ? MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY)
        : nullptr;
    systemTaskbarWindowStateObservedTick_ =
        systemTaskbarWindowStateChangedTick_.load();

    const bool statesEqual =
        previousStates.size() ==
            systemTaskbarMonitorWindowStates_.size() &&
        std::all_of(previousStates.begin(), previousStates.end(),
            [this](const auto& entry) {
                const auto found =
                    systemTaskbarMonitorWindowStates_.find(
                        entry.first);
                return found !=
                        systemTaskbarMonitorWindowStates_.end() &&
                    found->second.visible ==
                        entry.second.visible &&
                    found->second.maximized ==
                        entry.second.maximized;
            });
    return !statesEqual ||
        previousShellUiActive != systemTaskbarShellUiActive_ ||
        previousShellUiMonitor != systemTaskbarShellUiMonitor_;
}

bool DesktopApp::RefreshSystemTaskbarAppearance(
    bool forceWindowScan, bool skipUnchangedWindowState)
{
    const bool hookRequired = IsSystemTaskbarHookRequired(dockSettings_);
    if (!hookRequired)
    {
        ApplySystemTaskbarBackdrop(false, false,
            ResolveSystemTaskbarAppearance(dockSettings_));
        systemTaskbarBackdropRefreshTick_ = GetTickCount();
        return true;
    }

    const DWORD changedTick = systemTaskbarWindowStateChangedTick_.load();
    if (forceWindowScan || changedTick != systemTaskbarWindowStateObservedTick_)
    {
        const bool windowStateChanged =
            RefreshSystemTaskbarWindowState();
        systemTaskbarWindowScanTick_ = GetTickCount();
        if (skipUnchangedWindowState &&
            !windowStateChanged)
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
    std::vector<SystemTaskbarTargetAppearance> targets;
    targets.reserve(context.windows.size());
    for (HWND taskbar : context.windows)
    {
        const HMONITOR monitor = MonitorFromWindow(taskbar,
            MONITOR_DEFAULTTONULL);
        const auto stateIt = systemTaskbarMonitorWindowStates_.find(monitor);
        const SystemTaskbarMonitorWindowState state =
            stateIt == systemTaskbarMonitorWindowStates_.end()
            ? SystemTaskbarMonitorWindowState{} : stateIt->second;

        const SystemTaskbarDynamicRule* selectedRule = nullptr;
        if (dockSettings_.systemTaskbarShellUi.enabled &&
            (systemTaskbarTaskViewActive_ ||
             (systemTaskbarShellUiActive_ &&
              monitor == systemTaskbarShellUiMonitor_)))
            selectedRule = &dockSettings_.systemTaskbarShellUi;
        else if (dockSettings_.systemTaskbarMaximizedWindow.enabled &&
            state.maximized)
            selectedRule = &dockSettings_.systemTaskbarMaximizedWindow;
        else if (dockSettings_.systemTaskbarVisibleWindow.enabled &&
            state.visible)
            selectedRule = &dockSettings_.systemTaskbarVisibleWindow;

        SystemTaskbarTargetAppearance target;
        target.taskbar = taskbar;
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

    ApplySystemTaskbarBackdrop(true,
        dockSettings_.systemTaskbarBackdropEnabled, defaultAppearance, targets);
    systemTaskbarBackdropRefreshTick_ = GetTickCount();
    return true;
}

void DesktopApp::UpdateSystemTaskbarRevealGuard()
{
    if (!generalSettings_.dockEnabled || !dockSettings_.systemTaskbarAutoHide ||
        dockSettings_.edgeAttached ||
        dockSettings_.position != DockPosition::Bottom)
        return;

    constexpr int kRevealGuardPixels = 6;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const HMONITOR cursorMonitor =
        MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO cursorMonitorInfo{};
    cursorMonitorInfo.cbSize = sizeof(cursorMonitorInfo);
    if (!cursorMonitor ||
        !GetMonitorInfoW(cursorMonitor, &cursorMonitorInfo) ||
        cursor.y < cursorMonitorInfo.rcMonitor.bottom -
            kRevealGuardPixels)
        return;

    if (!IsSystemTaskbarAutoHideEnabled())
        return;

    APPBARDATA taskbarPosition{};
    taskbarPosition.cbSize = sizeof(taskbarPosition);
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &taskbarPosition) ||
        taskbarPosition.uEdge != ABE_BOTTOM)
        return;

    if (!hwnd_) return;

    RECT screen{};
    bool foundProtectedDock = false;
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        RECT candidate = dock->GetBounds();
        POINT topLeft{ candidate.left, candidate.top };
        POINT bottomRight{ candidate.right, candidate.bottom };
        if (!ClientToScreen(hwnd_, &topLeft) ||
            !ClientToScreen(hwnd_, &bottomRight))
            continue;
        candidate = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
        const POINT center{
            (candidate.left + candidate.right) / 2,
            (candidate.top + candidate.bottom) / 2
        };
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        if (!monitor || monitor != cursorMonitor ||
            !GetMonitorInfoW(monitor, &monitorInfo))
            continue;
        if (cursor.x < candidate.left || cursor.x >= candidate.right ||
            cursor.y < candidate.top || cursor.y >= monitorInfo.rcMonitor.bottom)
            continue;
        screen = monitorInfo.rcMonitor;
        foundProtectedDock = true;
        break;
    }
    if (!foundProtectedDock) return;

    const int guardedEdgeTop = screen.bottom - kRevealGuardPixels;
    if (cursor.y >= guardedEdgeTop)
        SetCursorPos(cursor.x, guardedEdgeTop - 1);
}

void DesktopApp::ToggleWindowsStartMenu()
{
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_LWIN;
    input[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    input[1] = input[0];
    input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT));
}
