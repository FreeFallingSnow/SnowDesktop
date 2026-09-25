#include "pch.h"
#include "system_calendar_view.h"
#include "../l10n.h"
#include <cstdio>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Globalization.DateTimeFormatting.h>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace f = winrt::Windows::Foundation;
namespace
{
std::string DateString(f::DateTime value)
{
    winrt::Windows::Globalization::Calendar local;
    local.ChangeCalendarSystem(L"GregorianCalendar"); local.SetDateTime(value);
    char result[16]{};
    std::snprintf(result, sizeof(result), "%04d-%02d-%02d", local.Year(), local.Month(), local.Day());
    return result;
}
f::DateTime PickerDate(const std::string& value)
{
    const auto date = calendar::CalendarService::GetDateInfo(value);
    if (!date) return winrt::clock::now();
    winrt::Windows::Globalization::Calendar local;
    local.ChangeCalendarSystem(L"GregorianCalendar"); local.Day(1);
    local.Year(date->year); local.Month(date->month); local.Day(date->day);
    local.Hour(12); local.Minute(0); local.Second(0); local.Nanosecond(0);
    return local.GetDateTime();
}
winrt::hstring DateLabel(const std::string& value, const wchar_t* pattern)
{
    const auto languages = winrt::single_threaded_vector<winrt::hstring>(
        {winrt::to_hstring(Locale::Instance().GetEffectiveLanguage())});
    return winrt::Windows::Globalization::DateTimeFormatting::DateTimeFormatter(pattern, languages).Format(PickerDate(value));
}
c::TextBlock Text(const winrt::hstring& value, double size = 14)
{
    c::TextBlock text; text.Text(value); text.FontSize(size); text.TextWrapping(x::TextWrapping::Wrap); return text;
}
}
struct SystemCalendarView::Impl : std::enable_shared_from_this<Impl>
{
    SystemCalendarActions actions;
    std::function<void()> layoutChanged;
    c::StackPanel root, agenda;
    c::CalendarView month;
    c::TextBlock dateHeading, weekday, dayNumber, monthYear;
    std::string today, selected;
    std::vector<calendar::CalendarEvent> events;
    bool closed = false;
    explicit Impl(SystemCalendarActions callbacks, std::function<void()> onLayout)
        : actions(std::move(callbacks)), layoutChanged(std::move(onLayout)) {}
    void Initialize()
    {
        today = actions.today ? actions.today() : calendar::CalendarService::CurrentLocalNow().date;
        selected = today;
        root.Spacing(12); agenda.Spacing(8);
        c::Grid dates; dates.ColumnSpacing(16);
        c::ColumnDefinition summary; summary.Width(x::GridLengthHelper::FromPixels(104));
        c::ColumnDefinition calendar; calendar.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
        dates.ColumnDefinitions().Append(summary); dates.ColumnDefinitions().Append(calendar);
        c::StackPanel current; current.Spacing(4); current.VerticalAlignment(x::VerticalAlignment::Center);
        weekday.FontSize(14); weekday.TextAlignment(x::TextAlignment::Center);
        dayNumber.FontSize(52); dayNumber.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        dayNumber.TextAlignment(x::TextAlignment::Center);
        monthYear.FontSize(12); monthYear.Opacity(.7); monthYear.TextAlignment(x::TextAlignment::Center);
        monthYear.TextWrapping(x::TextWrapping::Wrap);
        current.Children().Append(weekday); current.Children().Append(dayNumber); current.Children().Append(monthYear);
        c::Button backToToday; backToToday.Content(winrt::box_value(_LW("app.widget.date_picker.today")));
        backToToday.HorizontalAlignment(x::HorizontalAlignment::Center); backToToday.Margin({0, 12, 0, 0});
        current.Children().Append(backToToday); dates.Children().Append(current);
        month.Language(winrt::to_hstring(Locale::Instance().GetEffectiveLanguage()));
        month.CalendarIdentifier(L"GregorianCalendar"); month.SelectionMode(c::CalendarViewSelectionMode::Single);
        month.HorizontalAlignment(x::HorizontalAlignment::Stretch); month.MinWidth(280); month.MinHeight(260); month.Height(280);
        month.NumberOfWeeksInView(6);
        month.SetDisplayDate(PickerDate(selected)); month.SelectedDates().Append(PickerDate(selected));
        c::Grid::SetColumn(month, 1); dates.Children().Append(month); root.Children().Append(dates);
        c::Grid toolbar; toolbar.ColumnSpacing(12);
        c::ColumnDefinition main; main.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
        c::ColumnDefinition tail; tail.Width(x::GridLengthHelper::Auto());
        toolbar.ColumnDefinitions().Append(main); toolbar.ColumnDefinitions().Append(tail);
        dateHeading.FontSize(14); dateHeading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        dateHeading.TextTrimming(x::TextTrimming::CharacterEllipsis);
        dateHeading.VerticalAlignment(x::VerticalAlignment::Center); toolbar.Children().Append(dateHeading);
        c::Button manage; manage.Content(winrt::box_value(_LW("statusBar.manageCalendar")));
        manage.IsEnabled(static_cast<bool>(actions.manage)); c::Grid::SetColumn(manage, 1);
        toolbar.Children().Append(manage); root.Children().Append(toolbar);
        c::ScrollViewer scroll; scroll.MaxHeight(144); scroll.Content(agenda);
        scroll.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled); root.Children().Append(scroll);
        const auto weak = weak_from_this();
        month.SelectedDatesChanged([weak](const auto&, const auto&) {
            if (auto self = weak.lock(); self && !self->closed && self->month.SelectedDates().Size())
            {
                self->selected = DateString(self->month.SelectedDates().GetAt(0)); self->Refresh(true);
            }
        });
        backToToday.Click([weak](const auto&, const auto&) {
            if (auto self = weak.lock(); self && !self->closed)
            {
                self->month.SetDisplayDate(PickerDate(self->today));
                self->month.SelectedDates().Clear(); self->month.SelectedDates().Append(PickerDate(self->today));
            }
        });
        manage.Click([weak](const auto&, const auto&) {
            if (auto self = weak.lock(); self && !self->closed && self->actions.manage)
            {
                const auto callback = self->actions.manage;
                callback(); // Retain self if the action closes and releases the popup.
            }
        });
        Refresh(true);
    }
    void Refresh(bool force = false)
    {
        if (closed) return;
        const auto current = actions.today ? actions.today() : calendar::CalendarService::CurrentLocalNow().date;
        if (force || current != today)
        {
            today = current;
            weekday.Text(DateLabel(today, L"dayofweek.full"));
            monthYear.Text(DateLabel(today, L"month.full year"));
            if (const auto info = calendar::CalendarService::GetDateInfo(today)) dayNumber.Text(winrt::to_hstring(info->day));
        }
        auto next = actions.events ? actions.events(selected) : std::vector<calendar::CalendarEvent>{};
        if (!force && next.size() == events.size() && std::equal(next.begin(), next.end(), events.begin(),
            [](const auto& first, const auto& second) { return first.id == second.id && first.revision == second.revision; })) return;
        events = std::move(next); agenda.Children().Clear(); dateHeading.Text(DateLabel(selected, L"month.full day"));
        if (events.empty())
        {
            auto empty = Text(_LW("settings.calendar.empty")); empty.Opacity(.65); empty.Margin({0, 8, 0, 8});
            agenda.Children().Append(empty);
        }
        for (const auto& event : events)
        {
            c::Grid row; row.ColumnSpacing(12); row.Padding({0, 6, 0, 6});
            c::ColumnDefinition time; time.Width(x::GridLengthHelper::FromPixels(80));
            c::ColumnDefinition description; description.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
            row.ColumnDefinitions().Append(time); row.ColumnDefinitions().Append(description);
            wchar_t range[32]{};
            swprintf_s(range, L"%02d:%02d\n%02d:%02d", event.startMinutes / 60, event.startMinutes % 60, event.endMinutes / 60, event.endMinutes % 60);
            auto label = Text(event.allDay ? _LW("settings.calendar.allDay") : range, 12); label.Opacity(.7);
            auto title = Text(winrt::to_hstring(event.title)); title.MaxLines(2); title.TextTrimming(x::TextTrimming::CharacterEllipsis);
            title.VerticalAlignment(x::VerticalAlignment::Center); c::Grid::SetColumn(title, 1);
            c::ToolTipService::SetToolTip(title, winrt::box_value(winrt::to_hstring(event.title)));
            row.Children().Append(label); row.Children().Append(title); agenda.Children().Append(row);
        }
        if (layoutChanged) layoutChanged();
    }
    void Close() { closed = true; actions = {}; layoutChanged = {}; }
};
SystemCalendarView::SystemCalendarView(SystemCalendarActions actions, std::function<void()> layoutChanged)
    : impl_(std::make_shared<Impl>(std::move(actions), std::move(layoutChanged))) { impl_->Initialize(); }
SystemCalendarView::~SystemCalendarView() { Close(); }
x::UIElement SystemCalendarView::Root() const { return impl_->root; }
void SystemCalendarView::Refresh() { impl_->Refresh(); }
void SystemCalendarView::Close() { impl_->Close(); }
}
