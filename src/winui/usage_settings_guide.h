#pragma once
#include "../settings_search_index.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <functional>
#include <memory>

namespace snowdesktop::winui
{
// Private reference index. It reads the host's visible settings catalogue and
// can only navigate; it has no settings-update or desktop-practice action.
class UsageSettingsGuide final
{
public:
    using Localize = std::function<std::wstring(std::string_view)>;
    explicit UsageSettingsGuide(Localize localize);
    ~UsageSettingsGuide();
    void SetActions(std::function<std::vector<StaticSettingSearchDescriptor>()> source,
        std::function<void(const SettingsRoute&)> navigate);
    void Refresh();
    void SelectRoute(const SettingsRoute& route);
    void Resize(double width, double height);
    void Close();
    winrt::Microsoft::UI::Xaml::UIElement Content() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
