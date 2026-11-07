#pragma once
#include "dock_page_actions.h"
#include "settings_focus_feedback.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <memory>
#include "runtime_modules_query.h"
#include <memory>

namespace snowdesktop::winui
{
class RuntimeModulesPagePresenter final
{
public:
    RuntimeModulesPagePresenter(std::function<std::wstring(std::string_view)> localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle, DockPageActions actions, RuntimeModulesQuery query);
    ~RuntimeModulesPagePresenter();
    winrt::Microsoft::UI::Xaml::UIElement Content() const;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void Activate();
    void Deactivate();
    winrt::Microsoft::UI::Xaml::FrameworkElement FocusTarget(std::string_view id) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
