#pragma once

#include "../settings_controller.h"
#include "../usage_guide.h"
#include "home_about_page_model.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <memory>

namespace snowdesktop::winui
{
struct StartPageActions
{
    std::function<void(std::uint64_t, usage_guide::Topic)> begin;
    std::function<void(std::uint64_t, bool)> expandedChanged;
    std::function<void(const SettingsRoute&)> navigate;
};
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
