#pragma once
#include "../status_bar_settings.h"
#include "../tray_service.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <functional>
#include <memory>

namespace snowdesktop::winui
{
// Internal presentation boundary; the desktop panel supplies the real service,
// while the offline renderer supplies snapshots and records actions only.
struct SystemTrayActions
{
    std::function<bool(const tray::Icon&, const winrt::Microsoft::UI::Xaml::FrameworkElement&, tray::Activation)> activate;
    std::function<void(const StatusBarSettings&)> changed;
    std::function<bool(std::string_view, POINT)> dropOutside;
};
class SystemTrayView
{
public:
    SystemTrayView(SystemTrayActions actions, StatusBarSettings settings, std::function<void()> layoutChanged = {});
    ~SystemTrayView();
    winrt::Microsoft::UI::Xaml::FrameworkElement Root() const;
    double PreferredWidth() const;
    void Refresh(tray::Snapshot snapshot);
    bool Drop(std::string_view key, winrt::Windows::Foundation::Point point);
    void Close();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
