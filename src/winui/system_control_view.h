#pragma once
#include "../system_controls.h"
#include "../status_bar.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <memory>

namespace snowdesktop::widget_runtime { class WidgetSystemDataProvider; }
namespace snowdesktop::winui
{
class SystemControlView
{
public:
    SystemControlView(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data,
        const StatusBarSettings& settings, StatusBarAction initial = StatusBarAction::ControlCenter);
    ~SystemControlView();
    winrt::Microsoft::UI::Xaml::FrameworkElement Root() const;
    void Refresh();
    void Close();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
