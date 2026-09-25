#include "pch.h"
#include "system_calendar_view.h"
#include "../l10n.h"
#include <array>
#include <cstdio>
#include <winrt/Windows.Globalization.h>

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
c::TextBlock Text(const winrt::hstring& value)
{
    c::TextBlock text; text.Text(value); text.TextWrapping(x::TextWrapping::Wrap); return text;
}
}
struct SystemCalendarView::Impl : std::enable_shared_from_this<Impl>
{
    SystemCalendarActions actions;
    c::StackPanel root, agenda;
    c::CalendarView month;
    c::TextBlock dateHeading, error;
    c::ContentDialog dialog{nullptr};
    std::string selected = calendar::CalendarService::CurrentLocalNow().date;
    std::vector<calendar::CalendarEvent> events;
    bool closed = false;
    explicit Impl(SystemCalendarActions callbacks) : actions(std::move(callbacks)) {}
    void Initialize()
    {
        root.Spacing(12); agenda.Spacing(4);
        month.SelectionMode(c::CalendarViewSelectionMode::Single);
        month.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        month.SetDisplayDate(PickerDate(selected)); month.SelectedDates().Append(PickerDate(selected));
        root.Children().Append(month);
        c::Grid toolbar;
        c::ColumnDefinition main; main.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
        c::ColumnDefinition tail; tail.Width(x::GridLengthHelper::Auto());
        toolbar.ColumnDefinitions().Append(main); toolbar.ColumnDefinitions().Append(tail);
        dateHeading.FontSize(14); dateHeading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        dateHeading.VerticalAlignment(x::VerticalAlignment::Center); toolbar.Children().Append(dateHeading);
        c::Button add; add.Content(c::SymbolIcon(c::Symbol::Add)); c::Grid::SetColumn(add, 1);
        x::Automation::AutomationProperties::SetName(add, _LW("settings.calendar.add"));
        c::ToolTipService::SetToolTip(add, winrt::box_value(_LW("settings.calendar.add")));
        toolbar.Children().Append(add); root.Children().Append(toolbar);
        c::ScrollViewer scroll; scroll.MaxHeight(176); scroll.Content(agenda);
        scroll.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled); root.Children().Append(scroll);
        error.TextWrapping(x::TextWrapping::Wrap); root.Children().Append(error);
        const auto weak = weak_from_this();
        month.SelectedDatesChanged([weak](const auto&, const auto&) {
            if (auto self = weak.lock(); self && !self->closed && self->month.SelectedDates().Size())
            {
                self->selected = DateString(self->month.SelectedDates().GetAt(0)); self->Refresh(true);
            }
        });
        add.Click([weak](const auto&, const auto&) {
            if (auto self = weak.lock(); self && !self->closed)
            {
                calendar::CalendarEvent event; event.date = self->selected; event.allDay = true;
                self->Edit(std::move(event));
            }
        });
        Refresh(true);
    }
    void Refresh(bool force = false)
    {
        if (closed || !actions.events) return;
        auto next = actions.events(selected);
        if (!force && next.size() == events.size() && std::equal(next.begin(), next.end(), events.begin(),
            [](const auto& first, const auto& second) { return first.id == second.id && first.revision == second.revision; })) return;
        events = std::move(next); agenda.Children().Clear(); dateHeading.Text(winrt::to_hstring(selected));
        if (events.empty()) agenda.Children().Append(Text(_LW("settings.calendar.empty")));
        const auto weak = weak_from_this();
        for (const auto& event : events)
        {
            c::Grid row; row.ColumnSpacing(8);
            c::ColumnDefinition main; main.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
            c::ColumnDefinition tail; tail.Width(x::GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(main); row.ColumnDefinitions().Append(tail);
            c::Button edit; edit.HorizontalAlignment(x::HorizontalAlignment::Stretch); edit.HorizontalContentAlignment(x::HorizontalAlignment::Left);
            c::StackPanel label; label.Spacing(2); label.Children().Append(Text(winrt::to_hstring(event.title)));
            wchar_t time[32]{};
            swprintf_s(time, L"%02d:%02d – %02d:%02d", event.startMinutes / 60, event.startMinutes % 60, event.endMinutes / 60, event.endMinutes % 60);
            auto detail = Text(event.allDay ? _LW("settings.calendar.allDay") : time); detail.FontSize(12); detail.Opacity(.7);
            label.Children().Append(detail); edit.Content(label);
            edit.Click([weak, event](const auto&, const auto&) { if (auto self = weak.lock(); self && !self->closed) self->Edit(event); });
            row.Children().Append(edit);
            c::Button remove; remove.Content(c::SymbolIcon(c::Symbol::Delete)); c::Grid::SetColumn(remove, 1);
            remove.VerticalAlignment(x::VerticalAlignment::Center);
            x::Automation::AutomationProperties::SetName(remove, std::wstring(_LW("app.settings.delete")) + L" " + winrt::to_hstring(event.title).c_str());
            remove.Click([weak, event](const auto&, const auto&) { if (auto self = weak.lock(); self && !self->closed) self->Remove(event); });
            row.Children().Append(remove); agenda.Children().Append(row);
        }
    }
    winrt::fire_and_forget Edit(calendar::CalendarEvent event)
    {
        const auto lifetime = shared_from_this();
        if (closed || dialog || !actions.mutate) co_return;
        try
        {
            c::StackPanel fields; fields.Spacing(10);
            c::TextBox title, notes; title.MaxLength(512); notes.MaxLength(8192);
            title.Header(winrt::box_value(_LW("settings.calendar.title"))); title.Text(winrt::to_hstring(event.title));
            notes.Header(winrt::box_value(_LW("settings.calendar.notes"))); notes.Text(winrt::to_hstring(event.notes));
            notes.AcceptsReturn(true); notes.TextWrapping(x::TextWrapping::Wrap); notes.MaxHeight(100);
            c::CalendarDatePicker date; date.CalendarIdentifier(L"GregorianCalendar");
            date.Header(winrt::box_value(_LW("settings.calendar.date")));
            const auto eventDate = PickerDate(event.date);
            if (eventDate < date.MinDate()) date.MinDate(eventDate);
            if (eventDate > date.MaxDate()) date.MaxDate(eventDate);
            date.Date(winrt::box_value(eventDate).as<f::IReference<f::DateTime>>());
            c::ToggleSwitch allDay; allDay.Header(winrt::box_value(_LW("settings.calendar.allDay"))); allDay.IsOn(event.allDay);
            c::TimePicker start, end; start.Header(winrt::box_value(_LW("settings.calendar.start"))); end.Header(winrt::box_value(_LW("settings.calendar.end")));
            start.Time(std::chrono::minutes(event.startMinutes)); end.Time(std::chrono::minutes(event.endMinutes));
            start.MinuteIncrement(1); end.MinuteIncrement(1);
            const auto updateTime = [allDay, start, end] {
                const auto visibility = allDay.IsOn() ? x::Visibility::Collapsed : x::Visibility::Visible;
                start.Visibility(visibility); end.Visibility(visibility);
            };
            allDay.Toggled([updateTime](const auto&, const auto&) { updateTime(); }); updateTime();
            c::ComboBox reminder; reminder.Header(winrt::box_value(_LW("settings.calendar.reminder")));
            constexpr std::array<int, 7> reminders{-1, 0, 5, 15, 30, 60, 1440};
            for (std::size_t i = 0; i < reminders.size(); ++i)
            {
                reminder.Items().Append(winrt::box_value(_LW(("settings.calendar.reminder." + std::to_string(reminders[i])).c_str())));
                if (event.reminderMinutes == reminders[i]) reminder.SelectedIndex(static_cast<int>(i));
            }
            for (x::UIElement field : {title.as<x::UIElement>(), date.as<x::UIElement>(), allDay.as<x::UIElement>(), start.as<x::UIElement>(), end.as<x::UIElement>(), reminder.as<x::UIElement>(), notes.as<x::UIElement>()}) fields.Children().Append(field);
            auto validation = Text(L""); fields.Children().Append(validation);
            c::ScrollViewer scroll; scroll.Content(fields); scroll.MaxHeight(400);
            scroll.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled);
            c::ContentDialog editor; dialog = editor; editor.XamlRoot(root.XamlRoot());
            editor.Title(winrt::box_value(_LW(event.id.empty() ? "settings.calendar.add" : "settings.calendar.events")));
            editor.Content(scroll); editor.PrimaryButtonText(_LW("settings.calendar.save")); editor.CloseButtonText(_LW("app.settings.cancel"));
            editor.DefaultButton(c::ContentDialogButton::Primary);
            const auto weak = weak_from_this();
            editor.PrimaryButtonClick([weak, event, title, date, allDay, start, end, reminder, notes, validation, reminders](const auto&, const c::ContentDialogButtonClickEventArgs& args) mutable {
                const auto self = weak.lock();
                if (!self || self->closed || !self->actions.mutate) { args.Cancel(true); return; }
                event.title = winrt::to_string(title.Text()); event.notes = winrt::to_string(notes.Text());
                event.date = date.Date() ? DateString(date.Date().Value()) : std::string{};
                event.allDay = allDay.IsOn();
                const auto minutes = [](const c::TimePicker& picker) { return picker.SelectedTime() ? static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(picker.SelectedTime().Value()).count()) : -1; };
                event.startMinutes = event.allDay ? 0 : minutes(start); event.endMinutes = event.allDay ? 0 : minutes(end);
                event.reminderMinutes = reminder.SelectedIndex() >= 0 ? reminders[static_cast<std::size_t>(reminder.SelectedIndex())] : -1;
                const auto result = self->actions.mutate(event, false);
                if (!result.ok)
                {
                    const bool invalid = result.error.starts_with("invalid_") || result.error == "title_required" || result.error == "text_too_long";
                    args.Cancel(true); validation.Text(_LW(result.error == "conflict" ? "settings.calendar.conflict" : invalid ? "settings.calendar.invalid" : "settings.calendar.failed"));
                }
            });
            co_await editor.ShowAsync();
            if (dialog == editor) dialog = nullptr;
            if (!closed) Refresh(true);
        }
        catch (...) { dialog = nullptr; if (!closed) error.Text(_LW("settings.calendar.failed")); }
    }
    winrt::fire_and_forget Remove(calendar::CalendarEvent event)
    {
        const auto lifetime = shared_from_this();
        if (closed || dialog || !actions.mutate) co_return;
        try
        {
            c::ContentDialog confirmation; dialog = confirmation;
            confirmation.XamlRoot(root.XamlRoot()); confirmation.Title(winrt::box_value(_LW("settings.calendar.confirmDelete")));
            confirmation.Content(winrt::box_value(winrt::to_hstring(event.date + "  " + event.title)));
            confirmation.PrimaryButtonText(_LW("app.settings.delete")); confirmation.CloseButtonText(_LW("app.settings.cancel"));
            confirmation.DefaultButton(c::ContentDialogButton::Close);
            const auto answer = co_await confirmation.ShowAsync();
            if (dialog == confirmation) dialog = nullptr;
            if (closed || !actions.mutate || answer != c::ContentDialogResult::Primary) co_return;
            const auto result = actions.mutate(event, true);
            error.Text(result.ok ? L"" : _LW(result.error == "conflict" ? "settings.calendar.conflict" : "settings.calendar.failed"));
            Refresh(true);
        }
        catch (...) { dialog = nullptr; if (!closed) error.Text(_LW("settings.calendar.failed")); }
    }
    void Close()
    {
        closed = true; actions = {}; if (dialog) dialog.Hide();
    }
};
SystemCalendarView::SystemCalendarView(SystemCalendarActions actions) : impl_(std::make_shared<Impl>(std::move(actions))) { impl_->Initialize(); }
SystemCalendarView::~SystemCalendarView() { Close(); }
x::UIElement SystemCalendarView::Root() const { return impl_->root; }
void SystemCalendarView::Refresh() { impl_->Refresh(); }
void SystemCalendarView::Close() { impl_->Close(); }
}
