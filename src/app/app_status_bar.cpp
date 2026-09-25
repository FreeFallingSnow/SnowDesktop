#include "app.h"

void DesktopApp::SyncStatusBar()
{
    if (!systemDataProvider_ || !dcompDevice_ || !dwriteFactory_) return;
    if (!generalSettings_.statusBar.enabled)
    {
        if (systemPanel_) systemPanel_->Hide();
        statusBar_.reset();
        return;
    }
    if (!statusBar_)
    {
        statusBar_ = std::make_unique<snowdesktop::StatusBar>(systemDataProvider_,
            [this](snowdesktop::StatusBarAction action, HWND owner, RECT anchor) {
                using Action = snowdesktop::StatusBarAction;
                if (action == Action::Menu)
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
                        });
                    systemPanel_->Show(action, owner, anchor, collectionPopupAppearance_, generalSettings_.statusBar,
                        statusBar_->Tray(), systemDataProvider_);
                }
            },
            [this](HMONITOR monitor) { if (systemPanel_) systemPanel_->HideForMonitor(monitor); },
            [this](const std::wstring& error) {
                WriteDiagnosticLogEntry(error.c_str());
                MessageBoxW(hwnd_, error.c_str(), L"SnowDesktop", MB_OK | MB_ICONERROR);
            },
            [this](ID2D1DeviceContext* context, RECT frame, const PersonalizationSettings& appearance, float scale) {
                DrawWidgetPanelBackground(context, frame, 0,
                    D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                    D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha),
                    false, appearance.widgetBorderWidth * scale, &appearance, false, 0, scale);
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
