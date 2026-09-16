#pragma once

#include "home_about_page_presenter.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <memory>

namespace snowdesktop::winui
{
class StartPagePresenter final
{
public:
    StartPagePresenter(HomeAboutPagePresenter::LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& navigationCardStyle);
    ~StartPagePresenter();
    void SetActions(HomeAboutPageActions actions);
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void ApplyStatusPatch(const HomeAboutStatusPatch& patch);
    void RefreshLocalizedText();
    void Activate(bool active);
    void SelectRoute(std::string_view id);
    void Close();
    winrt::Microsoft::UI::Xaml::UIElement Content() const;
    winrt::Microsoft::UI::Xaml::FrameworkElement FocusTarget(std::string_view id);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
