#pragma once
#include "general_page_presenter.h"
#include "../calendar_service.h"
#include "../calendar_display.h"

namespace snowdesktop::winui
{
struct CalendarPageActions
{
    std::function<void(std::uint64_t, GeneralPageActions::GeneralEdit)> commitGeneral;
    std::function<std::optional<std::vector<calendar::CalendarEvent>>(std::uint64_t)> events;
    std::function<calendar::MutationResult(std::uint64_t, calendar::CalendarEvent, bool)> mutate;
};
class CalendarPagePresenter final
{
public:
    using LocalizeCallback = std::function<std::wstring(std::string_view)>;
    using FocusRegistrar = std::function<void(std::string, const winrt::Microsoft::UI::Xaml::FrameworkElement&)>;
    CalendarPagePresenter(LocalizeCallback localize, const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~CalendarPagePresenter();
    void SetActions(CalendarPageActions actions);
    winrt::Microsoft::UI::Xaml::UIElement Content() const;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();
    void RegisterFocusTargets(const FocusRegistrar& registrar) const;
    void Activate();
    void Deactivate();
    void Close();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
