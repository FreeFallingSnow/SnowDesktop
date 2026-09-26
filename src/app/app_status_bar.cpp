#include "app.h"
#include "../system_panel_model.h"
#include "system_controls.h"
#include "modern_menu.h"
#include "../status_bar_view.h"
#include "../status_bar_shell_shortcut.h"
#include "../dock_settings_rules.h"
#include "../taskbar_monitor.h"
#include "../taskbar_hook/taskbar_native.h"

void DesktopApp::ActivateStatusBar(snowdesktop::StatusBarAction action, HWND owner, RECT anchor)
{
    using Action = snowdesktop::StatusBarAction;
    uiAnimationScheduler_.Cancel(statusBarActivationToken_);
    statusBarActivationToken_ = 0;
    // The no-activate bar does not cause WM_ACTIVATE on an open surface.
    // A blank-area click must therefore explicitly dismiss it, including any
    // replacement action still waiting for a nested menu loop to unwind.
    if (action == Action::Dismiss)
    {
        quickNavigationPostCloseAction_ = {};
        snowdesktop::modern_menu::DismissActive();
        if (systemPanel_) systemPanel_->Hide();
        CloseQuickNavigation();
        return;
    }
    // A bar click can arrive inside the menu's nested message loop. Wait until
    // menu focus restoration has finished before opening another surface.
    if (snowdesktop::modern_menu::IsActive())
    {
        snowdesktop::modern_menu::DismissActive();
        statusBarActivationToken_ = uiAnimationScheduler_.ScheduleOnce(1, [this, action, owner, anchor](auto token) {
            if (token != statusBarActivationToken_) return;
            statusBarActivationToken_ = 0;
            if (statusBar_ && IsWindow(owner) && !statusBar_->IsFullscreen(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST)))
                ActivateStatusBar(action, owner, anchor);
        });
        return;
    }
    if (action == Action::None) return;
    const auto resume = [this, action, owner, anchor] {
        if (!exitRequested_ && generalSettings_.statusBar.enabled && statusBar_ && IsWindow(owner) &&
            IsWindowVisible(owner) && !statusBar_->IsFullscreen(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST)))
            ActivateStatusBar(action, owner, anchor);
    };
    if (action != Action::QuickSearch && (quickNavigationOpen_ || !quickNavigationAnimation_.IsHidden()))
    {
        quickNavigationPostCloseAction_ = resume;
        if (quickNavigationOpen_) CloseQuickNavigation();
        return;
    }
    const bool externalSurface = action == Action::SystemMenu || action == Action::Menu ||
        action == Action::QuickSearch || action == Action::Settings ||
        action == Action::Notifications || action == Action::SystemControlCenter;
    if (externalSurface && systemPanel_ && systemPanel_->IsOpen())
    {
        systemPanel_->CloseThen(resume);
        return;
    }
    const bool systemControls = action == Action::SystemControlCenter;
    if (action == Action::Notifications || systemControls)
    {
        if (systemPanel_) systemPanel_->Hide();
        CloseQuickNavigation();
        const WORD key = systemControls || IsClassicSystemTaskbar() ? 'A' : 'N';
        const HWND foreground = GetForegroundWindow();
        statusBarActivationToken_ = snowdesktop::ScheduleStatusBarShellShortcut(uiAnimationScheduler_, key, {
            [](int code) { return (GetAsyncKeyState(code) & 0x8000) != 0; },
            [](UINT count, INPUT* input, int size) { return SendInput(count, input, size); },
            [this, owner, anchor, foreground](auto token) {
                return token == statusBarActivationToken_ && statusBar_ && IsWindow(owner) && IsWindowVisible(owner) &&
                    GetForegroundWindow() == foreground &&
                    !statusBar_->IsFullscreen(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST));
            },
            [this](auto token, snowdesktop::StatusBarShortcutResult result) {
                if (token != statusBarActivationToken_) return;
                statusBarActivationToken_ = 0;
                if (result == snowdesktop::StatusBarShortcutResult::Failed || result == snowdesktop::StatusBarShortcutResult::TimedOut)
                {
                    WriteDiagnosticLogEntry(result == snowdesktop::StatusBarShortcutResult::Failed ?
                        L"StatusBar system shortcut input failed" : L"StatusBar system shortcut modifier release timed out");
                    MessageBeep(MB_ICONWARNING);
                }
            }});
    }
    else if (action == Action::SystemMenu)
    {
        CloseQuickNavigation();
        if (systemPanel_) systemPanel_->Hide();
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        const char* labels[]{"statusBar.taskManager", "statusBar.terminal", "statusBar.systemSettings",
            "controlCenter.lock", "controlCenter.sleep", "controlCenter.restart", "controlCenter.shutdown"};
        for (UINT index = 0; index < std::size(labels); ++index)
        {
            if (index == 3) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, index + 1, _LW(labels[index]));
        }
        const UINT command = ShowModernMenu(menu, {anchor.left, anchor.bottom}, owner);
        DestroyMenu(menu);
        if (command >= 1 && command <= 3)
        {
            const wchar_t* target = command == 1 ? L"taskmgr.exe" : command == 2 ? L"wt.exe" : L"ms-settings:";
            auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", target, nullptr, nullptr, SW_SHOWNORMAL));
            if (opened <= 32 && command == 2)
                opened = reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", L"powershell.exe", nullptr, nullptr, SW_SHOWNORMAL));
            if (opened <= 32) MessageBeep(MB_ICONWARNING);
        }
        else if (command >= 4 && command <= 7)
        {
            if (command >= 5 && MessageBoxW(owner, _LW(command == 5 ? "controlCenter.confirmSleep" : command == 6 ? "controlCenter.confirmRestart" : "controlCenter.confirmShutdown"),
                    _LW(labels[command - 1]), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
            snowdesktop::system_control::Request request;
            const char* tasks[]{"system.power.lock", "system.power.sleep", "system.power.restart", "system.power.shutdown"};
            request.name = tasks[command - 4]; request.hostConfirmed = command >= 5;
            if (!systemDataProvider_->Controls()->Start("statusBarVolume", std::move(request))) MessageBeep(MB_ICONWARNING);
        }
    }
    else if (action == Action::Menu)
    {
        if (systemPanel_) systemPanel_->Hide();
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, 1, _LW("statusBar.menu.settings"));
        AppendMenuW(menu, MF_STRING, 2, _LW("statusBar.taskManager"));
        const UINT command = ShowModernMenu(menu, {anchor.left, anchor.bottom}, owner);
        DestroyMenu(menu);
        if (command == 1) ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::StatusBar));
        else if (command == 2)
        {
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                MessageBeep(MB_ICONWARNING);
        }
    }
    else if (action == Action::QuickSearch)
    {
        if (systemPanel_) systemPanel_->Hide();
        if (quickNavigationOpen_) CloseQuickNavigation();
        else OpenQuickNavigation(QuickNavigationInvocationSource::Pointer);
    }
    else if (action == Action::Settings)
        ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::StatusBar));
    else if (statusBar_ && !statusBar_->IsFullscreen(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST)))
    {
        if (!systemPanel_)
            systemPanel_ = std::make_unique<snowdesktop::SystemPanel>([this](const auto& changed) {
                if (!settingsController_) return;
                auto settings = settingsController_->Snapshot()->values.general;
                // The popup owns only tray preferences. Do not overwrite
                // a concurrent settings-page edit with its old snapshot.
                settings.statusBar.pinnedTrayItems = changed.pinnedTrayItems;
                settings.statusBar.trayOrder = changed.trayOrder;
                settingsController_->UpdateGeneral(std::move(settings), snowdesktop::SettingsUpdateMode::PreviewAndCommit);
                uiAnimationScheduler_.ScheduleOnce(0, [this](auto) {
                    if (settingsController_) (void)settingsController_->FlushPending();
                });
            }, snowdesktop::SystemCalendarActions{
                [this](const std::string& date) {
                    return widgetEngine_ ? widgetEngine_->RuntimeCalendarEvents(date, date) :
                        std::vector<snowdesktop::calendar::CalendarEvent>{};
                },
                [this] {
                    if (systemPanel_) systemPanel_->Hide();
                    ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Calendar, "calendar.events"));
                }, {}, [this](const std::string& date) {
                    if (!widgetEngine_) return std::string{};
                    const auto& days = widgetEngine_->RuntimeCalendarAnnotations(date, date);
                    return !days.empty() && days.front().calendarAvailable ? days.front().fullDate : std::string{};
                }}, [this](std::string_view key, POINT screen) {
                    return statusBar_ && statusBar_->DropTrayIcon(key, screen);
                }, &uiAnimationScheduler_, dcompDevice_.Get(), dwriteFactory_.Get(),
                [this](ID2D1DeviceContext* context, RECT frame, const PersonalizationSettings& appearance, float scale) {
                    DrawWidgetPanelBackground(context, frame, appearance.cornerRadius * scale,
                        D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                        D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha),
                        false, 0, &appearance, false, 0, scale);
                    brushCache_.clear(); brushCacheContext_ = nullptr;
                });
        systemPanel_->Show(action, owner, anchor, collectionPopupAppearance_, generalSettings_.statusBar,
            statusBar_->Tray(), systemDataProvider_);
    }
}

