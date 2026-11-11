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
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.h>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = mux::Controls;
namespace muxa = mux::Automation;

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

mux::Style BorderFillStyle(std::wstring_view brush)
{
    return mux::Markup::XamlReader::Load(
        L"<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' "
        L"TargetType='Border'><Setter Property='Background' Value='{ThemeResource "
        + std::wstring(brush) + L"}'/></Style>").as<mux::Style>();
}

void AddColumn(const muxc::Grid& grid, double weight)
{
    muxc::ColumnDefinition column;
    column.Width(weight == 0 ? mux::GridLengthHelper::Auto() :
        mux::GridLengthHelper::FromValueAndType(weight, mux::GridUnitType::Star));
    grid.ColumnDefinitions().Append(column);
}

muxc::TextBlock SmallLabel(const std::wstring& value)
{
    muxc::TextBlock label;
    label.Text(value);
    label.FontSize(12);
    label.Opacity(0.75);
    return label;
}

muxc::TextBlock SectionTitle()
{
    muxc::TextBlock title;
    title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    title.TextWrapping(mux::TextWrapping::Wrap);
    muxa::AutomationProperties::SetHeadingLevel(title,
        muxa::Peers::AutomationHeadingLevel::Level2);
    return title;
}

mux::FrameworkElement UsageTrack(std::int64_t seconds, std::int64_t peak,
    const mux::Style& accent, const mux::Style& line)
{
    muxc::Grid track;
    track.Height(5);
    track.VerticalAlignment(mux::VerticalAlignment::Center);
    const auto fraction = std::clamp(static_cast<double>(seconds) / peak, 0.0, 1.0);
    AddColumn(track, fraction);
    AddColumn(track, 1 - fraction);
    muxc::Border background;
    background.Style(line);
    background.CornerRadius({2});
    muxc::Grid::SetColumnSpan(background, 2);
    track.Children().Append(background);
    if (seconds > 0 && peak > 0)
    {
        muxc::Border fill;
        fill.Style(accent);
        fill.CornerRadius({2});
        track.Children().Append(fill);
    }
    return track;
}

std::wstring HourLabel(int hour)
{
    return (hour < 10 ? L"0" : L"") + std::to_wstring(hour) + L":00";
}

