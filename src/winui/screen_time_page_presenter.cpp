#include "pch.h"
#include "screen_time_page_presenter.h"

#include "data/data_paths.h"
#include "settings_presenter_controls.h"
#include "system/screen_time/screen_time_tracker.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <string>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.h>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = mux::Controls;
namespace muxm = mux::Media;

namespace
{
std::wstring Format(std::wstring pattern, std::wstring_view first,
    std::wstring_view second = {})
{
    const auto replace = [&](std::wstring_view token, std::wstring_view value) {
        for (auto at = pattern.find(token); at != std::wstring::npos;
            at = pattern.find(token, at))
        {
            pattern.replace(at, token.size(), value);
            at += value.size();
        }
    };
    replace(L"{0}", first);
    replace(L"{1}", second);
    return pattern;
}

std::wstring Duration(const std::function<std::wstring(std::string_view)>& text,
    std::int64_t seconds)
{
    if (seconds < 0) seconds = 0;
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    if (hours > 0)
    {
        return Format(text("settings.screenTime.duration"),
            std::to_wstring(hours), std::to_wstring(minutes));
    }
    if (seconds < 60)
    {
        return Format(text("settings.screenTime.durationSeconds"),
            std::to_wstring(seconds));
    }
    return Format(text("settings.screenTime.durationMinutes"),
        std::to_wstring(minutes));
}

muxm::SolidColorBrush Teal(std::uint8_t alpha)
{
    return muxm::SolidColorBrush(
        winrt::Windows::UI::Color{alpha, 22, 140, 136});
}

mux::FrameworkElement UsageTrack(std::int64_t seconds, std::int64_t peak)
{
    muxc::Border track;
    track.Width(140);
    track.Height(4);
    track.VerticalAlignment(mux::VerticalAlignment::Center);
    track.CornerRadius({2, 2, 2, 2});
    track.Background(Teal(36));
    if (seconds > 0 && peak > 0)
    {
        muxc::Border fill;
        fill.Width(std::max(4.0, 140.0 * static_cast<double>(seconds) / peak));
        fill.Height(4);
        fill.HorizontalAlignment(mux::HorizontalAlignment::Left);
        fill.CornerRadius({2, 2, 2, 2});
        fill.Background(Teal(255));
        track.Child(fill);
    }
    return track;
}

mux::UIElement UsageChart(const std::vector<std::int64_t>& values)
{
    muxc::Grid chart;
    chart.Height(72);
    chart.ColumnSpacing(values.size() > 12 ? 3.0 : 10.0);
    std::int64_t peak = 1;
    for (const auto value : values) peak = std::max(peak, value);
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        muxc::ColumnDefinition column;
        column.Width(mux::GridLengthHelper::FromValueAndType(
            1, mux::GridUnitType::Star));
        chart.ColumnDefinitions().Append(column);
        if (values[index] <= 0) continue;
        muxc::Border bar;
        bar.Height(std::max(3.0, 72.0 * static_cast<double>(values[index]) / peak));
        bar.VerticalAlignment(mux::VerticalAlignment::Bottom);
        bar.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        bar.CornerRadius({2, 2, 2, 2});
        bar.Background(Teal(255));
        muxc::Grid::SetColumn(bar, static_cast<int>(index));
        chart.Children().Append(bar);
    }
    return chart;
}

std::string TodayDate()
{
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    char text[16]{};
    sprintf_s(text, "%04d-%02d-%02d",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return text;
}

std::wstring Widen(std::string_view value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring text(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), text.data(), size);
    return text;
}
}

