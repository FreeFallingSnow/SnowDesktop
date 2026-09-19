#pragma once
#include "personalization_page_presenter.h"

namespace snowdesktop::winui
{
class ContextMenuPagePresenter final
{
  public:
    using LocalizeCallback = std::function<std::wstring(std::string_view)>;
    using FocusRegistrar =
        std::function<void(std::string, const winrt::Microsoft::UI::Xaml::FrameworkElement &)>;
    ContextMenuPagePresenter(LocalizeCallback, const winrt::Microsoft::UI::Xaml::Style &);
    ~ContextMenuPagePresenter();
    void SetActions(PersonalizationPageActions);
    winrt::Microsoft::UI::Xaml::UIElement Content() const;
    void ApplySnapshot(const SettingsSnapshot &);
    void RefreshLocalizedText();
    void RegisterFocusTargets(const FocusRegistrar &) const;
    void Activate();
    void Deactivate();
    void Close();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace snowdesktop::winui
