#include "pch.h"
#include "calendar_page_presenter.h"
#include "settings_presenter_controls.h"
#include "common/l10n.h"
#include "system/calendar/system_calendar_editor_state.h"
#include <cmath>
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
    muxc::ToggleSwitch calendarToggle;
    muxc::CheckBox allDay;
    muxc::ComboBox calendarChoice;
    SettingRow calendarEnabledRow, calendarRow;
    SettingRow titleRow, modeRow, scopeRow, dateRow, multipleDatesRow;
    SettingRow intervalRow, weekdaysRow, monthDayRow, endTypeRow, untilRow;
    SettingRow occurrenceRow, allDayRow, startRow, endRow, reminderRow, notesRow, actionsRow;
    muxc::TextBlock listHeading;
    muxc::InfoBar scheduleFeedback;
    muxc::ListView list;
    muxc::TextBox title, notes;
    muxc::CalendarDatePicker date;
    muxc::CalendarDatePicker until, occurrenceDate;
    muxc::CalendarView multipleDates;
    muxc::ComboBox mode, scope, monthDay;
    muxc::NumberBox interval;
    muxc::ComboBox endType;
    muxc::Button editOccurrence;
    std::vector<muxc::CheckBox> weekdays;
    std::vector<muxc::TextBlock> weekdayLabels;
    muxc::Grid weekdayChoices;
    muxc::TimePicker start, end;
    muxc::ComboBox reminder;
    static constexpr std::array<int, 7> reminderValues = {-1, 0, 5, 15, 30, 60, 1440};
    muxc::Button save, remove, cancel;
    muxc::HyperlinkButton add, refresh;
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
    std::wstring savedDraft;
    int editingListIndex = -1;
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
    template<class Control, class F> void Click(Control button, F callback)
    {
        auto token = button.Click([this, callback](const auto&, const auto&) {
            if (!closed && active && hasSnapshot) callback();
        });
        revoke.push_back([button, token] { button.Click(token); });
    }
    Impl(LocalizeCallback callback, const mux::Style& style) : localize(std::move(callback))
    {
        root.Spacing(24);
        muxc::Border preferencesSurface;
        preferences = Card(style, &preferencesSurface);
        preferencesSurface.Padding({0, 0, 0, 0});
        calendarEnabledRow.Initialize(calendarToggle);
        calendarRow.Initialize(calendarChoice);
        calendarEnabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        calendarRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        for (const auto& row : {calendarEnabledRow.root, calendarRow.root})
            preferences.Children().Append(row);
        presenter_controls::UpdateSettingsSubrows(preferences, 0);
        muxc::StackPanel toolbar;
        toolbar.Orientation(muxc::Orientation::Horizontal); toolbar.Spacing(8);
        presenter_controls::ConfigureSettingsHeadingLink(add);
        presenter_controls::ConfigureSettingsHeadingLink(refresh);
        toolbar.Children().Append(add); toolbar.Children().Append(refresh);
        SettingRow schedulesHeader;
        schedulesHeader.Initialize(toolbar, 0.0);
        schedulesHeader.SetControlAlignment(mux::HorizontalAlignment::Right);
        listHeading = schedulesHeader.label;
        listHeading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        muxc::StackPanel schedulesSection;
        schedulesSection.Spacing(8);
        root.Children().Append(schedulesSection);
        schedulesSection.Children().Append(schedulesHeader.root);
        scheduleFeedback.Severity(muxc::InfoBarSeverity::Warning);
        scheduleFeedback.IsClosable(false);
        scheduleFeedback.IsOpen(false);
        presenter_controls::BindInfoBarVisibility(scheduleFeedback);
        schedulesSection.Children().Append(scheduleFeedback);
        muxc::Border schedulesSurface;
        auto schedules = Card(style, &schedulesSurface);
        root.Children().RemoveAtEnd();
        schedulesSection.Children().Append(schedulesSurface);
        schedulesSurface.Padding({0, 0, 0, 0});
        list.MaxHeight(320);
        list.SelectionMode(muxc::ListViewSelectionMode::Single);
        mux::Automation::AutomationProperties::SetAutomationId(list, L"CalendarSchedules");
        mux::Automation::AutomationProperties::SetAutomationId(add, L"CalendarAddSchedule");
        mux::Automation::AutomationProperties::SetAutomationId(save, L"CalendarSaveSchedule");
        mux::Automation::AutomationProperties::SetAutomationId(cancel, L"CalendarCancelEdit");
        mux::Automation::AutomationProperties::SetAutomationId(remove, L"CalendarDeleteSchedule");
        schedules.Children().Append(list);
        editor = Card(style, &editorCard);
        root.Children().RemoveAtEnd();
        schedulesSection.Children().Append(editorCard);
        editorCard.Padding({0, 0, 0, 0});
        editor.Spacing(0);
        title.MaxLength(512); notes.MaxLength(8192);
        date.CalendarIdentifier(L"GregorianCalendar");
        date.DateFormat(L"{year.full}-{month.integer(2)}-{day.integer(2)}");
        until.CalendarIdentifier(L"GregorianCalendar");
        until.DateFormat(L"{year.full}-{month.integer(2)}-{day.integer(2)}");
        occurrenceDate.CalendarIdentifier(L"GregorianCalendar");
        occurrenceDate.DateFormat(L"{year.full}-{month.integer(2)}-{day.integer(2)}");
        multipleDates.CalendarIdentifier(L"GregorianCalendar");
        multipleDates.SelectionMode(muxc::CalendarViewSelectionMode::Multiple);
        multipleDates.MinWidth(0);
        multipleDates.MaxHeight(320);
        interval.Minimum(1); interval.Maximum(99);
        interval.SmallChange(1); interval.LargeChange(10);
        interval.AcceptsExpression(false);
        interval.SpinButtonPlacementMode(muxc::NumberBoxSpinButtonPlacementMode::Compact);
        interval.MaxWidth(300);
        interval.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        interval.Value(1);
        weekdayChoices.ColumnSpacing(12);
        weekdayChoices.RowSpacing(8);
        for (int column = 0; column < 4; ++column)
            weekdayChoices.ColumnDefinitions().Append(muxc::ColumnDefinition{});
        for (int row = 0; row < 2; ++row)
            weekdayChoices.RowDefinitions().Append(muxc::RowDefinition{});
        for (int day = 0; day < 7; ++day)
        {
            muxc::CheckBox checkbox;
            checkbox.MinWidth(0);
            checkbox.HorizontalAlignment(mux::HorizontalAlignment::Right);
            muxc::TextBlock label;
            label.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::Grid choice;
            choice.ColumnSpacing(6);
            choice.ColumnDefinitions().Append(muxc::ColumnDefinition{});
            muxc::ColumnDefinition controlColumn;
            controlColumn.Width(mux::GridLengthHelper::Auto());
            choice.ColumnDefinitions().Append(controlColumn);
            choice.Children().Append(label);
            muxc::Grid::SetColumn(checkbox, 1);
            choice.Children().Append(checkbox);
            muxc::Grid::SetColumn(choice, day % 4);
            muxc::Grid::SetRow(choice, day / 4);
            weekdayChoices.Children().Append(choice);
            weekdays.push_back(checkbox);
            weekdayLabels.push_back(label);
        }
        start.MinuteIncrement(1); end.MinuteIncrement(1);
        notes.AcceptsReturn(true); notes.TextWrapping(mux::TextWrapping::Wrap); notes.MinHeight(72);
        const auto addRow = [&](SettingRow& row, const mux::FrameworkElement& control,
                                std::wstring_view automationId, double width = 300.0) {
            control.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            row.Initialize(control, width);
            mux::Automation::AutomationProperties::SetAutomationId(control, winrt::hstring(automationId));
            mux::Automation::AutomationProperties::SetLabeledBy(control, row.label);
            editor.Children().Append(row.root);
        };
        addRow(titleRow, title, L"CalendarTitle");
        addRow(scopeRow, scope, L"CalendarScope");
        muxc::Grid occurrenceControls;
        occurrenceControls.ColumnSpacing(8);
        occurrenceControls.ColumnDefinitions().Append(muxc::ColumnDefinition{});
        muxc::ColumnDefinition occurrenceActionColumn;
        occurrenceActionColumn.Width(mux::GridLengthHelper::Auto());
        occurrenceControls.ColumnDefinitions().Append(occurrenceActionColumn);
        occurrenceDate.MinWidth(0);
        occurrenceDate.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        occurrenceControls.Children().Append(occurrenceDate);
        muxc::Grid::SetColumn(editOccurrence, 1);
        occurrenceControls.Children().Append(editOccurrence);
        addRow(occurrenceRow, occurrenceControls, L"CalendarOccurrence", 420.0);
        mux::Automation::AutomationProperties::SetAutomationId(occurrenceDate, L"CalendarOccurrenceDate");
        mux::Automation::AutomationProperties::SetLabeledBy(occurrenceDate, occurrenceRow.label);
        mux::Automation::AutomationProperties::SetAutomationId(editOccurrence, L"CalendarEditOccurrence");
        addRow(modeRow, mode, L"CalendarDateMode");
        addRow(dateRow, date, L"CalendarDate");
        addRow(multipleDatesRow, multipleDates, L"CalendarMultipleDates", 320.0);
        multipleDatesRow.text.VerticalAlignment(mux::VerticalAlignment::Top);
        addRow(intervalRow, interval, L"CalendarInterval");
        addRow(weekdaysRow, weekdayChoices, L"CalendarWeekdays", 360.0);
        addRow(monthDayRow, monthDay, L"CalendarMonthDay");
        addRow(endTypeRow, endType, L"CalendarEndType");
        addRow(untilRow, until, L"CalendarUntil");
        addRow(allDayRow, allDay, L"CalendarAllDay");
        allDay.HorizontalAlignment(mux::HorizontalAlignment::Right);
        allDayRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        addRow(startRow, start, L"CalendarStartTime");
        addRow(endRow, end, L"CalendarEndTime");
        addRow(reminderRow, reminder, L"CalendarReminder");
        addRow(notesRow, notes, L"CalendarNotes");
        notesRow.text.VerticalAlignment(mux::VerticalAlignment::Top);
        muxc::StackPanel buttons; buttons.Orientation(muxc::Orientation::Horizontal); buttons.Spacing(8);
        buttons.HorizontalAlignment(mux::HorizontalAlignment::Right);
        buttons.Children().Append(save); buttons.Children().Append(cancel); buttons.Children().Append(remove);
        if (const auto accent = mux::Application::Current().Resources().TryLookup(winrt::box_value(L"AccentButtonStyle")))
            if (const auto accentStyle = accent.try_as<mux::Style>()) save.Style(accentStyle);
        addRow(actionsRow, buttons, L"CalendarEditorActions", 0.0);
        actionsRow.SetCompactEditor();
        actionsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        presenter_controls::UpdateSettingsSubrows(editor, 0);
        editorCard.Visibility(mux::Visibility::Collapsed);
        auto ctoken = calendarToggle.Toggled([this](const auto&, const auto&) { Commit(); });
        revoke.push_back([c = calendarToggle, ctoken] { c.Toggled(ctoken); });
        for (auto choice : {calendarChoice})
        {
            auto token = choice.SelectionChanged([this](const auto&, const auto&) { Commit(); });
            revoke.push_back([choice, token] { choice.SelectionChanged(token); });
        }
        auto token = allDay.Checked([this](const auto&, const auto&) { UpdateTimeVisibility(); });
        revoke.push_back([c = allDay, token] { c.Checked(token); });
        token = allDay.Unchecked([this](const auto&, const auto&) { UpdateTimeVisibility(); });
        revoke.push_back([c = allDay, token] { c.Unchecked(token); });
        token = interval.ValueChanged([this](const auto&, const auto&) { UpdateModeVisibility(); });
        revoke.push_back([c = interval, token] { c.ValueChanged(token); });
        auto modeToken = mode.SelectionChanged([this](const auto&, const auto&) { UpdateModeVisibility(); });
        revoke.push_back([c = mode, modeToken] { c.SelectionChanged(modeToken); });
        auto datesToken = multipleDates.SelectedDatesChanged([this](const auto&, const auto&) { UpdateModeVisibility(); });
        revoke.push_back([c = multipleDates, datesToken] { c.SelectedDatesChanged(datesToken); });
        auto scopeToken = scope.SelectionChanged([this](const auto&, const auto&) {
            if (updating || editingSeries.id.empty()) return;
            if (!CanReplaceDraft())
            {
                updating = true;
                scope.SelectedIndex(editingSeriesMode ? 1 : 0);
                updating = false;
                return;
            }
            if (scope.SelectedIndex() == 1) EditSeries(editingSeries);
            else if (scope.SelectedIndex() == 0 && !LoadOccurrence())
            {
                updating = true;
                scope.SelectedIndex(editingSeriesMode ? 1 : 0);
                updating = false;
            }
        });
        revoke.push_back([c = scope, scopeToken] { c.SelectionChanged(scopeToken); });
        auto endToken = endType.SelectionChanged([this](const auto&, const auto&) { UpdateModeVisibility(); });
        revoke.push_back([c = endType, endToken] { c.SelectionChanged(endToken); });
        Click(editOccurrence, [this] { LoadOccurrence(); });
        auto selection = list.SelectionChanged([this](const auto&, const auto&) {
            if (updating || closed || !active) return;
            if (!CanReplaceDraft())
            {
                updating = true;
                list.SelectedIndex(editingListIndex);
                updating = false;
                return;
            }
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
            if (!CanReplaceDraft()) return;
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
        titleRow.SetText(L("settings.calendar.title"));
        dateRow.SetText(L("settings.calendar.date"));
        multipleDatesRow.SetText(L("settings.calendar.chooseDates"));
        untilRow.SetText(L("settings.calendar.endDate"));
        occurrenceRow.SetText(L("settings.calendar.originalDate"));
        intervalRow.SetText(L("settings.calendar.interval"));
        weekdaysRow.SetText(L("settings.calendar.mode.weekly"));
        endTypeRow.SetText(L("settings.calendar.endDate"));
        const int selectedEndType = endType.SelectedIndex();
        endType.Items().Clear();
        endType.Items().Append(winrt::box_value(L("settings.calendar.neverEnds")));
        endType.Items().Append(winrt::box_value(L("settings.calendar.endsOn")));
        endType.SelectedIndex(selectedEndType >= 0 ? selectedEndType : 0);
        modeRow.SetText(L("settings.calendar.dateMode"));
        scopeRow.SetText(L("settings.calendar.scope"));
        monthDayRow.SetText(L("settings.calendar.monthDay"));
        const int selectedMode = mode.SelectedIndex();
        mode.Items().Clear();
        for (const auto& key : {"settings.calendar.mode.single", "settings.calendar.mode.dates",
            "settings.calendar.mode.weekly", "settings.calendar.mode.monthly"})
        {
            muxc::ComboBoxItem item;
            item.Content(winrt::box_value(L(key)));
            mode.Items().Append(item);
        }
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
        {
            const auto label = L(("settings.calendar.weekday." + std::to_string(index + 1)).c_str());
            weekdayLabels[index].Text(label);
            mux::Automation::AutomationProperties::SetName(weekdays[index], label);
            mux::Automation::AutomationProperties::SetAutomationId(weekdays[index],
                L"CalendarWeekday" + std::to_wstring(index + 1));
        }
        editOccurrence.Content(winrt::box_value(L("settings.calendar.editOccurrence")));
        startRow.SetText(L("settings.calendar.start"));
        endRow.SetText(L("settings.calendar.end"));
        reminderRow.SetText(L("settings.calendar.reminder"));
        const auto reminderSelection = reminder.SelectedIndex();
        reminder.Items().Clear();
        for (const auto& label : {L("settings.calendar.reminder.-1"),
                L("settings.calendar.reminder.0"), L("settings.calendar.reminder.5"),
                L("settings.calendar.reminder.15"), L("settings.calendar.reminder.30"),
                L("settings.calendar.reminder.60"), L("settings.calendar.reminder.1440")})
            reminder.Items().Append(winrt::box_value(label));
        reminder.SelectedIndex(reminderSelection >= 0 ? reminderSelection : 0);
        notesRow.SetText(L("settings.calendar.notes"));
        allDayRow.SetText(L("settings.calendar.allDay"));
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
        if (explicitRefresh && !CanReplaceDraft()) return;
        try
        {
            const auto result = actions.events(generation);
            const auto seriesResult = actions.series(generation);
            if (!result || !seriesResult) { SetError(L("settings.calendar.failed")); return; }
            bool same = result->size() == events.size() && seriesResult->size() == series.size();
            for (size_t i = 0; same && i < events.size(); ++i)
                same = (*result)[i].id == events[i].id && (*result)[i].revision == events[i].revision;
            for (size_t i = 0; same && i < series.size(); ++i)
                same = (*seriesResult)[i].id == series[i].id && (*seriesResult)[i].revision == series[i].revision;
            if (same && !explicitRefresh) return;
            updating = true;
            events = *result; series = *seriesResult; rows.clear(); list.Items().Clear();
            int selected = -1;
            const auto appendRow = [&](const std::string& name, const std::wstring& details) {
                muxc::StackPanel information; information.Spacing(4);
                muxc::TextBlock titleText, detailText;
                titleText.Text(winrt::to_hstring(name)); titleText.TextWrapping(mux::TextWrapping::Wrap);
                detailText.Text(details); detailText.FontSize(12); detailText.Opacity(.72);
                detailText.TextWrapping(mux::TextWrapping::Wrap);
                information.Children().Append(titleText); information.Children().Append(detailText);
                muxc::ListViewItem item; item.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
                item.Padding({16, 12, 16, 12}); item.Content(information); list.Items().Append(item);
            };
            for (size_t i = 0; i < events.size(); ++i)
            {
                const auto& event = events[i];
                rows.push_back({false, i});
                appendRow(event.title, std::wstring(winrt::to_hstring(event.date).c_str()));
                if (editingSeries.id.empty() && event.id == editing.id) selected = static_cast<int>(rows.size() - 1);
            }
            for (size_t i = 0; i < series.size(); ++i)
            {
                const auto& item = series[i];
                rows.push_back({true, i});
                const auto summary = RuleSummary(item.rule);
                const auto firstDate = item.rule.kind == "dates" && !item.rule.dates.empty()
                    ? item.rule.dates.front() : item.rule.startDate;
                std::wstring label = winrt::to_hstring(firstDate).c_str();
                label += L" · " + summary;
                appendRow(item.event.title, label);
                if (item.id == editingSeries.id) selected = static_cast<int>(rows.size() - 1);
            }
            list.SelectedIndex(selected); updating = false;
            editingListIndex = selected;
            listHeading.Text(L(rows.empty() ? "settings.calendar.empty" : "settings.calendar.events"));
            if (explicitRefresh)
            {
                SetError(L"");
                if (selected >= 0)
                {
                    const auto row = rows[static_cast<std::size_t>(selected)];
                    if (row.isSeries) EditSeries(series[row.index]);
                    else Edit(events[row.index]);
                }
            }
        }
        catch (...) { updating = false; SetError(L("settings.calendar.failed")); }
    }
    void UpdateTimeVisibility()
    {
        const auto visibility = allDay.IsChecked() && allDay.IsChecked().Value() ? mux::Visibility::Collapsed : mux::Visibility::Visible;
        startRow.root.Visibility(visibility); endRow.root.Visibility(visibility);
        presenter_controls::UpdateSettingsSubrows(editor, 0);
    }
    void SetError(const std::wstring& message)
    {
        scheduleFeedback.Message(message);
        scheduleFeedback.IsOpen(!message.empty());
    }

    void UpdateModeVisibility()
    {
        if (updating) return;
        if (editingSeriesMode && mode.SelectedIndex() == 0) mode.SelectedIndex(1);
        if (mode.Items().Size() > 0)
            mode.Items().GetAt(0).as<muxc::ComboBoxItem>().IsEnabled(!editingSeriesMode);
        const int selected = editingSeriesMode || editing.id.empty() ? mode.SelectedIndex() : 0;
        if (selected >= 2) intervalRow.SetText(L(selected == 2 ?
            "settings.calendar.intervalWeeks" : "settings.calendar.intervalMonths"));
        const bool rule = editingSeriesMode || editing.id.empty();
        modeRow.root.Visibility(rule ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        scopeRow.root.Visibility(editingSeries.id.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        dateRow.root.Visibility(selected == 1 ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        dateRow.SetText(L(selected >= 2 ? "settings.calendar.startDate" : "settings.calendar.date"));
        multipleDatesRow.root.Visibility(selected == 1 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        multipleDatesRow.SetText(L("settings.calendar.chooseDates"), L("settings.calendar.selectedDates") +
            L" " + std::to_wstring(multipleDates.SelectedDates().Size()));
        intervalRow.root.Visibility(selected >= 2 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        weekdaysRow.root.Visibility(selected == 2 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        monthDayRow.root.Visibility(selected == 3 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        endTypeRow.root.Visibility(selected >= 2 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        untilRow.root.Visibility(selected >= 2 && endType.SelectedIndex() == 1 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        occurrenceRow.root.Visibility(editingSeries.id.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        actionsRow.SetText(L(editing.id.empty() && editingSeries.id.empty() ?
            "settings.calendar.add" : "settings.calendar.edit"));
        remove.Content(winrt::box_value(L(editingSeriesMode ? "settings.calendar.deleteSeries" :
            editing.seriesId.empty() ? "app.settings.delete" : "settings.calendar.deleteOccurrence")));
        remove.Visibility(editing.id.empty() && editingSeries.id.empty() ?
            mux::Visibility::Collapsed : mux::Visibility::Visible);
        presenter_controls::UpdateSettingsSubrows(editor, 0);
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
    template<class Picker> static void IncludeDate(const Picker& picker,
        const winrt::Windows::Foundation::DateTime& value)
    {
        // WinUI's default picker range is narrower than persisted calendars.
        if (value < picker.MinDate()) picker.MinDate(value);
        if (value > picker.MaxDate()) picker.MaxDate(value);
    }
    static void SetDate(const muxc::CalendarDatePicker& picker, const std::string& value)
    {
        if (value.empty()) { picker.Date(nullptr); return; }
        const auto selected = PickerDate(value);
        IncludeDate(picker, selected);
        picker.Date(winrt::box_value(selected).as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::DateTime>>());
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
    std::wstring DraftKey() const
    {
        std::wstring result;
        const auto append = [&](std::wstring_view text) {
            result += std::to_wstring(text.size()) + L":";
            result.append(text);
        };
        append(title.Text()); append(notes.Text());
        append(std::to_wstring(mode.SelectedIndex()));
        append(std::to_wstring(monthDay.SelectedIndex()));
        append(std::to_wstring(endType.SelectedIndex()));
        append(std::to_wstring(reminder.SelectedIndex()));
        append(interval.Text());
        append(allDay.IsChecked() && allDay.IsChecked().Value() ? L"1" : L"0");
        append(std::to_wstring(Minutes(start))); append(std::to_wstring(Minutes(end)));
        for (const auto picker : {date, until})
            append(picker.Date() ? winrt::to_hstring(DateString(picker.Date().Value())) : winrt::hstring{});
        std::vector<std::string> selectedDates;
        for (const auto selected : multipleDates.SelectedDates()) selectedDates.push_back(DateString(selected));
        std::sort(selectedDates.begin(), selectedDates.end());
        for (const auto& selected : selectedDates) append(winrt::to_hstring(selected));
        for (const auto& checkbox : weekdays) append(checkbox.IsChecked() && checkbox.IsChecked().Value() ? L"1" : L"0");
        return result;
    }
    bool CanReplaceDraft()
    {
        if (editorCard.Visibility() != mux::Visibility::Visible || savedDraft.empty() || DraftKey() == savedDraft)
            return true;
        SetError(L("settings.calendar.unsavedDraft"));
        return false;
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
        mode.Items().GetAt(0).as<muxc::ComboBoxItem>().IsEnabled(true);
        mode.SelectedIndex(0);
        scope.SelectedIndex(0);
        title.Text(winrt::to_hstring(event.title));
        const auto selectedDate = PickerDate(event.date);
        multipleDates.SelectedDates().Clear();
        IncludeDate(multipleDates, selectedDate);
        multipleDates.SelectedDates().Append(selectedDate);
        multipleDates.SetDisplayDate(selectedDate);
        interval.Value(1);
        const auto info = calendar::CalendarService::GetDateInfo(event.date);
        for (std::size_t index = 0; index < weekdays.size(); ++index)
            weekdays[index].IsChecked(info && info->weekday == static_cast<int>(index + 1));
        monthDay.SelectedIndex(info ? info->day : 1);
        endType.SelectedIndex(0);
        until.Date(nullptr);
        SetDate(date, event.date);
        SetDate(occurrenceDate, event.occurrenceDate.empty() ? event.date : event.occurrenceDate);
        allDay.IsChecked(event.allDay);
        start.Time(std::chrono::minutes(event.startMinutes)); end.Time(std::chrono::minutes(event.endMinutes));
        UpdateTimeVisibility();
        notes.Text(winrt::to_hstring(event.notes)); for (size_t i = 0; i < reminderValues.size(); ++i) if (reminderValues[i] == event.reminderMinutes) reminder.SelectedIndex(static_cast<int>(i));
        remove.IsEnabled(!event.id.empty()); SetError(L""); editorCard.Visibility(mux::Visibility::Visible);
        updating = false; UpdateModeVisibility();
        editingListIndex = list.SelectedIndex();
        savedDraft = DraftKey();
    }
    void EditSeries(const calendar::CalendarSeries& item)
    {
        updating = true;
        const auto preferredOccurrence = item.id == editingSeries.id && occurrenceDate.Date()
            ? DateString(occurrenceDate.Date().Value()) : std::string{};
        editingSeries = item;
        editingSeriesMode = true;
        editing = item.event;
        confirmedRule.clear();
        title.Text(winrt::to_hstring(item.event.title));
        notes.Text(winrt::to_hstring(item.event.notes));
        allDay.IsChecked(item.event.allDay);
        start.Time(std::chrono::minutes(item.event.startMinutes));
        end.Time(std::chrono::minutes(item.event.endMinutes));
        for (size_t index = 0; index < reminderValues.size(); ++index)
            if (reminderValues[index] == item.event.reminderMinutes) reminder.SelectedIndex(static_cast<int>(index));
        const auto startDate = PickerDate(item.rule.startDate);
        SetDate(date, item.rule.startDate);
        SetDate(occurrenceDate, CalendarEditorOccurrenceDate(item, preferredOccurrence).value_or(std::string{}));
        multipleDates.SelectedDates().Clear();
        for (const auto& selected : item.rule.dates)
            IncludeDate(multipleDates, PickerDate(selected));
        for (const auto& selected : item.rule.dates)
            multipleDates.SelectedDates().Append(PickerDate(selected));
        IncludeDate(multipleDates, startDate);
        multipleDates.SetDisplayDate(startDate);
        mode.SelectedIndex(item.rule.kind == "dates" ? 1 : item.rule.kind == "weekly" ? 2 : 3);
        scope.SelectedIndex(1);
        interval.Value(item.rule.interval);
        monthDay.SelectedIndex(item.rule.monthDay);
        for (std::size_t index = 0; index < weekdays.size(); ++index)
        {
            const auto weekday = static_cast<int>(index + 1);
            const bool checked = std::find(item.rule.weekdays.begin(), item.rule.weekdays.end(), weekday) != item.rule.weekdays.end();
            weekdays[index].IsChecked(checked);
        }
        endType.SelectedIndex(item.rule.endDate.empty() ? 0 : 1);
        SetDate(until, item.rule.endDate);
        remove.IsEnabled(true); SetError(L""); editorCard.Visibility(mux::Visibility::Visible);
        updating = false; UpdateTimeVisibility(); UpdateModeVisibility();
        editingListIndex = list.SelectedIndex();
        savedDraft = DraftKey();
    }
    bool LoadOccurrence()
    {
        if (!CanReplaceDraft()) return false;
        if (editingSeries.id.empty() || !occurrenceDate.Date() || !actions.occurrence)
        { SetError(L("settings.calendar.invalidOccurrence")); return false; }
        const auto original = DateString(occurrenceDate.Date().Value());
        const auto result = actions.occurrence(generation, editingSeries.id + "/" + original);
        if (!result) { SetError(L("settings.calendar.invalidOccurrence")); return false; }
        Edit(*result);
        return true;
    }
    void Save()
    {
        if (!actions.mutate) return;
        auto event = editing;
        event.title = winrt::to_string(title.Text());
        event.notes = winrt::to_string(notes.Text()); event.allDay = allDay.IsChecked() && allDay.IsChecked().Value();
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
                rule.endDate = endType.SelectedIndex() == 0 ? std::string{} :
                    until.Date() ? DateString(until.Date().Value()) : std::string{};
                const double intervalValue = interval.Value();
                rule.interval = std::isfinite(intervalValue) && intervalValue >= 1 && intervalValue <= 99 &&
                    std::floor(intervalValue) == intervalValue ? static_cast<int>(intervalValue) : 0;
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
                    (endType.SelectedIndex() != 0 && (!calendar::CalendarService::GetDateInfo(rule.endDate) ||
                        rule.endDate < rule.startDate)))) ||
                (selectedMode == 2 && rule.weekdays.empty()) ||
                (selectedMode == 3 && (rule.monthDay < 0 || rule.monthDay > 31));
            if (invalid) { SetError(L("settings.calendar.invalid")); return; }
        }
        else event.date = SelectedDate();
        if (minutes < -1 ||
            event.title.empty() || !calendar::CalendarService::GetDateInfo(event.date) ||
            event.startMinutes < 0 || event.endMinutes < event.startMinutes)
        { SetError(L("settings.calendar.invalid")); return; }
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
                    SetError(L("settings.calendar.confirmExceptions"));
                    return;
                }
            }
            const auto result = seriesMode && actions.mutateSeries
                ? actions.mutateSeries(generation, candidate, false)
                : seriesMode ? calendar::MutationResult{false, {}, 0, "unavailable"}
                    : actions.mutate(generation, event, false);
            if (!result.ok) { SetError(L(result.error == "conflict" ? "settings.calendar.conflict" : "settings.calendar.failed")); return; }
            editing = {}; editingSeries = {}; editorCard.Visibility(mux::Visibility::Collapsed); Refresh(true);
        }
        catch (...) { SetError(L("settings.calendar.failed")); }
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
            confirmation.Title(winrt::box_value(L(removeSeries ? "settings.calendar.deleteSeries" :
                target.seriesId.empty() ? "settings.calendar.confirmDelete" : "settings.calendar.deleteOccurrence")));
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
            if (!result.ok) { SetError(L(result.error == "conflict" ? "settings.calendar.conflict" : "settings.calendar.failed")); co_return; }
            editing = {}; editingSeries = {}; editorCard.Visibility(mux::Visibility::Collapsed); Refresh(true);
        }
        catch (...) { dialog = nullptr; if (!closed) SetError(L("settings.calendar.failed")); }
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