mux::UIElement UsageChart(const std::vector<std::int64_t>& values,
    const std::vector<std::wstring>& dates,
    const std::function<std::wstring(std::string_view)>& text,
    const mux::Style& accent, const mux::Style& line)
{
    constexpr double plotHeight = 104.0;
    const bool hourly = dates.empty();
    const auto peak = *std::max_element(values.begin(), values.end());
    // Round the weekly ceiling in two steps. The hourly scale stays at one hour
    // unless a local clock change puts more than an hour in the same bucket.
    const std::int64_t step = hourly ? 3600 : (peak > 3600 ? 7200 : 3600);
    const auto ceiling = std::max(step, (peak + step - 1) / step * step);

    muxc::Grid chart;
    chart.ColumnSpacing(8);
    chart.RowSpacing(4);
    AddColumn(chart, 1);
    AddColumn(chart, 0);
    for (int row = 0; row < 2; ++row)
    {
        muxc::RowDefinition definition;
        definition.Height(mux::GridLengthHelper::Auto());
        chart.RowDefinitions().Append(definition);
    }

    muxc::Grid plot;
    plot.Height(plotHeight);
    muxc::Grid labels;
    muxc::Grid::SetRow(labels, 1);
    const auto count = static_cast<int>(values.size());
    for (int index = 0; index < count; ++index)
    {
        AddColumn(plot, 1);
        AddColumn(labels, 1);
    }
    chart.Children().Append(plot);
    chart.Children().Append(labels);

    muxc::Grid scale;
    muxc::Grid::SetColumn(scale, 1);
    chart.Children().Append(scale);
    const mux::VerticalAlignment positions[]{
        mux::VerticalAlignment::Top, mux::VerticalAlignment::Center,
        mux::VerticalAlignment::Bottom};
    for (int index = 0; index < 3; ++index)
    {
        muxc::Border rule;
        rule.Style(line);
        rule.Height(1);
        rule.VerticalAlignment(positions[index]);
        muxc::Grid::SetColumnSpan(rule, count);
        plot.Children().Append(rule);
        const auto value = index == 0 ? ceiling : ceiling / 2;
        auto tick = SmallLabel(index == 2 ? L"0" : hourly ?
            Format(text("settings.screenTime.durationMinutes"), std::to_wstring(value / 60)) :
            Duration(text, value));
        tick.VerticalAlignment(positions[index]);
        tick.HorizontalAlignment(mux::HorizontalAlignment::Right);
        scale.Children().Append(tick);
    }

    for (int index = 0; index < count; ++index)
    {
        const auto value = values[static_cast<std::size_t>(index)];
        if (!hourly || index % 6 == 0)
        {
            auto label = SmallLabel(hourly ? HourLabel(index) :
                dates[static_cast<std::size_t>(index)].substr(5));
            label.HorizontalAlignment(hourly ? mux::HorizontalAlignment::Left :
                mux::HorizontalAlignment::Center);
            if (hourly) muxc::Grid::SetColumnSpan(label, 4);
            muxc::Grid::SetColumn(label, index);
            labels.Children().Append(label);
        }
        if (value <= 0) continue;
        muxc::Border bar;
        bar.Style(accent);
        bar.Height(std::max(1.0, plotHeight * static_cast<double>(value) / ceiling));
        bar.VerticalAlignment(mux::VerticalAlignment::Bottom);
        bar.MaxWidth(hourly ? 24 : 48);
        bar.Margin({2, 0, 2, 0});
        bar.CornerRadius({3, 3, 0, 0});
        const auto detail = (hourly ? HourLabel(index) + L"–" + HourLabel(index + 1) :
            dates[static_cast<std::size_t>(index)]) + L"  ·  " + Duration(text, value);
        muxc::ToolTipService::SetToolTip(bar, winrt::box_value(detail));
        muxa::AutomationProperties::SetName(bar, detail);
        muxc::Grid::SetColumn(bar, index);
        plot.Children().Append(bar);
    }
    if (hourly)
    {
        auto end = SmallLabel(HourLabel(24));
        end.HorizontalAlignment(mux::HorizontalAlignment::Right);
        muxc::Grid::SetColumn(end, count - 4);
        muxc::Grid::SetColumnSpan(end, 4);
        labels.Children().Append(end);
    }
    return chart;
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
    mux::Style accentStyle{nullptr};
    mux::Style dividerStyle{nullptr};
    muxc::StackPanel root{nullptr};
    muxc::TextBlock overviewTitle{nullptr};
    muxc::TextBlock appsTitle{nullptr};
    muxc::TextBlock paused{nullptr};
    muxc::TextBlock total{nullptr};
    muxc::TextBlock weekCaption{nullptr};
    muxc::ComboBox dayList{nullptr};
    muxc::Border chartHost{nullptr};
    muxc::Border appsHost{nullptr};
    muxc::ToggleSwitch enabled{nullptr};
    muxc::Button clear{nullptr};
    muxc::SelectorBar range{nullptr};
    muxc::SelectorBarItem dayItem{nullptr};
    muxc::SelectorBarItem weekItem{nullptr};
    presenter_controls::SettingRow enabledRow;
    presenter_controls::SettingRow clearRow;
    mux::DispatcherTimer timer{nullptr};
    std::uint64_t generation = 0;
    bool enabledValue = true;
    bool week = false;
    std::string selectedDate;
    std::string firstPickerDate;
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
        accentStyle = BorderFillStyle(L"AccentFillColorDefaultBrush");
        dividerStyle = BorderFillStyle(L"DividerStrokeColorDefaultBrush");

        muxc::StackPanel body;
        body.Spacing(0);

        enabled = muxc::ToggleSwitch{};
        enabledRow.Initialize(enabled);
        body.Children().Append(enabledRow.root);

        clear = muxc::Button{};
        clearRow.Initialize(clear);
        body.Children().Append(clearRow.root);
        muxc::Border card;
        presenter_controls::InitializeSettingsGroup(
            card, nullptr, body, cardStyle);
        presenter_controls::UpdateSettingsSubrows(body, 0);
        root.Children().Append(card);

        muxc::StackPanel overview;
        overview.Spacing(8);
        muxc::Grid header;
        header.ColumnSpacing(8);
        AddColumn(header, 1);
        AddColumn(header, 0);
        overviewTitle = SectionTitle();
        overviewTitle.VerticalAlignment(mux::VerticalAlignment::Center);
        header.Children().Append(overviewTitle);
        range = muxc::SelectorBar{};
        dayItem = muxc::SelectorBarItem{};
        weekItem = muxc::SelectorBarItem{};
        range.Items().Append(dayItem);
        range.Items().Append(weekItem);
        muxc::Grid::SetColumn(range, 1);
        header.Children().Append(range);
        overview.Children().Append(header);

        muxc::Grid summary;
        summary.ColumnSpacing(8);
        AddColumn(summary, 1);
        AddColumn(summary, 0);
        total = muxc::TextBlock{};
        total.FontSize(30);
        total.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        total.TextWrapping(mux::TextWrapping::Wrap);
        total.VerticalAlignment(mux::VerticalAlignment::Center);
        summary.Children().Append(total);
        dayList = muxc::ComboBox{};
        dayList.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(dayList, 1);
        summary.Children().Append(dayList);
        weekCaption = SmallLabel(L"");
        weekCaption.Visibility(mux::Visibility::Collapsed);
        weekCaption.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(weekCaption, 1);
        summary.Children().Append(weekCaption);
        overview.Children().Append(summary);

        paused = SmallLabel(L"");
        paused.TextWrapping(mux::TextWrapping::Wrap);
        overview.Children().Append(paused);
        chartHost = muxc::Border{};
        overview.Children().Append(chartHost);
        muxc::Border overviewCard;
        if (cardStyle) overviewCard.Style(cardStyle);
        overviewCard.Child(overview);
        root.Children().Append(overviewCard);

        muxc::StackPanel apps;
        apps.Spacing(6);
        appsTitle = SectionTitle();
        apps.Children().Append(appsTitle);
        appsHost = muxc::Border{};
        apps.Children().Append(appsHost);
        muxc::Border appsCard;
        if (cardStyle) appsCard.Style(cardStyle);
        appsCard.Child(apps);
        root.Children().Append(appsCard);

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
        const auto history = snowdesktop::screen_time::LoadReports(
            GetDataFilePath(L"SnowDesktop.screen-time.json"));
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::vector<snowdesktop::screen_time::DayReport> recent;
        recent.reserve(7);
        for (int offset = 6; offset >= 0; --offset)
        {
            std::tm local{};
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

        dayList.Visibility(week ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        weekCaption.Visibility(week ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        if (week)
        {
            std::int64_t sum = 0;
            std::map<std::string, std::int64_t> apps;
            std::vector<std::int64_t> totals;
            std::vector<std::wstring> dates;
            totals.reserve(recent.size());
            dates.reserve(recent.size());
            for (const auto& day : recent)
            {
                sum += day.totalSeconds;
                totals.push_back(day.totalSeconds);
                dates.push_back(Widen(day.date));
                for (const auto& app : day.apps)
                    apps[app.name] += app.seconds;
            }
            total.Text(Duration(localize, sum));
            chartHost.Child(UsageChart(totals, dates, localize, accentStyle, dividerStyle));
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

        updating = true;
        if (firstPickerDate != recent.front().date)
        {
            firstPickerDate = recent.front().date;
            dayList.Items().Clear();
            for (const auto& day : recent)
                dayList.Items().Append(winrt::box_value(Widen(day.date)));
        }
        int selectedIndex = static_cast<int>(recent.size()) - 1;
        for (int index = 0; index < static_cast<int>(recent.size()); ++index)
        {
            if (recent[static_cast<std::size_t>(index)].date == selectedDate)
                selectedIndex = index;
        }
        if (dayList.SelectedIndex() != selectedIndex)
            dayList.SelectedIndex(selectedIndex);
        updating = false;
        const auto& day = recent[static_cast<std::size_t>(selectedIndex)];
        selectedDate = day.date;
        total.Text(Duration(localize, day.totalSeconds));
        chartHost.Child(UsageChart(
            std::vector<std::int64_t>(day.hours.begin(), day.hours.end()),
            {}, localize, accentStyle, dividerStyle));
        AppendApps(day.apps);
    }

    void AppendApps(const std::vector<snowdesktop::screen_time::AppTotal>& apps)
    {
        if (apps.empty())
        {
            auto empty = SmallLabel(L(week ? "settings.screenTime.emptyWeek" :
                "settings.screenTime.empty"));
            empty.TextWrapping(mux::TextWrapping::Wrap);
            appsHost.Child(empty);
            return;
        }
        const auto peak = std::max<std::int64_t>(1, apps.front().seconds);
        const auto limit = std::min<std::size_t>(apps.size(), 30);
        muxc::Grid rows;
        rows.ColumnSpacing(12);
        AddColumn(rows, 3);
        AddColumn(rows, 1);
        AddColumn(rows, 0);
        for (std::size_t index = 0; index < limit; ++index)
        {
            const auto& app = apps[index];
            const auto rowIndex = static_cast<int>(index);
            muxc::RowDefinition row;
            row.Height(mux::GridLengthHelper::Auto());
            row.MinHeight(32);
            rows.RowDefinitions().Append(row);
            if (index > 0)
            {
                muxc::Border divider;
                divider.Style(dividerStyle);
                divider.Height(1);
                divider.VerticalAlignment(mux::VerticalAlignment::Top);
                muxc::Grid::SetColumnSpan(divider, 3);
                muxc::Grid::SetRow(divider, rowIndex);
                rows.Children().Append(divider);
            }
            muxc::TextBlock name;
            name.Text(Widen(app.name));
            name.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            name.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::ToolTipService::SetToolTip(name, winrt::box_value(name.Text()));
            const auto bar = UsageTrack(app.seconds, peak, accentStyle, dividerStyle);
            auto amount = SmallLabel(Duration(localize, app.seconds));
            amount.VerticalAlignment(mux::VerticalAlignment::Center);
            amount.HorizontalAlignment(mux::HorizontalAlignment::Right);
            muxc::Grid::SetRow(name, rowIndex);
            muxc::Grid::SetRow(bar, rowIndex);
            muxc::Grid::SetRow(amount, rowIndex);
            muxc::Grid::SetColumn(bar, 1);
            muxc::Grid::SetColumn(amount, 2);
            rows.Children().Append(name);
            rows.Children().Append(bar);
            rows.Children().Append(amount);
        }
        appsHost.Child(rows);
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
    impl_->enabledRow.SetText(impl_->L("settings.screenTime.enabled"),
        impl_->L("settings.screenTime.enabledHint"));
    impl_->enabled.OnContent(winrt::box_value(impl_->L("app.interact.on")));
    impl_->enabled.OffContent(winrt::box_value(impl_->L("app.interact.off")));
    impl_->clear.Content(winrt::box_value(
        impl_->L("settings.screenTime.clearAction")));
    impl_->clearRow.SetText(impl_->L("settings.screenTime.clear"));
    impl_->dayItem.Text(impl_->L("settings.screenTime.day"));
    impl_->weekItem.Text(impl_->L("settings.screenTime.week"));
    impl_->overviewTitle.Text(impl_->L("settings.screenTime.overview"));
    impl_->appsTitle.Text(impl_->L("settings.screenTime.appUsage"));
    impl_->weekCaption.Text(impl_->L("settings.screenTime.recentWeek"));
    muxa::AutomationProperties::SetName(impl_->dayList,
        impl_->L("settings.screenTime.day"));
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
