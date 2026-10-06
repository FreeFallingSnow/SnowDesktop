#include "pch.h"
#include "system_resource_view.h"
#include "../status_bar_presentation.h"
#include "../widget_gpu_presentation.h"
#include "../l10n.h"
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <cmath>
#include <array>
#include <limits>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace m = x::Media;
namespace a = x::Automation;
namespace wr = widget_runtime;
namespace
{
template<class T> T Themed(std::wstring_view tag, std::wstring_view attributes)
{
    return x::Markup::XamlReader::Load(L"<" + std::wstring(tag) +
        L" xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" " + std::wstring(attributes) + L"/>").as<T>();
}
void Text(const c::TextBlock& target, const std::wstring& value)
{
    if (target.Text() == value) return;
    target.Text(value); c::ToolTipService::SetToolTip(target, winrt::box_value(value));
}
std::wstring Bytes(std::uint64_t bytes)
{
    constexpr const wchar_t* units[]{L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double value = static_cast<double>(bytes); unsigned unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    wchar_t text[80]{}; swprintf_s(text, L"%.1f %s", value, units[unit]); return text;
}
std::wstring Percent(double value)
{
    if (!std::isfinite(value) || value < 0 || value > 100) return L"—";
    wchar_t text[40]{}; swprintf_s(text, L"%.0f%%", value); return text;
}
const char* Topic(StatusBarAction action)
{
    switch (action)
    {
    case StatusBarAction::Cpu: return "system.cpu";
    case StatusBarAction::Memory: return "system.memory";
    case StatusBarAction::Gpu: return "system.gpu";
    default: return "system.network.traffic";
    }
}
SystemResourceSource LiveSource(std::shared_ptr<wr::WidgetSystemDataProvider> data)
{
    SystemResourceSource source;
    source.cpu = [data] { return data->Cpu(); }; source.memory = [data] { return data->Memory(); };
    source.gpu = [data] { return data->Gpu(); }; source.traffic = [data] { return data->NetworkTraffic(); };
    source.history = [data](auto topic, auto adapter) { return data->ResourceHistory(topic, adapter); };
    source.subscribe = [data](auto topic) { data->StartTopic("resourcePanel", topic, std::chrono::milliseconds(1000)); };
    source.close = [data] { data->RemoveConsumer("resourcePanel"); };
    source.now = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count(); };
    return source;
}
}
struct SystemResourceView::Impl : std::enable_shared_from_this<Impl>
{
    SystemResourceSource source;
    StatusBarAction action;
    std::function<void()> layoutChanged;
    c::StackPanel root;
    c::TextBlock subtitle, status, axis;
    c::ComboBox adapter;
    c::Canvas chart;
    std::array<c::TextBlock, 4> values;
    std::array<x::Shapes::Path, 2> traces;
    std::array<x::Shapes::Line, 12> gridLines;
    std::vector<wr::WidgetResourcePoint> points;
    std::vector<std::pair<std::string, std::string>> adapters;
    std::string selected;
    bool selecting = false, closed = false;
    std::int64_t now = 0;
    double maximum = 100;
    Impl(SystemResourceSource value, StatusBarAction kind, std::function<void()> layout)
        : source(std::move(value)), action(kind), layoutChanged(std::move(layout)) {}
    ~Impl() { Close(); }
    void Build()
    {
        root.Spacing(12);
        c::TextBlock title; title.FontSize(20); title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.Text(_LW(action == StatusBarAction::Cpu ? "statusBar.cpu" : action == StatusBarAction::Memory ?
            "statusBar.memory" : action == StatusBarAction::Gpu ? "statusBar.gpu" : "statusBar.traffic"));
        root.Children().Append(title);
        subtitle = Themed<c::TextBlock>(L"TextBlock", L"Foreground=\"{ThemeResource TextFillColorSecondaryBrush}\" FontSize=\"12\" TextTrimming=\"CharacterEllipsis\" MaxLines=\"1\"");
        subtitle.MinHeight(18); root.Children().Append(subtitle);
        adapter.HorizontalAlignment(x::HorizontalAlignment::Stretch); adapter.Visibility(x::Visibility::Collapsed);
        a::AutomationProperties::SetName(adapter, _LW("resourcePanel.adapter"));
        a::AutomationProperties::SetAutomationId(adapter, L"resource.adapter");
        const auto weak = weak_from_this();
        adapter.SelectionChanged([weak](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed && !self->selecting && self->adapter.SelectedIndex() >= 0)
            {
                const auto index = static_cast<std::size_t>(self->adapter.SelectedIndex());
                if (index < self->adapters.size()) { self->selected = self->adapters[index].first; self->Refresh(); }
            }
        });
        root.Children().Append(adapter);
        c::Grid caption; caption.ColumnDefinitions().Append(c::ColumnDefinition()); caption.ColumnDefinitions().Append(c::ColumnDefinition());
        auto period = Themed<c::TextBlock>(L"TextBlock", L"Foreground=\"{ThemeResource TextFillColorSecondaryBrush}\" FontSize=\"12\"");
        period.Text(_LW("resourcePanel.history")); caption.Children().Append(period);
        axis = Themed<c::TextBlock>(L"TextBlock", L"Foreground=\"{ThemeResource TextFillColorSecondaryBrush}\" FontSize=\"12\"");
        axis.HorizontalAlignment(x::HorizontalAlignment::Right); c::Grid::SetColumn(axis, 1); caption.Children().Append(axis);
        root.Children().Append(caption);
        chart.Height(128); chart.IsHitTestVisible(false);
        a::AutomationProperties::SetName(chart, _LW("resourcePanel.history"));
        a::AutomationProperties::SetAutomationId(chart, L"resource.chart");
        for (auto& line : gridLines)
        {
            line = Themed<x::Shapes::Line>(L"Line", L"Stroke=\"{ThemeResource ControlStrokeColorDefaultBrush}\" StrokeThickness=\"1\"");
            chart.Children().Append(line);
        }
        for (int i = 0; i < 2; ++i)
        {
            traces[i] = Themed<x::Shapes::Path>(L"Path", i == 0 ?
                L"Stroke=\"{ThemeResource AccentTextFillColorPrimaryBrush}\" StrokeThickness=\"2\" StrokeLineJoin=\"Round\"" :
                L"Stroke=\"{ThemeResource TextFillColorSecondaryBrush}\" StrokeThickness=\"2\" StrokeDashArray=\"4,3\"");
            a::AutomationProperties::SetAutomationId(traces[i], i == 0 ? L"resource.primary" : L"resource.secondary");
            if (i == 0 || action == StatusBarAction::Traffic) chart.Children().Append(traces[i]);
        }
        chart.SizeChanged([weak](const auto&, const auto&) { if (const auto self = weak.lock(); self && !self->closed) self->Draw(); });
        root.Children().Append(chart);
        status = Themed<c::TextBlock>(L"TextBlock", L"Foreground=\"{ThemeResource TextFillColorSecondaryBrush}\" FontSize=\"12\" TextTrimming=\"CharacterEllipsis\" MaxLines=\"1\"");
        status.MinHeight(18); a::AutomationProperties::SetAutomationId(status, L"resource.status"); root.Children().Append(status);
        c::Grid cards; cards.ColumnSpacing(12); cards.RowSpacing(12);
        cards.ColumnDefinitions().Append(c::ColumnDefinition()); cards.ColumnDefinitions().Append(c::ColumnDefinition());
        cards.RowDefinitions().Append(c::RowDefinition());
        const int count = action == StatusBarAction::Cpu ? 2 : 4;
        if (count == 4) cards.RowDefinitions().Append(c::RowDefinition());
        const char* labels[4]{};
        if (action == StatusBarAction::Cpu) { labels[0] = "resourcePanel.usage"; labels[1] = "resourcePanel.logical"; }
        else if (action == StatusBarAction::Memory)
        { labels[0] = "resourcePanel.used"; labels[1] = "resourcePanel.available"; labels[2] = "resourcePanel.total"; labels[3] = "resourcePanel.committed"; }
        else if (action == StatusBarAction::Gpu)
        { labels[0] = "resourcePanel.usage"; labels[1] = "resourcePanel.dedicated"; labels[2] = "resourcePanel.shared"; labels[3] = "resourcePanel.capacity"; }
        else
        { labels[0] = "resourcePanel.download"; labels[1] = "resourcePanel.upload"; labels[2] = "resourcePanel.received"; labels[3] = "resourcePanel.sent"; }
        for (int i = 0; i < count; ++i)
        {
            auto card = Themed<c::Border>(L"Border", L"Background=\"{ThemeResource CardBackgroundFillColorDefaultBrush}\" BorderBrush=\"{ThemeResource CardStrokeColorDefaultBrush}\" BorderThickness=\"1\" CornerRadius=\"8\" Padding=\"14\" MinHeight=\"82\"");
            a::AutomationProperties::SetAutomationId(card, winrt::to_hstring("resource.card." + std::to_string(i)));
            c::StackPanel content; content.Spacing(6);
            auto label = Themed<c::TextBlock>(L"TextBlock", L"Foreground=\"{ThemeResource TextFillColorSecondaryBrush}\" FontSize=\"12\" TextTrimming=\"CharacterEllipsis\" MaxLines=\"1\"");
            label.Text(_LW(labels[i])); content.Children().Append(label);
            c::ToolTipService::SetToolTip(label, winrt::box_value(_LW(labels[i])));
            values[i].FontSize(22); values[i].FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            values[i].TextTrimming(x::TextTrimming::CharacterEllipsis);
            a::AutomationProperties::SetName(values[i], _LW(labels[i]));
            a::AutomationProperties::SetAutomationId(values[i], winrt::to_hstring("resource.value." + std::to_string(i)));
            content.Children().Append(values[i]); card.Child(content);
            c::Grid::SetRow(card, i / 2); c::Grid::SetColumn(card, i % 2); cards.Children().Append(card);
        }
        root.Children().Append(cards);
        if (source.subscribe) source.subscribe(Topic(action));
        Refresh();
    }
    void SelectAdapter(const std::vector<wr::WidgetGpuAdapterDataSnapshot>& list)
    {
        std::vector<std::pair<std::string, std::string>> next;
        for (const auto& item : list)
        {
            auto name = item.name;
            if (std::count_if(list.begin(), list.end(), [&](const auto& other) { return other.name == item.name; }) > 1)
                name += " (" + item.id + ")";
            next.emplace_back(item.id, std::move(name));
        }
        if (next == adapters) return;
        selecting = true; adapters = std::move(next); adapter.Items().Clear();
        auto chosen = std::find_if(adapters.begin(), adapters.end(), [&](const auto& item) { return item.first == selected; });
        if (chosen == adapters.end() && !adapters.empty()) chosen = adapters.begin();
        selected = chosen == adapters.end() ? std::string{} : chosen->first;
        for (const auto& item : adapters) adapter.Items().Append(winrt::box_value(winrt::to_hstring(item.second)));
        adapter.SelectedIndex(chosen == adapters.end() ? -1 : static_cast<int>(chosen - adapters.begin()));
        adapter.Visibility(adapters.size() > 1 ? x::Visibility::Visible : x::Visibility::Collapsed);
        selecting = false;
        if (layoutChanged) layoutChanged();
    }
    void Refresh()
    {
        if (closed) return;
        now = source.now ? source.now() : 0;
        bool available = false, warming = false;
        std::array<std::wstring, 4> text{L"—", L"—", L"—", L"—"};
        std::wstring detail;
        if (action == StatusBarAction::Cpu && source.cpu)
        {
            if (const auto value = source.cpu())
            {
                available = value->available && !value->warmingUp; warming = value->warmingUp;
                detail = winrt::to_hstring(value->name);
                if (available) text[0] = Percent(value->usagePercent);
                if (value->logicalProcessors) text[1] = std::to_wstring(value->logicalProcessors);
            }
        }
        else if (action == StatusBarAction::Memory && source.memory)
        {
            detail = _LW("resourcePanel.physical");
            if (const auto value = source.memory(); value && value->available)
            {
                available = value->totalBytes > 0;
                if (available) text = {Bytes(value->usedBytes), Bytes(value->freeBytes), Bytes(value->totalBytes), Bytes(value->commitUsedBytes)};
            }
        }
        else if (action == StatusBarAction::Gpu && source.gpu)
        {
            if (const auto value = source.gpu())
            {
                const auto presented = wr::PresentGpuAdapters(value->adapters);
                warming = value->warmingUp; SelectAdapter(presented);
                const auto chosen = std::find_if(value->adapters.begin(), value->adapters.end(), [&](const auto& item) { return item.id == selected; });
                if (chosen != value->adapters.end())
                {
                    detail = winrt::to_hstring(chosen->name); available = chosen->usageAvailable && !warming;
                    if (available) text[0] = Percent(chosen->usagePercent);
                    if (chosen->dedicatedUsageAvailable) text[1] = Bytes(chosen->dedicatedUsedBytes);
                    if (chosen->sharedUsageAvailable) text[2] = Bytes(chosen->sharedUsedBytes);
                    text[3] = Bytes(chosen->dedicatedMemoryBytes);
                }
            }
        }
        else if (source.traffic)
        {
            detail = _LW("resourcePanel.networkLegend");
            if (const auto value = source.traffic())
            {
                available = value->available && !value->warmingUp; warming = value->warmingUp;
                if (available) text = {StatusBarRate(value->downloadBytesPerSecond), StatusBarRate(value->uploadBytesPerSecond),
                    Bytes(value->receivedBytes), Bytes(value->sentBytes)};
            }
        }
        const auto next = source.history ? source.history(Topic(action), selected) : std::vector<wr::WidgetResourcePoint>{};
        const bool stale = !next.empty() && now > next.back().timestampMs + 5000;
        // Lua envelopes intentionally debounce transient errors. The native
        // chart uses raw sampling history, so do not relabel a held old value
        // as the current reading during warm-up, a failed sample or resume.
        if (stale || next.empty() || !next.back().primary)
        {
            available = false;
            text[0] = L"—";
            if (action != StatusBarAction::Cpu)
            {
                text[1] = text[2] = L"—";
                if (action != StatusBarAction::Gpu) text[3] = L"—";
            }
        }
        Text(subtitle, detail);
        for (int i = 0; i < 4; ++i) Text(values[i], text[i]);
        Text(status, !available ? _LW(warming || next.empty() ? "resourcePanel.waiting" : "resourcePanel.unavailable") : L"");
        const bool changed = next != points; points = next;
        if (changed || stale) Draw();
    }
    void Draw()
    {
        const float width = static_cast<float>(chart.ActualWidth()), height = static_cast<float>(chart.Height());
        if (width <= 0 || closed) return;
        maximum = 100;
        if (action == StatusBarAction::Traffic)
        {
            double peak = 1024;
            for (const auto& point : points)
                for (const auto& value : {point.primary, point.secondary}) if (value) peak = (std::max)(peak, *value);
            const double base = std::pow(10., std::floor(std::log10(peak)));
            maximum = std::ceil(peak / base) * base;
            Text(axis, StatusBarRate(maximum >= static_cast<double>((std::numeric_limits<std::uint64_t>::max)()) ?
                (std::numeric_limits<std::uint64_t>::max)() : static_cast<std::uint64_t>(maximum)));
        }
        else Text(axis, L"100%");
        for (int i = 0; i <= 6; ++i)
        {
            auto line = gridLines[i];
            line.X1(width * i / 6); line.X2(width * i / 6); line.Y2(height);
        }
        for (int i = 0; i <= 4; ++i)
        {
            auto line = gridLines[i + 7];
            line.X2(width); line.Y1(height * i / 4); line.Y2(height * i / 4);
        }
        const auto end = points.empty() ? now : (std::max)(now, points.back().timestampMs);
        for (int channel = 0; channel < (action == StatusBarAction::Traffic ? 2 : 1); ++channel)
        {
            m::PathGeometry geometry; m::PathFigure figure{nullptr}; std::int64_t previous = 0;
            for (const auto& point : points)
            {
                const auto value = channel ? point.secondary : point.primary;
                if (!value || point.timestampMs < end - wr::WidgetResourceHistory::WindowMs)
                { figure = nullptr; continue; }
                const float px = static_cast<float>(width * (1. - static_cast<double>(end - point.timestampMs) / wr::WidgetResourceHistory::WindowMs));
                const float py = static_cast<float>(height - 2 - std::clamp(*value / maximum, 0., 1.) * (height - 4));
                if (!figure || point.timestampMs - previous > 2500)
                {
                    figure = m::PathFigure(); figure.StartPoint({px, py}); figure.IsClosed(false); figure.IsFilled(false);
                    geometry.Figures().Append(figure);
                    // An isolated valid sample is a short mark, never a line
                    // across an unavailable interval or sleep/resume gap.
                    m::LineSegment dot; dot.Point({(std::max)(0.f, px - 1), py}); figure.Segments().Append(dot);
                }
                else { m::LineSegment segment; segment.Point({px, py}); figure.Segments().Append(segment); }
                previous = point.timestampMs;
            }
            traces[channel].Data(geometry);
        }
    }
    void Close()
    {
        if (closed) return;
        closed = true;
        if (const auto callback = source.close) callback();
        source = {}; layoutChanged = {};
    }
};
bool IsSystemResourceAction(StatusBarAction action)
{ return action == StatusBarAction::Cpu || action == StatusBarAction::Memory || action == StatusBarAction::Gpu || action == StatusBarAction::Traffic; }
SystemResourceView::SystemResourceView(std::shared_ptr<wr::WidgetSystemDataProvider> data, StatusBarAction action, std::function<void()> layout)
    : SystemResourceView(LiveSource(std::move(data)), action, std::move(layout)) {}
SystemResourceView::SystemResourceView(SystemResourceSource source, StatusBarAction action, std::function<void()> layout)
    : impl_(std::make_shared<Impl>(std::move(source), action, std::move(layout))) { impl_->Build(); }
SystemResourceView::~SystemResourceView() { Close(); }
x::FrameworkElement SystemResourceView::Root() const { return impl_->root; }
void SystemResourceView::Refresh() { impl_->Refresh(); }
void SystemResourceView::Close() { impl_->Close(); }
}
