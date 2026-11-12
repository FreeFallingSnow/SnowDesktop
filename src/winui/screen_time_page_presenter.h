#pragma once

#include "screen_time_page_actions.h"
#include "settings/settings_snapshot.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <winrt/Microsoft.UI.Xaml.h>

namespace snowdesktop::winui
{

class ScreenTimePagePresenter final
{
public:
    using LocalizeCallback = std::function<std::wstring(std::string_view)>;
    using FocusRegistrar = std::function<void(
        std::string, const winrt::Microsoft::UI::Xaml::FrameworkElement&)>;

    ScreenTimePagePresenter(LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~ScreenTimePagePresenter();

    void SetActions(ScreenTimePageActions actions);
    winrt::Microsoft::UI::Xaml::UIElement Content() const;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void SetCollectionState(std::uint64_t generation, bool enabled);
    void RefreshLocalizedText();
    void RegisterFocusTargets(const FocusRegistrar& registrar) const;
    void Activate();
    void Deactivate();
    void Close();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
