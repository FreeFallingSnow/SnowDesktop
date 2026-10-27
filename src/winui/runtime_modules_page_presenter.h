#pragma once
#include "dock_page_presenter.h"
#include "platform/runtime_module_status.h"
#include <memory>

namespace snowdesktop::winui
{
using RuntimeModulesQuery = std::function<void(std::function<void(runtime_modules::Snapshot)>)>;
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
