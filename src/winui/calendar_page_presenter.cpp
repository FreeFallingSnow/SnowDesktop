#include "pch.h"
#include "calendar_page_presenter.h"
#include "settings_presenter_controls.h"
#include "../l10n.h"
#include <charconv>
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
    muxc::ToggleSwitch calendarToggle, allDay;
    muxc::ComboBox calendarChoice;
    SettingRow calendarEnabledRow, calendarRow;
    muxc::TextBlock listHeading, error;
    muxc::ListView list;
    muxc::TextBox title, notes;
    muxc::CalendarDatePicker date;
    muxc::CalendarDatePicker until, occurrenceDate;
    muxc::CalendarView multipleDates;
    muxc::ComboBox mode, scope, monthDay;
    muxc::TextBox interval;
    muxc::ToggleSwitch neverEnds;
    muxc::TextBlock ruleSummary;
    muxc::Button editOccurrence;
    std::vector<muxc::CheckBox> weekdays;
    muxc::StackPanel weekdayRow;
    muxc::TimePicker start, end;
    muxc::ComboBox reminder;
    static constexpr std::array<int, 7> reminderValues = {-1, 0, 5, 15, 30, 60, 1440};
    muxc::Button add, save, remove, cancel, refresh;
    muxc::ContentDialog dialog{nullptr};
    std::vector<calendar::DisplayOption> calendars;
    std::vector<calendar::CalendarEvent> events;
    std::vector<calendar::CalendarSeries> series;
    struct Row { bool isSeries = false; std::size_t index = 0; };
    std::vector<Row> rows;
    calendar::CalendarEvent editing;
    calendar::CalendarSeries editingSeries;
    bool editingSeriesMode = false;
    std::string confirmedRule;
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
        calendarEnabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        calendarRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        for (const auto& row : {calendarEnabledRow.root, calendarRow.root})
            preferences.Children().Append(row);
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
        until.CalendarIdentifier(L"GregorianCalendar");
        until.DateFormat(L"{year.full}-{month.integer(2)}-{day.integer(2)}");
        occurrenceDate.CalendarIdentifier(L"GregorianCalendar");
        occurrenceDate.DateFormat(L"{year.full}-{month.integer(2)}-{day.integer(2)}");
        multipleDates.CalendarIdentifier(L"GregorianCalendar");
        multipleDates.SelectionMode(muxc::CalendarViewSelectionMode::Multiple);
        multipleDates.MaxHeight(320);
        interval.MaxLength(2);
        interval.Text(L"1");
        for (int day = 0; day < 7; ++day)
        {
            muxc::CheckBox checkbox;
            weekdays.push_back(checkbox);
            weekdayRow.Children().Append(checkbox);
        }
        weekdayRow.Orientation(muxc::Orientation::Vertical);
        weekdayRow.Spacing(8);
        weekdayRow.MaxWidth(700);
        start.MinuteIncrement(1); end.MinuteIncrement(1);
        notes.AcceptsReturn(true); notes.TextWrapping(mux::TextWrapping::Wrap); notes.MaxHeight(180);
        editor.Children().Append(title); editor.Children().Append(mode); editor.Children().Append(scope);
        editor.Children().Append(ruleSummary);
        editor.Children().Append(date); editor.Children().Append(multipleDates);
        editor.Children().Append(interval); editor.Children().Append(weekdayRow);
        editor.Children().Append(monthDay); editor.Children().Append(neverEnds);
        editor.Children().Append(until); editor.Children().Append(occurrenceDate);
        editor.Children().Append(editOccurrence); editor.Children().Append(allDay);
        editor.Children().Append(start); editor.Children().Append(end); editor.Children().Append(reminder);
        editor.Children().Append(notes);
        muxc::StackPanel buttons; buttons.Orientation(muxc::Orientation::Horizontal); buttons.Spacing(8);
        buttons.Children().Append(save); buttons.Children().Append(cancel); buttons.Children().Append(remove);
        editor.Children().Append(buttons);
        error.TextWrapping(mux::TextWrapping::Wrap); root.Children().Append(error);
        editorCard.Visibility(mux::Visibility::Collapsed);
        auto ctoken = calendarToggle.Toggled([this](const auto&, const auto&) { Commit(); });
        revoke.push_back([c = calendarToggle, ctoken] { c.Toggled(ctoken); });
        for (auto choice : {calendarChoice})
        {
            auto token = choice.SelectionChanged([this](const auto&, const auto&) { Commit(); });
            revoke.push_back([choice, token] { choice.SelectionChanged(token); });
        }
        auto token = allDay.Toggled([this](const auto&, const auto&) { UpdateTimeVisibility(); });
        revoke.push_back([c = allDay, token] { c.Toggled(token); });
        auto modeToken = mode.SelectionChanged([this](const auto&, const auto&) { UpdateModeVisibility(); });
        revoke.push_back([c = mode, modeToken] { c.SelectionChanged(modeToken); });
        auto datesToken = multipleDates.SelectedDatesChanged([this](const auto&, const auto&) { UpdateModeVisibility(); });
        revoke.push_back([c = multipleDates, datesToken] { c.SelectedDatesChanged(datesToken); });
        auto scopeToken = scope.SelectionChanged([this](const auto&, const auto&) {
            if (updating || editingSeries.id.empty()) return;
            if (scope.SelectedIndex() == 1) EditSeries(editingSeries);
            else if (scope.SelectedIndex() == 0) LoadOccurrence();
        });
        revoke.push_back([c = scope, scopeToken] { c.SelectionChanged(scopeToken); });
        auto endToken = neverEnds.Toggled([this](const auto&, const auto&) { UpdateModeVisibility(); });
        revoke.push_back([c = neverEnds, endToken] { c.Toggled(endToken); });
        Click(editOccurrence, [this] { LoadOccurrence(); });
        auto selection = list.SelectionChanged([this](const auto&, const auto&) {
            if (updating || closed || !active) return;
            auto index = list.SelectedIndex();
            if (index >= 0 && static_cast<size_t>(index) < rows.size())
            {
                const auto row = rows[static_cast<std::size_t>(index)];
                if (row.isSeries) EditSeries(series[row.index]);
                else Edit(events[row.index]);
            }
        });
        revoke.push_back([c = list, selection] { c.SelectionChanged(selection); });
        Click(add, [this] {
            calendar::CalendarEvent event;
            event.date = calendar::CalendarService::CurrentLocalNow().date;
            event.allDay = true; Edit(event);
        });
        Click(refresh, [this] { Refresh(true); });
        Click(cancel, [this] { editorCard.Visibility(mux::Visibility::Collapsed); editing = {}; editingSeries = {}; list.SelectedIndex(-1); });
        Click(save, [this] { Save(); });
        Click(remove, [this] { ConfirmDelete(); });
        timer.Interval(std::chrono::seconds(3));
        auto tick = timer.Tick([this](const auto&, const auto&) { Refresh(false); });
        revoke.push_back([c = timer, tick] { c.Tick(tick); });
        Localize();
    }
    void Commit()
    {
        calendarChoice.IsEnabled(calendarToggle.IsOn());
        if (updating || closed || !active || !hasSnapshot || !actions.commitGeneral) return;
        auto p = prefs;
        p.enabled = calendarToggle.IsOn();
        const auto c = calendarChoice.SelectedIndex();
        if (c >= 0 && static_cast<size_t>(c) < calendars.size()) p.calendar = calendars[c].id;
        actions.commitGeneral(generation, [p](GeneralSettings& settings) { settings.calendarDisplay = p; });
    }
    void Select()
    {
        calendarToggle.IsOn(prefs.enabled);
        for (size_t i = 0; i < calendars.size(); ++i) if (calendars[i].id == prefs.calendar) calendarChoice.SelectedIndex(static_cast<int>(i));
        calendarChoice.IsEnabled(prefs.enabled);
    }
    void Localize()
    {
        updating = true;
        calendarEnabledRow.SetText(L("settings.calendar.showSecondary"));
        calendarRow.SetText(L("settings.calendar.type"));
        listHeading.Text(L("settings.calendar.events"));
        title.Header(winrt::box_value(L("settings.calendar.title")));
        date.Header(winrt::box_value(L("settings.calendar.date")));
        until.Header(winrt::box_value(L("settings.calendar.endDate")));
        occurrenceDate.Header(winrt::box_value(L("settings.calendar.occurrenceDate")));
        interval.Header(winrt::box_value(L("settings.calendar.interval")));
        neverEnds.Header(winrt::box_value(L("settings.calendar.neverEnds")));
        mode.Header(winrt::box_value(L("settings.calendar.dateMode")));
        scope.Header(winrt::box_value(L("settings.calendar.scope")));
        monthDay.Header(winrt::box_value(L("settings.calendar.monthDay")));
        const int selectedMode = mode.SelectedIndex();
        mode.Items().Clear();
        for (const auto& key : {"settings.calendar.mode.single", "settings.calendar.mode.dates",
            "settings.calendar.mode.weekly", "settings.calendar.mode.monthly"})
            mode.Items().Append(winrt::box_value(L(key)));
        mode.SelectedIndex(selectedMode >= 0 ? selectedMode : 0);
        const int selectedScope = scope.SelectedIndex();
        scope.Items().Clear();
        scope.Items().Append(winrt::box_value(L("settings.calendar.scope.once")));
        scope.Items().Append(winrt::box_value(L("settings.calendar.scope.series")));
        scope.SelectedIndex(selectedScope >= 0 ? selectedScope : 1);
        const int selectedDay = monthDay.SelectedIndex();
        monthDay.Items().Clear();
        monthDay.Items().Append(winrt::box_value(L("settings.calendar.lastDay")));
        for (int day = 1; day <= 31; ++day)
            monthDay.Items().Append(winrt::box_value(day));
        monthDay.SelectedIndex(selectedDay >= 0 ? selectedDay : 1);
        for (std::size_t index = 0; index < weekdays.size(); ++index)
            weekdays[index].Content(winrt::box_value(L(("settings.calendar.weekday." + std::to_string(index + 1)).c_str())));
        editOccurrence.Content(winrt::box_value(L("settings.calendar.editOccurrence")));
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
        calendarChoice.Items().Clear();
        for (auto& option : calendars) calendarChoice.Items().Append(winrt::box_value(option.label));
        Select(); updating = false;
        UpdateModeVisibility();
    }
    std::wstring RuleSummary(const calendar::CalendarSeriesRule& rule) const
    {
        auto text = L(("settings.calendar.mode." + rule.kind).c_str());
        if (rule.kind == "dates")
            text += L" · " + std::to_wstring(rule.dates.size()) + L" " + L("settings.calendar.selectedDates");
        else
        {
            text += L" · " + L("settings.calendar.interval") + L" " + std::to_wstring(rule.interval);
            if (rule.kind == "weekly")
                for (const auto weekday : rule.weekdays)
                    text += L" " + L(("settings.calendar.weekday." + std::to_string(weekday)).c_str());
            else
                text += L" · " + (rule.monthDay == 0 ? L("settings.calendar.lastDay") :
                    L("settings.calendar.monthDay") + L" " + std::to_wstring(rule.monthDay));
            text += L" · " + (rule.endDate.empty() ? L("settings.calendar.neverEnds") :
                L("settings.calendar.endDate") + L" " + winrt::to_hstring(rule.endDate).c_str());
        }
        return text;
    }
    void Refresh(bool explicitRefresh)
    {
        if (!active || closed || !actions.events || !actions.series || !hasSnapshot) return;
        try
        {
            const auto result = actions.events(generation);
            const auto seriesResult = actions.series(generation);
            if (!result || !seriesResult) { error.Text(L("settings.calendar.failed")); return; }
            bool same = result->size() == events.size() && seriesResult->size() == series.size();
            for (size_t i = 0; same && i < events.size(); ++i)
                same = (*result)[i].id == events[i].id && (*result)[i].revision == events[i].revision;
            for (size_t i = 0; same && i < series.size(); ++i)
                same = (*seriesResult)[i].id == series[i].id && (*seriesResult)[i].revision == series[i].revision;
            if (same && !explicitRefresh) return;
            updating = true;
            events = *result; series = *seriesResult; rows.clear(); list.Items().Clear();
            int selected = -1;
            for (size_t i = 0; i < events.size(); ++i)
            {
                const auto& event = events[i];
                rows.push_back({false, i});
                list.Items().Append(winrt::box_value(winrt::to_hstring(event.date + "  " + event.title)));
                if (editingSeries.id.empty() && event.id == editing.id) selected = static_cast<int>(rows.size() - 1);
            }
            for (size_t i = 0; i < series.size(); ++i)
            {
                const auto& item = series[i];
                rows.push_back({true, i});
                const auto summary = RuleSummary(item.rule);
                const auto firstDate = item.rule.kind == "dates" && !item.rule.dates.empty()
                    ? item.rule.dates.front() : item.rule.startDate;
                std::wstring label = winrt::to_hstring(firstDate + "  " + item.event.title).c_str();
                label += L" · " + summary;
                list.Items().Append(winrt::box_value(label));
                if (item.id == editingSeries.id) selected = static_cast<int>(rows.size() - 1);
            }
            list.SelectedIndex(selected); updating = false;
            listHeading.Text(L(rows.empty() ? "settings.calendar.empty" : "settings.calendar.events"));
            if (explicitRefresh)
            {
                error.Text(L"");
                if (selected >= 0)
                {
                    const auto row = rows[static_cast<std::size_t>(selected)];
                    if (row.isSeries) EditSeries(series[row.index]);
                    else Edit(events[row.index]);
                }
            }
        }
        catch (...) { updating = false; error.Text(L("settings.calendar.failed")); }
    }
    void UpdateTimeVisibility()
    {
        const auto visibility = allDay.IsOn() ? mux::Visibility::Collapsed : mux::Visibility::Visible;
        start.Visibility(visibility); end.Visibility(visibility);
    }
    void UpdateModeVisibility()
    {
        if (updating) return;
        if (editingSeriesMode && mode.SelectedIndex() == 0) mode.SelectedIndex(1);
        const int selected = editingSeriesMode || editing.id.empty() ? mode.SelectedIndex() : 0;
        const bool rule = editingSeriesMode || editing.id.empty();
        mode.Visibility(rule ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        scope.Visibility(editingSeries.id.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        date.Visibility(selected == 1 ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        multipleDates.Visibility(selected == 1 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        interval.Visibility(selected >= 2 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        weekdayRow.Visibility(selected == 2 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        monthDay.Visibility(selected == 3 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        neverEnds.Visibility(selected >= 2 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        until.Visibility(selected >= 2 && !neverEnds.IsOn() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        occurrenceDate.Visibility(editingSeries.id.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        editOccurrence.Visibility(editingSeries.id.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        if (selected == 1)
            ruleSummary.Text(L("settings.calendar.selectedDates") + L" " +
                winrt::to_hstring(multipleDates.SelectedDates().Size()));
        else if (selected >= 2)
            ruleSummary.Text(L(selected == 2 ? "settings.calendar.mode.weekly" : "settings.calendar.mode.monthly") +
                L" · " + interval.Text());
        else ruleSummary.Text(L"");
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
    static std::string DateString(const winrt::Windows::Foundation::DateTime& value)
    {
        winrt::Windows::Globalization::Calendar local;
        local.ChangeCalendarSystem(L"GregorianCalendar"); local.SetDateTime(value);
        char text[16]{};
        std::snprintf(text, sizeof(text), "%04d-%02d-%02d", local.Year(), local.Month(), local.Day());
        return text;
    }
    std::string SelectedDate() const
    {
        if (!date.Date()) return {};
        return DateString(date.Date().Value());
    }
    static int Minutes(const muxc::TimePicker& picker)
    {
        return picker.SelectedTime() ? static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(picker.SelectedTime().Value()).count()) : -1;
    }
    void Edit(const calendar::CalendarEvent& event)
    {
        updating = true;
        editing = event;
        editingSeries = {};
        if (!event.seriesId.empty())
            for (const auto& item : series)
                if (item.id == event.seriesId) { editingSeries = item; break; }
        editingSeriesMode = false;
        confirmedRule.clear();
        mode.SelectedIndex(0);
        scope.SelectedIndex(0);
        title.Text(winrt::to_hstring(event.title));
        const auto selectedDate = PickerDate(event.date);
        multipleDates.SelectedDates().Clear();
        multipleDates.SelectedDates().Append(selectedDate);
        interval.Text(L"1");
        const auto info = calendar::CalendarService::GetDateInfo(event.date);
        for (std::size_t index = 0; index < weekdays.size(); ++index)
            weekdays[index].IsChecked(info && info->weekday == static_cast<int>(index + 1));
        monthDay.SelectedIndex(info ? info->day : 1);
        neverEnds.IsOn(true);
        until.Date(nullptr);
        if (selectedDate < date.MinDate()) date.MinDate(selectedDate);
        if (selectedDate > date.MaxDate()) date.MaxDate(selectedDate);
        date.Date(winrt::box_value(selectedDate).as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::DateTime>>());
        occurrenceDate.Date(winrt::box_value(PickerDate(event.occurrenceDate.empty() ? event.date : event.occurrenceDate))
            .as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::DateTime>>());
        allDay.IsOn(event.allDay);
        start.Time(std::chrono::minutes(event.startMinutes)); end.Time(std::chrono::minutes(event.endMinutes));
        UpdateTimeVisibility();
        notes.Text(winrt::to_hstring(event.notes)); for (size_t i = 0; i < reminderValues.size(); ++i) if (reminderValues[i] == event.reminderMinutes) reminder.SelectedIndex(static_cast<int>(i));
        remove.IsEnabled(!event.id.empty()); error.Text(L""); editorCard.Visibility(mux::Visibility::Visible);
        updating = false; UpdateModeVisibility();
    }
    void EditSeries(const calendar::CalendarSeries& item)
    {
        updating = true;
        editingSeries = item;
        editingSeriesMode = true;
        editing = item.event;
        confirmedRule.clear();
        title.Text(winrt::to_hstring(item.event.title));
        notes.Text(winrt::to_hstring(item.event.notes));
        allDay.IsOn(item.event.allDay);
        start.Time(std::chrono::minutes(item.event.startMinutes));
        end.Time(std::chrono::minutes(item.event.endMinutes));
        for (size_t index = 0; index < reminderValues.size(); ++index)
            if (reminderValues[index] == item.event.reminderMinutes) reminder.SelectedIndex(static_cast<int>(index));
        const auto startDate = PickerDate(item.rule.startDate);
        if (startDate < date.MinDate()) date.MinDate(startDate);
        if (startDate > date.MaxDate()) date.MaxDate(startDate);
        date.Date(winrt::box_value(startDate).as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::DateTime>>());
        occurrenceDate.Date(winrt::box_value(startDate).as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::DateTime>>());
        multipleDates.SelectedDates().Clear();
        for (const auto& selected : item.rule.dates)
            multipleDates.SelectedDates().Append(PickerDate(selected));
        mode.SelectedIndex(item.rule.kind == "dates" ? 1 : item.rule.kind == "weekly" ? 2 : 3);
        scope.SelectedIndex(1);
        interval.Text(winrt::to_hstring(item.rule.interval));
        monthDay.SelectedIndex(item.rule.monthDay);
        for (std::size_t index = 0; index < weekdays.size(); ++index)
        {
            const auto weekday = static_cast<int>(index + 1);
            const bool checked = std::find(item.rule.weekdays.begin(), item.rule.weekdays.end(), weekday) != item.rule.weekdays.end();
            weekdays[index].IsChecked(checked);
        }
        neverEnds.IsOn(item.rule.endDate.empty());
        if (!item.rule.endDate.empty())
            until.Date(winrt::box_value(PickerDate(item.rule.endDate))
                .as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::DateTime>>());
        remove.IsEnabled(true); error.Text(L""); editorCard.Visibility(mux::Visibility::Visible);
        updating = false; UpdateTimeVisibility(); UpdateModeVisibility();
    }
    void LoadOccurrence()
    {
        if (editingSeries.id.empty() || !occurrenceDate.Date() || !actions.occurrence) return;
        const auto original = DateString(occurrenceDate.Date().Value());
        const auto result = actions.occurrence(generation, editingSeries.id + "/" + original);
        if (!result) { error.Text(L("settings.calendar.invalidOccurrence")); return; }
        Edit(*result);
    }
    void Save()
    {
        if (!actions.mutate) return;
        auto event = editing;
        event.title = winrt::to_string(title.Text());
        event.notes = winrt::to_string(notes.Text()); event.allDay = allDay.IsOn();
        event.startMinutes = event.allDay ? 0 : Minutes(start);
        event.endMinutes = event.allDay ? 0 : Minutes(end);
        const int reminderIndex = reminder.SelectedIndex();
        const int minutes = reminderIndex >= 0 && static_cast<size_t>(reminderIndex) < reminderValues.size() ? reminderValues[reminderIndex] : -2;
        const int selectedMode = editingSeriesMode || editing.id.empty() ? mode.SelectedIndex() : 0;
        const bool seriesMode = selectedMode > 0;
        calendar::CalendarSeries candidate;
        if (seriesMode)
        {
            auto& rule = candidate.rule;
            rule.kind = selectedMode == 1 ? "dates" : selectedMode == 2 ? "weekly" : "monthly";
            if (selectedMode == 1)
            {
                for (const auto& selected : multipleDates.SelectedDates())
                    rule.dates.push_back(DateString(selected));
                std::sort(rule.dates.begin(), rule.dates.end());
                rule.dates.erase(std::unique(rule.dates.begin(), rule.dates.end()), rule.dates.end());
                if (!rule.dates.empty())
                {
                    rule.startDate = rule.dates.front();
                    rule.endDate = rule.dates.back();
                }
            }
            else
            {
                rule.startDate = SelectedDate();
                rule.endDate = neverEnds.IsOn() ? std::string{} :
                    until.Date() ? DateString(until.Date().Value()) : std::string{};
                const auto intervalText = winrt::to_string(interval.Text());
                const auto parsed = std::from_chars(intervalText.data(),
                    intervalText.data() + intervalText.size(), rule.interval);
                if (parsed.ec != std::errc{} || parsed.ptr != intervalText.data() + intervalText.size())
                    rule.interval = 0;
                if (selectedMode == 2)
                {
                    for (std::size_t index = 0; index < weekdays.size(); ++index)
                        if (weekdays[index].IsChecked() && weekdays[index].IsChecked().Value())
                            rule.weekdays.push_back(static_cast<int>(index + 1));
                }
                else rule.monthDay = monthDay.SelectedIndex();
            }
            event.date = rule.startDate;
            candidate.id = editingSeriesMode ? editingSeries.id : std::string{};
            candidate.revision = editingSeriesMode ? editingSeries.revision : 0;
            candidate.event = event;
            const bool invalid = !calendar::CalendarService::GetDateInfo(rule.startDate) ||
                (selectedMode == 1 && (rule.dates.empty() || rule.dates.size() > 366)) ||
                (selectedMode >= 2 && (rule.interval < 1 || rule.interval > 99 ||
                    (!neverEnds.IsOn() && (!calendar::CalendarService::GetDateInfo(rule.endDate) ||
                        rule.endDate < rule.startDate)))) ||
                (selectedMode == 2 && rule.weekdays.empty()) ||
                (selectedMode == 3 && (rule.monthDay < 0 || rule.monthDay > 31));
            if (invalid) { error.Text(L("settings.calendar.invalid")); return; }
        }
        else event.date = SelectedDate();
        if (minutes < -1 ||
            event.title.empty() || !calendar::CalendarService::GetDateInfo(event.date) ||
            event.startMinutes < 0 || event.endMinutes < event.startMinutes)
        { error.Text(L("settings.calendar.invalid")); return; }
        event.reminderMinutes = static_cast<int>(minutes);
        if (seriesMode) candidate.event = event;
        try
        {
            if (seriesMode && editingSeriesMode)
            {
                std::string signature = candidate.rule.kind + candidate.rule.startDate +
                    candidate.rule.endDate + std::to_string(candidate.rule.interval) +
                    std::to_string(candidate.rule.monthDay);
                for (const auto& value : candidate.rule.dates) signature += value;
                for (const auto value : candidate.rule.weekdays) signature += std::to_string(value);
                bool discards = false;
                for (const auto& [originalDate, exception] : editingSeries.exceptions)
                    if (!calendar::CalendarService::MatchesRule(candidate.rule, originalDate))
                    { discards = true; break; }
                if (discards && confirmedRule != signature)
                {
                    confirmedRule = std::move(signature);
                    error.Text(L("settings.calendar.confirmExceptions"));
                    return;
                }
            }
            const auto result = seriesMode && actions.mutateSeries
                ? actions.mutateSeries(generation, candidate, false)
                : seriesMode ? calendar::MutationResult{false, {}, 0, "unavailable"}
                    : actions.mutate(generation, event, false);
            if (!result.ok) { error.Text(L(result.error == "conflict" ? "settings.calendar.conflict" : "settings.calendar.failed")); return; }
            editing = {}; editingSeries = {}; editorCard.Visibility(mux::Visibility::Collapsed); Refresh(true);
        }
        catch (...) { error.Text(L("settings.calendar.failed")); }
    }
    winrt::fire_and_forget ConfirmDelete()
    {
        const auto lifetime = shared_from_this();
        if ((editing.id.empty() && editingSeries.id.empty()) || dialog || !actions.mutate) co_return;
        const auto target = editing;
        const auto targetSeries = editingSeries;
        const bool removeSeries = editingSeriesMode;
        const auto session = generation;
        try
        {
            muxc::ContentDialog confirmation;
            dialog = confirmation;
            confirmation.XamlRoot(root.XamlRoot());
            confirmation.Title(winrt::box_value(L("settings.calendar.confirmDelete")));
            confirmation.Content(winrt::box_value(winrt::to_hstring(
                (removeSeries ? targetSeries.rule.startDate : target.date) + "  " + target.title)));
            confirmation.PrimaryButtonText(L("app.settings.delete")); confirmation.CloseButtonText(L("app.settings.cancel"));
            confirmation.DefaultButton(muxc::ContentDialogButton::Close);
            const auto answer = co_await confirmation.ShowAsync();
            if (dialog == confirmation) dialog = nullptr;
            if (closed || !active || generation != session || answer != muxc::ContentDialogResult::Primary || !actions.mutate) co_return;
            const auto result = removeSeries && actions.mutateSeries
                ? actions.mutateSeries(session, targetSeries, true)
                : actions.mutate(session, target, true);
            if (!result.ok) { error.Text(L(result.error == "conflict" ? "settings.calendar.conflict" : "settings.calendar.failed")); co_return; }
            editing = {}; editingSeries = {}; editorCard.Visibility(mux::Visibility::Collapsed); Refresh(true);
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
    registrar("calendar.events", impl_->list);
}
}
