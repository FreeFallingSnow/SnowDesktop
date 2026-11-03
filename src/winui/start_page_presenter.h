#pragma once

#include "start_page_actions.h"
#include "home_about_page_model.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <memory>

namespace snowdesktop::winui
{
class StartPagePresenter final
{
public:
    using LocalizeCallback = std::function<std::wstring(std::string_view)>;
    StartPagePresenter(LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle,
        const winrt::Microsoft::UI::Xaml::Style& navigationCardStyle);
    ~StartPagePresenter();
    void SetActions(StartPageActions actions);
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void ApplyStatusPatch(const HomeAboutStatusPatch& patch);
    void RefreshLocalizedText();
    void Activate(bool active);
    void SelectRoute(std::string_view id);
    void Close();
    winrt::Microsoft::UI::Xaml::UIElement Content() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
