#pragma once
#include "../calendar_service.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <functional>
#include <memory>

namespace snowdesktop::winui
{
struct SystemCalendarActions
{
    std::function<std::vector<calendar::CalendarEvent>(const std::string&)> events;
    std::function<void()> manage;
    std::function<std::string()> today;
};
class SystemCalendarView
{
public:
    explicit SystemCalendarView(SystemCalendarActions actions, std::function<void()> layoutChanged = {});
    ~SystemCalendarView();
    winrt::Microsoft::UI::Xaml::UIElement Root() const;
    void Refresh();
    void Close();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
