#include "pch.h"
#include "calendar_page_presenter.h"
#include "settings_presenter_controls.h"
#include "../l10n.h"
#include <cmath>
#include <cstdio>

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
    muxc::ToggleSwitch calendarToggle, holidayToggle, allDay;
    muxc::ComboBox calendarChoice, regionChoice;
    SettingRow calendarEnabledRow, calendarRow, holidayEnabledRow, regionRow;
    muxc::TextBlock description, listHeading, error;
    muxc::ListView list;
    muxc::TextBox title, date, start, end, notes;
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
    muxc::StackPanel Card(const mux::Style& style)
    {
        muxc::Border border;
        border.Style(style);
        muxc::StackPanel content;
        content.Spacing(12);
        border.Child(content);
        root.Children().Append(border);
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
        editor = Card(style);
        title.MaxLength(512); date.MaxLength(10); start.MaxLength(5); end.MaxLength(5); notes.MaxLength(8192);
        date.PlaceholderText(L"YYYY-MM-DD"); start.PlaceholderText(L"HH:mm"); end.PlaceholderText(L"HH:mm");
        notes.AcceptsReturn(true); notes.TextWrapping(mux::TextWrapping::Wrap); notes.MaxHeight(180);
        for (int value : reminderValues) reminder.Items().Append(winrt::box_value(std::to_wstring(value)));
        reminder.SelectedIndex(0);
        editor.Children().Append(title); editor.Children().Append(date); editor.Children().Append(allDay);
        editor.Children().Append(start); editor.Children().Append(end); editor.Children().Append(reminder);
        editor.Children().Append(notes);
        muxc::StackPanel buttons; buttons.Orientation(muxc::Orientation::Horizontal); buttons.Spacing(8);
        buttons.Children().Append(save); buttons.Children().Append(cancel); buttons.Children().Append(remove);
        editor.Children().Append(buttons);
        error.TextWrapping(mux::TextWrapping::Wrap); root.Children().Append(error);
        editor.Visibility(mux::Visibility::Collapsed);
        auto ctoken = calendarToggle.Toggled([this](const auto&, const auto&) { Commit(); });
        revoke.push_back([c = calendarToggle, ctoken] { c.Toggled(ctoken); });
        auto htoken = holidayToggle.Toggled([this](const auto&, const auto&) { Commit(); });
        revoke.push_back([c = holidayToggle, htoken] { c.Toggled(htoken); });
        for (auto choice : {calendarChoice, regionChoice})
        {
            auto token = choice.SelectionChanged([this](const auto&, const auto&) { Commit(); });
            revoke.push_back([choice, token] { choice.SelectionChanged(token); });
        }
        auto token = allDay.Toggled([this](const auto&, const auto&) { start.IsEnabled(!allDay.IsOn()); end.IsEnabled(!allDay.IsOn()); });
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
        Click(cancel, [this] { editor.Visibility(mux::Visibility::Collapsed); editing = {}; list.SelectedIndex(-1); });
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
        notes.Header(winrt::box_value(L("settings.calendar.notes")));
        allDay.Header(winrt::box_value(L("settings.calendar.allDay")));
        add.Content(winrt::box_value(L("settings.calendar.add")));
        refresh.Content(winrt::box_value(L("settings.calendar.refresh")));
        save.Content(winrt::box_value(L("settings.calendar.save")));
        cancel.Content(winrt::box_value(L("app.settings.cancel")));
        remove.Content(winrt::box_value(L("app.settings.delete")));
        calendars = calendar::CalendarOptions(Locale::Instance().GetLanguage());
        regions = calendar::HolidayRegions(Locale::Instance().GetLanguage());
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
    static std::wstring Time(int minutes)
    { wchar_t value[6]{}; swprintf_s(value, L"%02d:%02d", minutes / 60, minutes % 60); return value; }
    static int Minutes(const winrt::hstring& text)
    {
        if (text.size() != 5 || text[2] != L':' || text[0] < L'0' || text[0] > L'9' || text[1] < L'0' || text[1] > L'9' || text[3] < L'0' || text[3] > L'9' || text[4] < L'0' || text[4] > L'9') return -1;
        const int h = (text[0] - L'0') * 10 + text[1] - L'0', m = (text[3] - L'0') * 10 + text[4] - L'0';
        return h < 24 && m < 60 ? h * 60 + m : -1;
    }
    void Edit(const calendar::CalendarEvent& event)
    {
        editing = event;
        title.Text(winrt::to_hstring(event.title)); date.Text(winrt::to_hstring(event.date));
        allDay.IsOn(event.allDay); start.Text(Time(event.startMinutes)); end.Text(Time(event.endMinutes));
        start.IsEnabled(!event.allDay); end.IsEnabled(!event.allDay);
        notes.Text(winrt::to_hstring(event.notes)); for (size_t i = 0; i < reminderValues.size(); ++i) if (reminderValues[i] == event.reminderMinutes) reminder.SelectedIndex(static_cast<int>(i));
        remove.IsEnabled(!event.id.empty()); error.Text(L""); editor.Visibility(mux::Visibility::Visible);
    }
    void Save()
    {
        if (!actions.mutate) return;
        auto event = editing;
        event.title = winrt::to_string(title.Text()); event.date = winrt::to_string(date.Text());
        event.notes = winrt::to_string(notes.Text()); event.allDay = allDay.IsOn();
        event.startMinutes = event.allDay ? 0 : Minutes(start.Text());
        event.endMinutes = event.allDay ? 0 : Minutes(end.Text());
        const int reminderIndex = reminder.SelectedIndex();
        const int minutes = reminderIndex >= 0 && static_cast<size_t>(reminderIndex) < reminderValues.size() ? reminderValues[reminderIndex] : -2;
        if (!std::isfinite(minutes) || std::floor(minutes) != minutes || minutes < -1 || minutes > 10080 ||
            event.title.empty() || !calendar::CalendarService::GetDateInfo(event.date) ||
            event.startMinutes < 0 || event.endMinutes < event.startMinutes)
        { error.Text(L("settings.calendar.invalid")); return; }
        event.reminderMinutes = static_cast<int>(minutes);
        try
        {
            const auto result = actions.mutate(generation, event, false);
            if (!result.ok) { error.Text(L(result.error == "conflict" ? "settings.calendar.conflict" : "settings.calendar.failed")); return; }
            editing = {}; editor.Visibility(mux::Visibility::Collapsed); Refresh(true);
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
            editing = {}; editor.Visibility(mux::Visibility::Collapsed); Refresh(true);
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
    { impl_->editing = {}; impl_->editor.Visibility(mux::Visibility::Collapsed); }
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
