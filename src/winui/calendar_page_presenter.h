#pragma once
#include "calendar_page_actions.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <memory>
#include "system/calendar/calendar_service.h"
#include "system/calendar/calendar_display.h"

namespace snowdesktop::winui
{
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
