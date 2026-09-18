#include "pch.h"
#include "calendar_page_presenter.h"
#include "settings_presenter_controls.h"
#include "../l10n.h"
#include <cstdio>
#include <winrt/Windows.Globalization.h>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
using presenter_controls::SettingRow;

struct CalendarPagePresenter::Impl : std::enable_shared_from_this<Impl>
{
    LocalizeCallback localize;
    CalendarPageActions actions;
    muxc::StackPanel root, preferences, editor;
    muxc::Border editorCard{nullptr};
    muxc::ToggleSwitch calendarToggle, holidayToggle, allDay;
    muxc::ComboBox calendarChoice, regionChoice;
    SettingRow calendarEnabledRow, calendarRow, holidayEnabledRow, regionRow;
    muxc::TextBlock description, listHeading, error;
    muxc::ListView list;
    muxc::TextBox title, notes;
    muxc::CalendarDatePicker date;
    muxc::TimePicker start, end;
    muxc::ComboBox reminder;
    static constexpr std::array<int, 7> reminderValues = {-1, 0, 5, 15, 30, 60, 1440};
    muxc::Button add, save, remove, cancel, refresh;
    muxc::ContentDialog dialog{nullptr};
    std::vector<calendar::DisplayOption> calendars, regions;
    std::vector<calendar::CalendarEvent> events;
    calendar::CalendarEvent editing;
    calendar::DisplayPreferences prefs;
    mux::DispatcherTimer timer;
    std::vector<std::function<void()>> revoke;
    std::uint64_t generation = 0;
    bool active = false, closed = false, updating = false, hasSnapshot = false;

    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    muxc::StackPanel Card(const mux::Style& style, muxc::Border* outer = nullptr)
    {
        muxc::Border border;
        border.Style(style);
        muxc::StackPanel content;
        content.Spacing(12);
        border.Child(content);
        root.Children().Append(border);
        if (outer) *outer = border;
        return content;
    }
    template<class F> void Click(muxc::Button button, F callback)
    {
        auto token = button.Click([this, callback](const auto&, const auto&) {
            if (!closed && active && hasSnapshot) callback();
        });
        revoke.push_back([button, token] { button.Click(token); });
    }
    Impl(LocalizeCallback callback, const mux::Style& style) : localize(std::move(callback))
    {
        root.Spacing(8);
        preferences = Card(style);
        calendarEnabledRow.Initialize(calendarToggle);
        calendarRow.Initialize(calendarChoice);
        holidayEnabledRow.Initialize(holidayToggle);
        regionRow.Initialize(regionChoice);
        calendarEnabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        calendarRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        holidayEnabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        regionRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        for (const auto& row : {calendarEnabledRow.root, calendarRow.root, holidayEnabledRow.root, regionRow.root})
            preferences.Children().Append(row);
        description.TextWrapping(mux::TextWrapping::Wrap);
        preferences.Children().Append(description);
        auto schedules = Card(style);
        listHeading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        schedules.Children().Append(listHeading);
        muxc::StackPanel toolbar;
        toolbar.Orientation(muxc::Orientation::Horizontal); toolbar.Spacing(8);
        toolbar.Children().Append(add); toolbar.Children().Append(refresh);
        schedules.Children().Append(toolbar);
        list.MaxHeight(320);
        list.SelectionMode(muxc::ListViewSelectionMode::Single);
        schedules.Children().Append(list);
        editor = Card(style, &editorCard);
        title.MaxLength(512); notes.MaxLength(8192);
        date.CalendarIdentifier(L"GregorianCalendar");
        date.DateFormat(L"{year.full}-{month.integer(2)}-{day.integer(2)}");
        start.MinuteIncrement(1); end.MinuteIncrement(1);
        notes.AcceptsReturn(true); notes.TextWrapping(mux::TextWrapping::Wrap); notes.MaxHeight(180);
        editor.Children().Append(title); editor.Children().Append(date); editor.Children().Append(allDay);
        editor.Children().Append(start); editor.Children().Append(end); editor.Children().Append(reminder);
        editor.Children().Append(notes);
        muxc::StackPanel buttons; buttons.Orientation(muxc::Orientation::Horizontal); buttons.Spacing(8);
        buttons.Children().Append(save); buttons.Children().Append(cancel); buttons.Children().Append(remove);
        editor.Children().Append(buttons);
        error.TextWrapping(mux::TextWrapping::Wrap); root.Children().Append(error);
        editorCard.Visibility(mux::Visibility::Collapsed);
        auto ctoken = calendarToggle.Toggled([this](const auto&, const auto&) { Commit(); });
        revoke.push_back([c = calendarToggle, ctoken] { c.Toggled(ctoken); });
        auto htoken = holidayToggle.Toggled([this](const auto&, const auto&) { Commit(); });
        revoke.push_back([c = holidayToggle, htoken] { c.Toggled(htoken); });
        for (auto choice : {calendarChoice, regionChoice})
        {
            auto token = choice.SelectionChanged([this](const auto&, const auto&) { Commit(); });
            revoke.push_back([choice, token] { choice.SelectionChanged(token); });
        }
        auto token = allDay.Toggled([this](const auto&, const auto&) { UpdateTimeVisibility(); });
        revoke.push_back([c = allDay, token] { c.Toggled(token); });
        auto selection = list.SelectionChanged([this](const auto&, const auto&) {
            if (updating || closed || !active) return;
            auto index = list.SelectedIndex();
            if (index >= 0 && static_cast<size_t>(index) < events.size()) Edit(events[index]);
        });
        revoke.push_back([c = list, selection] { c.SelectionChanged(selection); });
        Click(add, [this] {
            calendar::CalendarEvent event;
            event.date = calendar::CalendarService::CurrentLocalNow().date;
            event.allDay = true; Edit(event);
        });
        Click(refresh, [this] { Refresh(true); });
        Click(cancel, [this] { editorCard.Visibility(mux::Visibility::Collapsed); editing = {}; list.SelectedIndex(-1); });
        Click(save, [this] { Save(); });
        Click(remove, [this] { ConfirmDelete(); });
        timer.Interval(std::chrono::seconds(3));
        auto tick = timer.Tick([this](const auto&, const auto&) { Refresh(false); });
        revoke.push_back([c = timer, tick] { c.Tick(tick); });
        Localize();
    }
    void Commit()
    {
        calendarChoice.IsEnabled(calendarToggle.IsOn()); regionChoice.IsEnabled(holidayToggle.IsOn());
        if (updating || closed || !active || !hasSnapshot || !actions.commitGeneral) return;
        auto p = prefs;
        p.enabled = calendarToggle.IsOn(); p.holidaysEnabled = holidayToggle.IsOn();
        const auto c = calendarChoice.SelectedIndex(), r = regionChoice.SelectedIndex();
        if (c >= 0 && static_cast<size_t>(c) < calendars.size()) p.calendar = calendars[c].id;
        if (r >= 0 && static_cast<size_t>(r) < regions.size()) p.region = regions[r].id;
        actions.commitGeneral(generation, [p](GeneralSettings& settings) { settings.calendarDisplay = p; });
    }
    void Select()
    {
        calendarToggle.IsOn(prefs.enabled); holidayToggle.IsOn(prefs.holidaysEnabled);
        for (size_t i = 0; i < calendars.size(); ++i) if (calendars[i].id == prefs.calendar) calendarChoice.SelectedIndex(static_cast<int>(i));
        for (size_t i = 0; i < regions.size(); ++i) if (regions[i].id == prefs.region) regionChoice.SelectedIndex(static_cast<int>(i));
        calendarChoice.IsEnabled(prefs.enabled); regionChoice.IsEnabled(prefs.holidaysEnabled);
    }
    void Localize()
    {
        updating = true;
        calendarEnabledRow.SetText(L("settings.calendar.showSecondary"));
        calendarRow.SetText(L("settings.calendar.type"));
        holidayEnabledRow.SetText(L("settings.calendar.showHolidays"));
        regionRow.SetText(L("settings.calendar.region"));
        description.Text(L("settings.calendar.description"));
        listHeading.Text(L("settings.calendar.events"));
        title.Header(winrt::box_value(L("settings.calendar.title")));
        date.Header(winrt::box_value(L("settings.calendar.date")));
        start.Header(winrt::box_value(L("settings.calendar.start")));
        end.Header(winrt::box_value(L("settings.calendar.end")));
        reminder.Header(winrt::box_value(L("settings.calendar.reminder")));
        const auto reminderSelection = reminder.SelectedIndex();
        reminder.Items().Clear();
        for (const auto& label : {L("settings.calendar.reminder.-1"),
                L("settings.calendar.reminder.0"), L("settings.calendar.reminder.5"),
                L("settings.calendar.reminder.15"), L("settings.calendar.reminder.30"),
                L("settings.calendar.reminder.60"), L("settings.calendar.reminder.1440")})
            reminder.Items().Append(winrt::box_value(label));
        reminder.SelectedIndex(reminderSelection >= 0 ? reminderSelection : 0);
        notes.Header(winrt::box_value(L("settings.calendar.notes")));
        allDay.Header(winrt::box_value(L("settings.calendar.allDay")));
        add.Content(winrt::box_value(L("settings.calendar.add")));
        refresh.Content(winrt::box_value(L("settings.calendar.refresh")));
        save.Content(winrt::box_value(L("settings.calendar.save")));
        cancel.Content(winrt::box_value(L("app.settings.cancel")));
        remove.Content(winrt::box_value(L("app.settings.delete")));
        calendars = calendar::CalendarOptions(Locale::Instance().GetEffectiveLanguage());
        regions = calendar::HolidayRegions(Locale::Instance().GetEffectiveLanguage());
        calendarChoice.Items().Clear(); regionChoice.Items().Clear();
        for (auto& option : calendars) calendarChoice.Items().Append(winrt::box_value(option.label));
        for (auto& option : regions) regionChoice.Items().Append(winrt::box_value(option.label));
        Select(); updating = false;
    }
    void Refresh(bool explicitRefresh)
    {
        if (!active || closed || !actions.events || !hasSnapshot) return;
        try
        {
            const auto result = actions.events(generation);
            if (!result) { error.Text(L("settings.calendar.failed")); return; }
            bool same = result->size() == events.size();
            for (size_t i = 0; same && i < events.size(); ++i)
                same = (*result)[i].id == events[i].id && (*result)[i].revision == events[i].revision;
            if (same && !explicitRefresh) return;
            updating = true;
            events = *result; list.Items().Clear();
            int selected = -1;
            for (size_t i = 0; i < events.size(); ++i)
            {
                const auto& event = events[i];
                list.Items().Append(winrt::box_value(winrt::to_hstring(event.date + "  " + event.title)));
                if (event.id == editing.id) selected = static_cast<int>(i);
            }
            list.SelectedIndex(selected); updating = false;
            listHeading.Text(L(events.empty() ? "settings.calendar.empty" : "settings.calendar.events"));
            if (explicitRefresh)
            {
                error.Text(L"");
                if (selected >= 0) Edit(events[static_cast<size_t>(selected)]);
            }
        }
        catch (...) { updating = false; error.Text(L("settings.calendar.failed")); }
    }
    void UpdateTimeVisibility()
    {
        const auto visibility = allDay.IsOn() ? mux::Visibility::Collapsed : mux::Visibility::Visible;
        start.Visibility(visibility); end.Visibility(visibility);
    }
    static winrt::Windows::Foundation::DateTime PickerDate(const std::string& value)
    {
        const auto info = *calendar::CalendarService::GetDateInfo(value);
        winrt::Windows::Globalization::Calendar local;
        local.ChangeCalendarSystem(L"GregorianCalendar");
        local.Day(1); local.Year(info.year); local.Month(info.month); local.Day(info.day);
        local.Hour(12); local.Minute(0); local.Second(0); local.Nanosecond(0);
        return local.GetDateTime();
    }
    std::string SelectedDate() const
    {
        if (!date.Date()) return {};
        winrt::Windows::Globalization::Calendar local;
        local.ChangeCalendarSystem(L"GregorianCalendar"); local.SetDateTime(date.Date().Value());
        char value[16]{};
        std::snprintf(value, sizeof(value), "%04d-%02d-%02d", local.Year(), local.Month(), local.Day());
        return value;
    }
    static int Minutes(const muxc::TimePicker& picker)
    {
        return picker.SelectedTime() ? static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(picker.SelectedTime().Value()).count()) : -1;
    }
    void Edit(const calendar::CalendarEvent& event)
    {
        editing = event;
        title.Text(winrt::to_hstring(event.title));
        const auto selectedDate = PickerDate(event.date);
        if (selectedDate < date.MinDate()) date.MinDate(selectedDate);
        if (selectedDate > date.MaxDate()) date.MaxDate(selectedDate);
        date.Date(winrt::box_value(selectedDate).as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::DateTime>>());
        allDay.IsOn(event.allDay);
        start.Time(std::chrono::minutes(event.startMinutes)); end.Time(std::chrono::minutes(event.endMinutes));
        UpdateTimeVisibility();
        notes.Text(winrt::to_hstring(event.notes)); for (size_t i = 0; i < reminderValues.size(); ++i) if (reminderValues[i] == event.reminderMinutes) reminder.SelectedIndex(static_cast<int>(i));
        remove.IsEnabled(!event.id.empty()); error.Text(L""); editorCard.Visibility(mux::Visibility::Visible);
    }
    void Save()
    {
        if (!actions.mutate) return;
        auto event = editing;
        event.title = winrt::to_string(title.Text()); event.date = SelectedDate();
        event.notes = winrt::to_string(notes.Text()); event.allDay = allDay.IsOn();
        event.startMinutes = event.allDay ? 0 : Minutes(start);
        event.endMinutes = event.allDay ? 0 : Minutes(end);
        const int reminderIndex = reminder.SelectedIndex();
        const int minutes = reminderIndex >= 0 && static_cast<size_t>(reminderIndex) < reminderValues.size() ? reminderValues[reminderIndex] : -2;
        if (minutes < -1 ||
            event.title.empty() || !calendar::CalendarService::GetDateInfo(event.date) ||
            event.startMinutes < 0 || event.endMinutes < event.startMinutes)
        { error.Text(L("settings.calendar.invalid")); return; }
        event.reminderMinutes = static_cast<int>(minutes);
        try
        {
            const auto result = actions.mutate(generation, event, false);
            if (!result.ok) { error.Text(L(result.error == "conflict" ? "settings.calendar.conflict" : "settings.calendar.failed")); return; }
            editing = {}; editorCard.Visibility(mux::Visibility::Collapsed); Refresh(true);
        }
        catch (...) { error.Text(L("settings.calendar.failed")); }
    }
    winrt::fire_and_forget ConfirmDelete()
    {
        const auto lifetime = shared_from_this();
        if (editing.id.empty() || dialog || !actions.mutate) co_return;
        const auto target = editing; const auto session = generation;
        try
        {
            muxc::ContentDialog confirmation;
            dialog = confirmation;
            confirmation.XamlRoot(root.XamlRoot());
            confirmation.Title(winrt::box_value(L("settings.calendar.confirmDelete")));
            confirmation.Content(winrt::box_value(winrt::to_hstring(target.date + "  " + target.title)));
            confirmation.PrimaryButtonText(L("app.settings.delete")); confirmation.CloseButtonText(L("app.settings.cancel"));
            confirmation.DefaultButton(muxc::ContentDialogButton::Close);
            const auto answer = co_await confirmation.ShowAsync();
            if (dialog == confirmation) dialog = nullptr;
            if (closed || !active || generation != session || answer != muxc::ContentDialogResult::Primary || !actions.mutate) co_return;
            const auto result = actions.mutate(session, target, true);
            if (!result.ok) { error.Text(L(result.error == "conflict" ? "settings.calendar.conflict" : "settings.calendar.failed")); co_return; }
            editing = {}; editorCard.Visibility(mux::Visibility::Collapsed); Refresh(true);
        }
        catch (...) { dialog = nullptr; if (!closed) error.Text(L("settings.calendar.failed")); }
    }
    void Deactivate()
    { active = false; timer.Stop(); if (dialog) dialog.Hide(); }
    void Close()
    {
        if (closed) return;
        closed = true; Deactivate(); actions = {};
        for (auto& callback : revoke) callback(); revoke.clear();
    }
};
CalendarPagePresenter::CalendarPagePresenter(LocalizeCallback localize, const mux::Style& style)
    : impl_(std::make_shared<Impl>(std::move(localize), style)) {}