struct ScreenTimePagePresenter::Impl
{
    LocalizeCallback localize;
    ScreenTimePageActions actions;
    mux::Style cardStyle{nullptr};
    muxc::StackPanel root{nullptr};
    muxc::TextBlock paused{nullptr};
    muxc::TextBlock total{nullptr};
    muxc::ComboBox dayList{nullptr};
    muxc::StackPanel report{nullptr};
    muxc::ToggleSwitch enabled{nullptr};
    muxc::Button clear{nullptr};
    muxc::SelectorBar range{nullptr};
    muxc::SelectorBarItem dayItem{nullptr};
    muxc::SelectorBarItem weekItem{nullptr};
    presenter_controls::SettingRow enabledRow;
    presenter_controls::SettingRow clearRow;
    mux::DispatcherTimer timer{nullptr};
    muxc::TextBlock enabledLabel;
    std::uint64_t generation = 0;
    bool enabledValue = true;
    bool week = false;
    std::string selectedDate;
    winrt::event_token dayChanged;
    bool active = false;
    bool closed = false;
    bool updating = false;
    bool handlersBound = false;
    winrt::event_token toggled;
    winrt::event_token cleared;
    winrt::event_token ranged;
    winrt::event_token ticked;

    std::wstring L(std::string_view key) const
    {
        return localize ? localize(key) : std::wstring{};
    }

    void Build()
    {
        root = muxc::StackPanel{};
        root.Spacing(12);

        muxc::TextBlock heading;
        heading.Text(L("settings.screenTime.page"));
        heading.FontSize(16);
        muxc::StackPanel body;
        body.Spacing(0);

        enabled = muxc::ToggleSwitch{};
        enabled.MinWidth(0);
        enabled.OnContent(winrt::box_value(L""));
        enabled.OffContent(winrt::box_value(L""));
        enabledRow.Initialize(enabled);
        enabledLabel = enabledRow.label;
        body.Children().Append(enabledRow.root);

        clear = muxc::Button{};
        clearRow.Initialize(clear);
        body.Children().Append(clearRow.root);
        muxc::Border card;
        presenter_controls::InitializeSettingsGroup(
            card, heading, body, cardStyle);
        presenter_controls::UpdateSettingsSubrows(body, 0);
        root.Children().Append(card);

        paused = muxc::TextBlock{};
        paused.TextWrapping(mux::TextWrapping::Wrap);
        paused.Opacity(0.72);
        root.Children().Append(paused);

        range = muxc::SelectorBar{};
        dayItem = muxc::SelectorBarItem{};
        weekItem = muxc::SelectorBarItem{};
        range.Items().Append(dayItem);
        range.Items().Append(weekItem);
        root.Children().Append(range);

        total = muxc::TextBlock{};
        total.FontSize(20);
        root.Children().Append(total);
        dayList = muxc::ComboBox{};
        dayList.HorizontalAlignment(mux::HorizontalAlignment::Left);
        dayList.MinWidth(160);
        root.Children().Append(dayList);

        report = muxc::StackPanel{};
        report.Spacing(10);
        root.Children().Append(report);

        timer = mux::DispatcherTimer{};
        timer.Interval(std::chrono::seconds(15));
    }

    void Bind()
    {
        toggled = enabled.Toggled([this](const auto&, const auto&) {
            if (updating || closed || !actions.commitGeneral) return;
            const bool value = enabled.IsOn();
            enabledValue = value;
            actions.commitGeneral(generation, [value](GeneralSettings& settings) {
                settings.screenTimeEnabled = value;
            });
            ShowPaused();
        });
        cleared = clear.Click([this](const auto&, const auto&) {
            if (closed || !actions.clear) return;
            if (actions.clear(generation)) Reload();
        });
        ranged = range.SelectionChanged([this](const auto&, const auto&) {
            if (updating || closed) return;
            week = range.SelectedItem() == weekItem;
            Reload();
        });
        ticked = timer.Tick([this](const auto&, const auto&) {
            if (active && !closed) Reload();
        });
        dayChanged = dayList.SelectionChanged([this](const auto&, const auto&) {
            if (updating || closed) return;
            if (const auto selected = dayList.SelectedItem().try_as<winrt::hstring>())
            {
                selectedDate = winrt::to_string(*selected);
                Reload();
            }
        });
        handlersBound = true;
    }

