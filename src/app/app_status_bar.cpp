#include "app.h"

void DesktopApp::SyncStatusBar()
{
    if (!systemDataProvider_ || !dcompDevice_ || !dwriteFactory_) return;
    if (!generalSettings_.statusBar.enabled)
    {
        statusBar_.reset();
        return;
    }
    if (!statusBar_)
    {
        statusBar_ = std::make_unique<snowdesktop::StatusBar>(systemDataProvider_,
            [this](snowdesktop::StatusBarAction action, HWND, RECT) {
                using Action = snowdesktop::StatusBarAction;
                if (action == Action::Settings)
                    ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::StatusBar));
            },
            [](HMONITOR) {},
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
