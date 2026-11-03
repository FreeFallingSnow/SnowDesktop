#pragma once

#include "settings/settings_fwd.h"
#include "dock/dock_fullscreen_types.h"
#include <string>
#include <string_view>
#include <vector>
#include <winrt/Microsoft.UI.Xaml.h>
#include <functional>
#include <memory>

namespace snowdesktop::winui
{
class DockTraySettingsEditor final
{
public:
    using Localize = std::function<std::wstring(std::string_view, std::wstring_view)>;
    using Edit = std::function<void(std::function<void(DockSettings&)>)>;
    using Query = std::function<std::vector<DockFullscreenApplication>()>;
    DockTraySettingsEditor(Localize localize, Edit edit, Query query);
    ~DockTraySettingsEditor();
    winrt::Microsoft::UI::Xaml::UIElement Content() const;
    winrt::Microsoft::UI::Xaml::FrameworkElement FocusTarget();
    void Update(const DockSettings& settings);
    void RefreshText();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
