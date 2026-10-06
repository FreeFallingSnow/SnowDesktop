#pragma once

#include "dock_page_presenter.h"

namespace snowdesktop::winui
{

/** Shared settings editors and a compact layout-preset selector. */
class DesktopStylePagePresenter final
{
public:
    using LocalizeCallback = DockPagePresenter::LocalizeCallback;
    using ApplyPresetCallback = std::function<void(std::string, DockPosition, bool)>;

    DesktopStylePagePresenter(LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle,
        ApplyPresetCallback applyPreset);
    ~DesktopStylePagePresenter();
    DesktopStylePagePresenter(const DesktopStylePagePresenter&) = delete;
    DesktopStylePagePresenter& operator=(const DesktopStylePagePresenter&) = delete;

    void SetActions(DockPageActions actions);
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement Content() const;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    [[nodiscard]] bool NeedsRecommendedAnimations(std::string_view preset) const;
    void RefreshLocalizedText();
    /** A preset focus ID selects its preview without applying any settings. */
    void Activate(std::string_view focusId = {});
    void Deactivate();
    void Close() noexcept;
    void RegisterFocusTargets(const std::function<void(std::string,
        const winrt::Microsoft::UI::Xaml::FrameworkElement&)>& registerTarget) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
