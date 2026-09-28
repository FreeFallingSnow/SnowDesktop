#pragma once
#include "dock_page_presenter.h"

namespace snowdesktop::winui
{
class StatusBarPagePresenter
{
public:
    StatusBarPagePresenter(DockPagePresenter::LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle,
        std::function<void(SettingsRoute)> navigate = {});
    ~StatusBarPagePresenter();
    void SetActions(DockPageActions actions);
    winrt::Microsoft::UI::Xaml::UIElement Content() const;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();
    void Activate(std::string_view focusId = {});
    void Deactivate();
    void Close();
    void RegisterFocusTargets(const std::function<void(std::string,
        const winrt::Microsoft::UI::Xaml::FrameworkElement&)>& registerTarget) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
