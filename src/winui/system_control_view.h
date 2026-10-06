#pragma once
#include "../system_controls.h"
#include "../status_bar.h"
#include "../widget_system_data_provider.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <memory>

namespace snowdesktop::widget_runtime { class WidgetSystemDataProvider; }
namespace snowdesktop::winui
{
inline constexpr double SystemControlViewportHeight = 540;
// Internal device boundary. The production adapter uses the application-owned
// provider; offline rendering supplies fixed state and never touches hardware.
struct SystemControlViewSource
{
    std::function<std::optional<system_control::Snapshot>(std::string_view)> current;
    std::function<std::uint64_t(system_control::Request)> start;
    std::function<std::vector<system_control::Completion>()> completions;
    std::function<void(std::string, std::chrono::milliseconds)> subscribe;
    std::function<void(std::string_view)> unsubscribe;
    std::function<void()> close;
    std::function<std::optional<widget_runtime::WidgetMediaSessionsDataSnapshot>()> media;
    std::function<std::optional<widget_runtime::WidgetMediaArtworkDataSnapshot>()> artwork;
    std::function<void(const wchar_t*)> settings;
};
class SystemControlView
{
public:
    SystemControlView(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data,
        const StatusBarSettings& settings, StatusBarAction initial = StatusBarAction::ControlCenter,
        std::function<void()> layoutChanged = {});
    SystemControlView(SystemControlViewSource source, const StatusBarSettings& settings,
        StatusBarAction initial = StatusBarAction::ControlCenter, std::function<void()> layoutChanged = {});
    ~SystemControlView();
    winrt::Microsoft::UI::Xaml::FrameworkElement Root() const;
    void ApplyAppearance(const PersonalizationSettings& appearance);
    std::vector<RECT> CardBounds(double scale) const;
    void Refresh();
    void Select(std::string_view section);
    void SetViewportHeight(double height);
    void Close();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
