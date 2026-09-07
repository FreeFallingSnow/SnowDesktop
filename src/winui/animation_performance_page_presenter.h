#pragma once

#include "dock_page_presenter.h"

namespace snowdesktop::winui
{

/** Host animation preferences; each edit patches the latest domain value. */
class AnimationPerformancePagePresenter final
{
public:
    using LocalizeCallback = std::function<std::wstring(std::string_view)>;
    using NavigateCallback = std::function<void(const SettingsRoute&)>;
    using FocusRegistrar = std::function<void(std::string,
        const winrt::Microsoft::UI::Xaml::FrameworkElement&)>;

    AnimationPerformancePagePresenter(LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle,
        NavigateCallback navigate);
    ~AnimationPerformancePagePresenter();
    void SetActions(DockPageActions actions);
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement Content() const noexcept;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();
    void RegisterFocusTargets(const FocusRegistrar& registrar) const;
    void Activate() noexcept;
    void Deactivate() noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
