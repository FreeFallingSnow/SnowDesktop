#include "app.h"
#include "system_controls.h"
#include "modern_menu.h"
#include "../status_bar_view.h"

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
    const bool systemControls = action == Action::ControlCenter && (GetKeyState(VK_CONTROL) & 0x8000);
    if (action == Action::Notifications || systemControls)
    {
        if (systemPanel_) systemPanel_->Hide();
        CloseQuickNavigation();
        INPUT input[4]{};
        for (auto& item : input) item.type = INPUT_KEYBOARD;
        const WORD key = systemControls || IsClassicSystemTaskbar() ? 'A' : 'N';
        input[0].ki.wVk = VK_LWIN; input[1].ki.wVk = key;
        input[2].ki.wVk = key; input[2].ki.dwFlags = KEYEVENTF_KEYUP;
        input[3].ki.wVk = VK_LWIN; input[3].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(4, input, sizeof(INPUT)) != 4) MessageBeep(MB_ICONWARNING);
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
            if (command >= 6 && MessageBoxW(owner, _LW(command == 6 ? "controlCenter.confirmRestart" : "controlCenter.confirmShutdown"),
                    _LW(labels[command - 1]), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
            snowdesktop::system_control::Request request;
            const char* tasks[]{"system.power.lock", "system.power.sleep", "system.power.restart", "system.power.shutdown"};
            request.name = tasks[command - 4]; request.hostConfirmed = command >= 6;
            if (!systemDataProvider_->Controls()->Start("statusBarVolume", std::move(request))) MessageBeep(MB_ICONWARNING);
        }
    }
    else if (action == Action::Menu)
    {
        if (systemPanel_) systemPanel_->Hide();
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, 1, _LW("statusBar.menu.settings"));
        AppendMenuW(menu, MF_STRING, 2, _LW("settings.personalization.theme"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (generalSettings_.statusBar.position == DockPosition::Top ? MF_CHECKED : 0), 3, _LW("app.dock.top"));
        AppendMenuW(menu, MF_STRING | (generalSettings_.statusBar.position == DockPosition::Bottom ? MF_CHECKED : 0), 4, _LW("app.dock.bottom"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 5, _LW("statusBar.menu.hide"));
        const UINT command = ShowModernMenu(menu, {anchor.left, anchor.bottom}, owner);
        DestroyMenu(menu);
        if (command == 1) ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::StatusBar));
        else if (command == 2) ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::AppearanceTheme, "personalization.statusBarTheme"));
        else if (command >= 3 && command <= 5 && settingsController_)
        {
            auto settings = settingsController_->Snapshot()->values.general;
            if (command == 5) settings.statusBar.enabled = false;
            else settings.statusBar.position = command == 3 ? DockPosition::Top : DockPosition::Bottom;
            // Preview updates are queued by the controller; the menu
            // owner must not be destroyed inside its input callback.
            settingsController_->UpdateGeneral(std::move(settings), snowdesktop::SettingsUpdateMode::PreviewAndCommit);
            uiAnimationScheduler_.ScheduleOnce(0, [this](auto) {
                if (settingsController_) (void)settingsController_->FlushPending();
            });
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
            systemPanel_ = std::make_unique<snowdesktop::winui::SystemPanel>([this](const auto& changed) {
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
            }, snowdesktop::winui::SystemCalendarActions{
                [this](const std::string& date) {
                    return widgetEngine_ ? widgetEngine_->RuntimeCalendarEvents(date, date) :
                        std::vector<snowdesktop::calendar::CalendarEvent>{};
                },
                [this] {
                    if (systemPanel_) systemPanel_->Hide();
                    ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Calendar, "calendar.events"));
                }, {}});
        systemPanel_->Show(action, owner, anchor, collectionPopupAppearance_, generalSettings_.statusBar,
            statusBar_->Tray(), systemDataProvider_);
    }
}

void DesktopApp::SyncStatusBar()
{
    if (!systemDataProvider_ || !dcompDevice_ || !dwriteFactory_) return;
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
    for (const auto index : order)
    {
        const auto& page = gridPages_[index];
        RECT screen = page.bounds;
        OffsetRect(&screen, virtualLeft_, virtualTop_);
        if (auto monitor = MonitorFromRect(&screen, MONITOR_DEFAULTTONULL))
            monitors.push_back({page.monitorId, monitor});
    }
    statusBar_->Configure(generalSettings_.statusBar, personalizationSettings_, monitors,
        dcompDevice_.Get(), dwriteFactory_.Get());
}