CalendarPagePresenter::~CalendarPagePresenter() { Close(); }
void CalendarPagePresenter::SetActions(CalendarPageActions actions) { impl_->actions = std::move(actions); }
mux::UIElement CalendarPagePresenter::Content() const { return impl_->root; }
void CalendarPagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_->closed) return;
    if (impl_->generation != snapshot.generation)
    { impl_->editing = {}; impl_->editorCard.Visibility(mux::Visibility::Collapsed); }
    impl_->generation = snapshot.generation; impl_->hasSnapshot = snapshot.initialized;
    impl_->prefs = snapshot.values.general.calendarDisplay;
    impl_->updating = true; impl_->Select(); impl_->updating = false;
    if (!snapshot.sessionActive) impl_->Deactivate();
}
void CalendarPagePresenter::RefreshLocalizedText() { impl_->Localize(); }
void CalendarPagePresenter::Activate() { if (impl_->closed) return; impl_->active = true; impl_->Refresh(true); impl_->timer.Start(); }
void CalendarPagePresenter::Deactivate() { impl_->Deactivate(); }
void CalendarPagePresenter::Close() { if (impl_) impl_->Close(); }
void CalendarPagePresenter::RegisterFocusTargets(const FocusRegistrar& registrar) const
{
    registrar("calendar.secondary", impl_->calendarToggle);
    registrar("calendar.region", impl_->regionChoice);
    registrar("calendar.events", impl_->list);
}
}