void DesktopApp::SyncStatusBar()
{
    if (!systemDataProvider_ || !dcompDevice_ || !dwriteFactory_) return;
    if (systemPanel_) systemPanel_->UpdateSettings(generalSettings_.statusBar);
    if (!generalSettings_.statusBar.enabled)
    {
        uiAnimationScheduler_.Cancel(statusBarActivationToken_);
        statusBarActivationToken_ = 0;
        if (systemPanel_) systemPanel_->Hide();
        statusBar_.reset();
        return;
    }
    if (!statusBar_)
    {
        statusBar_ = std::make_unique<snowdesktop::StatusBar>(systemDataProvider_,
            [this](snowdesktop::StatusBarAction action, HWND owner, RECT anchor) {
                // Sample once at the input boundary. Deferred activation must
                // not reinterpret either an ordinary click or a Ctrl click.
                action = snowdesktop::ResolveStatusBarClick(action, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                ActivateStatusBar(action, owner, anchor);
            },
            [this](HMONITOR monitor) { if (systemPanel_) systemPanel_->HideForMonitor(monitor); },
            [this](const std::wstring& error) {
                WriteDiagnosticLogEntry(error.c_str());
                MessageBoxW(hwnd_, error.c_str(), L"SnowDesktop", MB_OK | MB_ICONERROR);
            },
            [this](ID2D1DeviceContext* context, RECT frame, const PersonalizationSettings& appearance, float scale) {
                auto fillAppearance = snowdesktop::StatusBarFillAppearance(appearance);
                DrawWidgetPanelBackground(context, frame, 0,
                    D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                    D2D1::ColorF(0, 0.f), false, 0, &fillAppearance, false, 0, scale);
                 snowdesktop::DrawStatusBarEdge(context, frame, appearance, scale, generalSettings_.statusBar.position);
                brushCache_.clear(); brushCacheContext_ = nullptr;
             });
        statusBar_->SetTrayDragHandlers([this](const auto& changed) {
            if (!settingsController_) return;
            auto settings = settingsController_->Snapshot()->values.general;
            settings.statusBar.pinnedTrayItems = changed.pinnedTrayItems;
            settings.statusBar.trayOrder = changed.trayOrder;
            settingsController_->UpdateGeneral(std::move(settings), snowdesktop::SettingsUpdateMode::PreviewAndCommit);
            uiAnimationScheduler_.ScheduleOnce(0, [this](auto) {
                if (settingsController_) (void)settingsController_->FlushPending();
            });
        }, [this](std::string_view key, POINT screen) {
            return systemPanel_ && systemPanel_->DropTrayIcon(key, screen);
        });
        statusBar_->SetDockChanged([this](bool geometry) {
            if (exitRequested_) return;
            if (geometry) ScheduleDisplayTopologyRefresh();
            else
            {
                UpdatePersistentDockHostVisibility();
                InvalidateDockRects();
            }
        });
        statusBar_->SetSceneProvider([this](HMONITOR monitor) {
            snowdesktop::StatusBarSceneState scene;
            const auto& settings = generalSettings_.statusBar;
            if (!settings.shellUi.enabled && !settings.maximizedWindow.enabled && !settings.visibleWindow.enabled)
                return scene;
            // StartDockForegroundMonitor already owns these observations even
            // when Dock/taskbar styling is disabled. The taskbar's own timer
            // maintains the cache while its controls are active. Otherwise the
            // first bar consumes dirty observations for all monitors together.
            // Never enable the Explorer appearance hook to obtain this state.
            const DWORD now = GetTickCount();
            const bool dirty = systemTaskbarWindowStateChangedTick_.load() != systemTaskbarWindowStateObservedTick_;
            const bool taskbarObserverActive = IsSystemTaskbarHookRequired(dockSettings_);
            if (systemTaskbarWindowScanTick_ == 0 ||
                (!taskbarObserverActive &&
                    ((dirty && now - systemTaskbarWindowScanTick_ >= 250) ||
                        now - systemTaskbarWindowScanTick_ >= 1500)))
            {
                RefreshSystemTaskbarWindowState();
                systemTaskbarWindowScanTick_ = now;
            }
            if (const auto found = systemTaskbarMonitorWindowStates_.find(monitor);
                found != systemTaskbarMonitorWindowStates_.end())
            {
                scene.visibleWindow = found->second.visible;
                scene.maximizedWindow = found->second.maximized;
            }
            scene.shellUi = snowdesktop::dock_settings_rules::ShouldRevealTaskbarForShellPanel(
                taskbarObserverActive && systemTaskbarTaskViewActive_, systemTaskbarShellUiActive_,
                monitor == systemTaskbarShellUiMonitor_);
            if (taskbarObserverActive)
                for (const HWND taskbar : systemTaskbarWindows_)
                    if (IsWindow(taskbar) && snowdesktop::taskbar_monitor::Resolve(taskbar) == monitor &&
                        GetPropW(taskbar, snowdesktop::taskbar_hook::native::kContextMenuProperty))
                    { scene.shellUi = true; break; }
            return scene;
        });
    }
    auto order = BuildMonitorRenderOrder();
    if (order.size() > 1)
    {
        if (generalSettings_.statusBar.monitorScope == DockMonitorScope::First)
            order.erase(order.begin() + 1, order.end());
        else if (generalSettings_.statusBar.monitorScope == DockMonitorScope::Last)
            order.erase(order.begin(), order.end() - 1);
    }
    std::vector<snowdesktop::StatusBarMonitor> monitors;
    auto dockOrder = BuildMonitorRenderOrder();
    if (dockOrder.size() > 1)
    {
        if (dockSettings_.monitorScope == DockMonitorScope::First) dockOrder.resize(1);
        else if (dockSettings_.monitorScope == DockMonitorScope::Last) dockOrder.erase(dockOrder.begin(), dockOrder.end() - 1);
    }
    for (const auto index : order)
    {
        const auto& page = gridPages_[index];
        RECT screen = page.bounds;
        OffsetRect(&screen, virtualLeft_, virtualTop_);
        if (auto monitor = MonitorFromRect(&screen, MONITOR_DEFAULTTONULL))
        {
            int mergedHeight = 0;
            if (generalSettings_.dockEnabled && dockSettings_.edgeAttached &&
                dockSettings_.position == generalSettings_.statusBar.position &&
                std::find(dockOrder.begin(), dockOrder.end(), index) != dockOrder.end())
            {
                const float scale = ClampDockScale(dockSettings_.thicknessScale);
                mergedHeight = std::max(1, static_cast<int>(std::round(GetGridPageItemIconSize(page) * scale))) +
                    2 * std::max(1, static_cast<int>(std::round(kDockSpacing * scale)));
            }
            monitors.push_back({page.monitorId, monitor, mergedHeight});
        }
    }
    statusBar_->Configure(generalSettings_.statusBar, personalizationSettings_, monitors,
        dcompDevice_.Get(), dwriteFactory_.Get(), &collectionPopupAppearance_,
        [this](ID2D1DeviceContext* context, RECT frame, const PersonalizationSettings& appearance, float scale) {
            DrawWidgetPanelBackground(context, frame, appearance.cornerRadius * scale,
                D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha),
                false, 0, &appearance, false, 0, scale);
            brushCache_.clear(); brushCacheContext_ = nullptr;
        });
}