    void Unbind()
    {
        if (!handlersBound) return;
        handlersBound = false;
        if (enabled) enabled.Toggled(toggled);
        if (clear) clear.Click(cleared);
        if (range) range.SelectionChanged(ranged);
        if (timer) timer.Tick(ticked);
        if (dayList) dayList.SelectionChanged(dayChanged);
    }

    void ShowPaused()
    {
        paused.Text(L("settings.screenTime.paused"));
        paused.Visibility(enabledValue
            ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    }

    void Reload()
    {
        if (!report) return;
        report.Children().Clear();
        const auto history = snowdesktop::screen_time::LoadReports(
            GetDataFilePath(L"SnowDesktop.screen-time.json"));
        const std::string today = TodayDate();
        std::vector<snowdesktop::screen_time::DayReport> recent;
        for (int offset = 6; offset >= 0; --offset)
        {
            std::tm local{};
            const auto now = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            localtime_s(&local, &now);
            local.tm_mday -= offset;
            local.tm_hour = 12;
            mktime(&local);
            char text[16]{};
            sprintf_s(text, "%04d-%02d-%02d",
                local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
            snowdesktop::screen_time::DayReport day;
            day.date = text;
            const auto found = std::find_if(history.begin(), history.end(),
                [&](const auto& item) { return item.date == day.date; });
            if (found != history.end()) day = *found;
            recent.push_back(std::move(day));
        }

        if (week)
        {
            dayList.Visibility(mux::Visibility::Collapsed);
            std::int64_t sum = 0;
            std::map<std::string, std::int64_t> apps;
            std::vector<std::int64_t> totals;
            totals.reserve(recent.size());
            for (const auto& day : recent)
            {
                sum += day.totalSeconds;
                totals.push_back(day.totalSeconds);
                for (const auto& app : day.apps)
                    apps[app.name] += app.seconds;
            }
            total.Text(Duration(localize, sum));
            report.Children().Append(UsageChart(totals));
            std::vector<snowdesktop::screen_time::AppTotal> ranked;
            for (const auto& [name, seconds] : apps)
                ranked.push_back({name, seconds});
            std::sort(ranked.begin(), ranked.end(),
                [](const auto& left, const auto& right) {
                    return left.seconds > right.seconds;
                });
            AppendApps(ranked);
            return;
        }

        if (selectedDate.empty()) selectedDate = today;
        updating = true;
        dayList.Items().Clear();
        dayList.Visibility(mux::Visibility::Visible);
        int selectedIndex = static_cast<int>(recent.size()) - 1;
        for (int index = 0; index < static_cast<int>(recent.size()); ++index)
        {
            dayList.Items().Append(winrt::box_value(Widen(recent[index].date)));
            if (recent[index].date == selectedDate) selectedIndex = index;
        }
        dayList.SelectedIndex(selectedIndex);
        if (selectedIndex >= 0 &&
            selectedIndex < static_cast<int>(recent.size()))
            selectedDate = recent[static_cast<std::size_t>(selectedIndex)].date;
        updating = false;
        const auto selected = std::find_if(recent.begin(), recent.end(),
            [&](const auto& day) { return day.date == selectedDate; });
        const auto& day = selected == recent.end() ? recent.back() : *selected;
        total.Text(Duration(localize, day.totalSeconds));
        report.Children().Append(UsageChart(
            std::vector<std::int64_t>(day.hours.begin(), day.hours.end())));
        AppendApps(day.apps);
    }

    void AppendApps(const std::vector<snowdesktop::screen_time::AppTotal>& apps)
    {
        if (apps.empty())
        {
            muxc::TextBlock empty;
            empty.Text(L("settings.screenTime.empty"));
            empty.Opacity(0.72);
            report.Children().Append(empty);
            return;
        }
        const auto peak = std::max<std::int64_t>(1, apps.front().seconds);
        const auto limit = std::min<std::size_t>(apps.size(), 30);
        for (std::size_t index = 0; index < limit; ++index)
        {
            const auto& app = apps[index];
            muxc::Grid row;
            row.ColumnSpacing(12);
            muxc::ColumnDefinition nameColumn;
            nameColumn.Width(mux::GridLengthHelper::FromValueAndType(
                1, mux::GridUnitType::Star));
            muxc::ColumnDefinition barColumn;
            barColumn.Width(mux::GridLengthHelper::FromValueAndType(
                160, mux::GridUnitType::Pixel));
            muxc::ColumnDefinition valueColumn;
            valueColumn.Width(mux::GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(nameColumn);
            row.ColumnDefinitions().Append(barColumn);
            row.ColumnDefinitions().Append(valueColumn);
            muxc::TextBlock name;
            name.Text(Widen(app.name));
            name.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            name.VerticalAlignment(mux::VerticalAlignment::Center);
            const auto bar = UsageTrack(app.seconds, peak);
            muxc::TextBlock amount;
            amount.Text(Duration(localize, app.seconds));
            amount.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::Grid::SetColumn(name, 0);
            muxc::Grid::SetColumn(bar, 1);
            muxc::Grid::SetColumn(amount, 2);
            row.MinHeight(28);
            row.Children().Append(name);
            row.Children().Append(bar);
            row.Children().Append(amount);
            report.Children().Append(row);
        }
    }
};

ScreenTimePagePresenter::ScreenTimePagePresenter(
    LocalizeCallback localize, const mux::Style& cardStyle)
    : impl_(std::make_shared<Impl>())
{
    impl_->localize = std::move(localize);
    impl_->cardStyle = cardStyle;
    impl_->Build();
    impl_->Bind();
    RefreshLocalizedText();
}

ScreenTimePagePresenter::~ScreenTimePagePresenter()
{
    if (impl_) impl_->Unbind();
}

void ScreenTimePagePresenter::SetActions(ScreenTimePageActions actions)
{
    impl_->actions = std::move(actions);
}

mux::UIElement ScreenTimePagePresenter::Content() const
{
    return impl_->root;
}

void ScreenTimePagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    SetCollectionState(snapshot.generation,
        snapshot.values.general.screenTimeEnabled);
}

void ScreenTimePagePresenter::SetCollectionState(
    std::uint64_t generation, bool enabled)
{
    impl_->generation = generation;
    impl_->enabledValue = enabled;
    impl_->updating = true;
    impl_->enabled.IsOn(impl_->enabledValue);
    impl_->updating = false;
    impl_->ShowPaused();
    impl_->Reload();
}

void ScreenTimePagePresenter::RefreshLocalizedText()
{
    impl_->enabledLabel.Text(impl_->L("settings.screenTime.enabled"));
    impl_->enabledRow.SetText(impl_->L("settings.screenTime.enabled"),
        impl_->L("settings.screenTime.enabledHint"));
    impl_->clear.Content(winrt::box_value(
        impl_->L("settings.screenTime.clearAction")));
    impl_->clearRow.SetText(impl_->L("settings.screenTime.clear"));
    impl_->dayItem.Text(impl_->L("settings.screenTime.day"));
    impl_->weekItem.Text(impl_->L("settings.screenTime.week"));
    impl_->ShowPaused();
    if (impl_->active) impl_->Reload();
}

void ScreenTimePagePresenter::RegisterFocusTargets(
    const FocusRegistrar& registrar) const
{
    if (!registrar) return;
    registrar("screenTime.enabled", impl_->enabled);
    registrar("screenTime.clear", impl_->clear);
    registrar("screenTime.report", impl_->range);
}

void ScreenTimePagePresenter::Activate()
{
    impl_->active = true;
    if (!impl_->range.SelectedItem())
    {
        impl_->updating = true;
        impl_->range.SelectedItem(impl_->dayItem);
        impl_->updating = false;
    }
    impl_->Reload();
    impl_->timer.Start();
}

void ScreenTimePagePresenter::Deactivate()
{
    impl_->active = false;
    if (impl_->timer) impl_->timer.Stop();
}

void ScreenTimePagePresenter::Close()
{
    impl_->closed = true;
    Deactivate();
    impl_->Unbind();
}

} // namespace snowdesktop::winui
