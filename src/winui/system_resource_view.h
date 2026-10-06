#pragma once
#include "../status_bar.h"
#include "../widget_system_data_provider.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <functional>
#include <memory>

namespace snowdesktop::winui
{
struct SystemResourceSource
{
    std::function<std::optional<widget_runtime::WidgetCpuDataSnapshot>()> cpu;
    std::function<std::optional<widget_runtime::WidgetMemoryDataSnapshot>()> memory;
    std::function<std::optional<widget_runtime::WidgetGpuDataSnapshot>()> gpu;
    std::function<std::optional<widget_runtime::WidgetNetworkTrafficDataSnapshot>()> traffic;
    std::function<std::vector<widget_runtime::WidgetResourcePoint>(std::string_view, std::string_view)> history;
    std::function<void(std::string_view)> subscribe;
    std::function<void()> close;
    std::function<std::int64_t()> now;
};
bool IsSystemResourceAction(StatusBarAction action);
class SystemResourceView
{
public:
    SystemResourceView(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data, StatusBarAction action,
        std::function<void()> layoutChanged = {});
    SystemResourceView(SystemResourceSource source, StatusBarAction action, std::function<void()> layoutChanged = {});
    ~SystemResourceView();
    winrt::Microsoft::UI::Xaml::FrameworkElement Root() const;
    void Refresh();
    void Close();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
