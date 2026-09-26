#include "pch.h"
#include "system_control_view.h"
#include "../widget_system_data_provider.h"
#include "../l10n.h"
#include "../system_control_feedback.h"
#include "../system_control_wifi_presentation.h"
#include "../status_bar_battery.h"
#include <shellapi.h>
#include <robuffer.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <map>
#include <cmath>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace j = system_control::json;
namespace
{
using Request = system_control::Request;
using Arguments = system_control::Arguments;
const std::vector<JsonValue>& Items(const JsonValue& value, const char* field)
{ static const std::vector<JsonValue> empty; const auto* item = value.Find(field); return item && item->IsArray() ? item->array : empty; }
std::wstring Wide(std::string_view text) { return std::wstring(winrt::to_hstring(text)); }
std::string Identity(const std::vector<JsonValue>& values, const char* key = "id")
{
    std::string result;
    for (const auto& value : values)
    { const auto id = j::String(value, key); result += std::to_string(id.size()) + ":" + id; }
    return result;
}
c::TextBlock Text(std::wstring_view value)
{ c::TextBlock text; text.Text(value); text.TextWrapping(x::TextWrapping::Wrap); return text; }
void Name(const x::DependencyObject& object, std::wstring_view value)
{ x::Automation::AutomationProperties::SetName(object, value); }
c::FontIcon Chevron(bool back = false)
{
    c::FontIcon icon; icon.FontFamily(x::Media::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
    icon.Glyph(back ? L"\uE76B" : L"\uE76C"); icon.FontSize(14); return icon;
}
c::StackPanel Group(c::StackPanel parent, const wchar_t* title = nullptr)
{
    auto card = x::Markup::XamlReader::Load(L"<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' BorderBrush='{ThemeResource DividerStrokeColorDefaultBrush}' BorderThickness='0,1,0,0' Padding='0,12,0,0' />").as<c::Border>();
    c::StackPanel content; content.Spacing(10);
    if (title) { auto heading = Text(title); heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); content.Children().Append(heading); }
    card.Child(content); parent.Children().Append(card); return content;
}

}
struct SystemControlView::Impl : std::enable_shared_from_this<Impl>
{
    struct Section
    {
        std::string topic, key, title;
        c::StackPanel panel{nullptr};
        c::StackPanel footer{nullptr};
        c::Button tile{nullptr};
        c::Primitives::ToggleButton radio{nullptr};
        c::TextBlock summary{nullptr};
        std::vector<std::function<void()>> updates;
        bool open = false;
        std::optional<bool> accentApplied;
    };
    SystemControlViewSource source;
    c::Grid root, navigation;
    c::StackPanel overview, details, footers;
    c::ScrollViewer bodyScroll;
    c::ToggleSwitch detailRadio;
    c::Grid tiles;
    Section overviewAudio, overviewBrightness;
    Section overviewMedia;
    c::TextBlock batterySummary;
    c::Grid batteryGlyph;
    c::FontIcon batteryNormal;
    c::PathIcon batteryCharging;
    c::TextBlock detailTitle;
    c::InfoBar status;
    c::ContentDialog dialog{nullptr};
    std::map<std::string, Section> sections;
    std::string interfaceId, mediaId, quickBrightnessId, activeSection, wifiSelection;
    system_control::ControlFeedback feedback;
    std::function<void()> layoutChanged;
    bool updating = false, closed = false, scanOnArrival = false, layoutDirty = false;
    static constexpr const char* Consumer = "controlCenter";
    explicit Impl(SystemControlViewSource inputs) : source(std::move(inputs)) {}
    void Settings(const wchar_t* uri) { source.settings(uri); }
    JsonValue Current(const char* topic) const
    { const auto value = source.current(topic); return value && value->available ? value->value : j::Object(); }
    JsonValue Find(const char* topic, const char* collection, const std::string& id) const
    {
        const auto value = Current(topic);
        for (const auto& item : Items(value, collection)) if (j::String(item, "id") == id) return item;
        return j::Object();
    }
    void Notify(const char* key, bool error)
    { layoutDirty = layoutDirty || !status.IsOpen() || status.Message() != _LW(key); status.Message(_LW(key)); status.Severity(error ? c::InfoBarSeverity::Error : c::InfoBarSeverity::Informational); status.IsOpen(true); }
    c::Button Button(c::StackPanel parent, const wchar_t* label, std::function<void()> action)
    {
        c::Button button; button.Content(winrt::box_value(label)); Name(button, label);
        const auto weak = weak_from_this(); button.Click([weak, action = std::move(action)](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed && !self->updating) action();
        }); parent.Children().Append(button); return button;
    }
    c::StackPanel Row(c::StackPanel parent, c::TextBlock label, x::UIElement leading = nullptr)
    {
        c::Grid row; row.ColumnSpacing(8);
        c::ColumnDefinition content; content.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
        c::ColumnDefinition commands; commands.Width(x::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(content); row.ColumnDefinitions().Append(commands);
        label.MaxLines(2); label.TextTrimming(x::TextTrimming::CharacterEllipsis); label.VerticalAlignment(x::VerticalAlignment::Center);
        if (leading)
        {
            c::StackPanel heading; heading.Orientation(c::Orientation::Horizontal); heading.Spacing(6);
            heading.Children().Append(leading); heading.Children().Append(label); row.Children().Append(heading);
        }
        else row.Children().Append(label);
        c::StackPanel buttons; buttons.Spacing(4); buttons.Orientation(c::Orientation::Horizontal); c::Grid::SetColumn(buttons, 1);
        row.Children().Append(buttons); parent.Children().Append(row); return buttons;
    }
    void Start(std::string task, Arguments arguments = {})
    {
        auto request = std::make_shared<Request>(); request->name = std::move(task); request->arguments = std::move(arguments);
        if (system_control::RequiresConfirmation(request->name) || system_control::RequiresPasswordPrompt(*request)) { Prompt(request); return; }
        Submit(request);
    }
    void Submit(const std::shared_ptr<Request>& request)
    {
        if (closed) return;
        auto feedbackKey = system_control::ControlFeedback::Key(*request);
        const auto id = source.start(std::move(*request));
        feedback.Track(std::move(feedbackKey), id);
        if (!id) Notify("controlCenter.failed", true);
        else { status.IsOpen(false); }
    }
    void Prompt(std::shared_ptr<Request> request)
    {
        if (dialog || !root.XamlRoot()) return;
        dialog = c::ContentDialog(); dialog.XamlRoot(root.XamlRoot()); dialog.Title(winrt::box_value(_LW("statusBar.controlCenter")));
        dialog.PrimaryButtonText(_LW("settings.dialog.confirm")); dialog.CloseButtonText(_LW("settings.dialog.cancel"));
        dialog.DefaultButton(c::ContentDialogButton::Close);
        c::StackPanel fields; fields.Spacing(8); c::PasswordBox password;
        if (system_control::RequiresPasswordPrompt(*request))
        {
            fields.Children().Append(Text(_LW("controlCenter.passwordHint")));
            password.Header(winrt::box_value(_LW("controlCenter.password"))); password.MaxLength(63); fields.Children().Append(password);
        }
        else
        {
            const char* key = request->name == "network.wifi.forget" ? "controlCenter.confirmForget" :
                request->name == "system.power.sleep" ? "controlCenter.confirmSleep" :
                request->name == "system.power.restart" ? "controlCenter.confirmRestart" : "controlCenter.confirmShutdown";
            fields.Children().Append(Text(_LW(key)));
            if (request->name == "network.wifi.forget") fields.Children().Append(Text(Wide(request->arguments.at("profileName"))));
        }
        dialog.Content(fields);
        const auto weak = weak_from_this();
        dialog.PrimaryButtonClick([weak, request, password](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed)
            {
                const auto text = password.Password(); request->password = system_control::Secret(std::wstring_view(text)); password.Password(L"");
                request->hostConfirmed = true; self->Submit(request);
            }
        });
        dialog.Closed([weak](const auto&, const auto&) { if (const auto self = weak.lock()) self->dialog = nullptr; });
        dialog.ShowAsync();
    }
    c::ComboBox Choice(Section& section, const wchar_t* label, const std::vector<std::pair<std::string, std::wstring>>& choices,
        std::function<std::string()> selected, std::function<void(std::string)> change, c::StackPanel parent = nullptr)
    {
        c::ComboBox combo; combo.Header(winrt::box_value(label)); combo.HorizontalAlignment(x::HorizontalAlignment::Stretch); Name(combo, label);
        std::vector<std::string> ids;
        for (const auto& [id, title] : choices) { combo.Items().Append(winrt::box_value(title)); ids.push_back(id); }
        const auto weak = weak_from_this();
        combo.SelectionChanged([weak, ids, change](const auto& sender, const auto&) {
            if (const auto self = weak.lock(); self && !self->updating && !self->closed)
            { const auto index = sender.template as<c::ComboBox>().SelectedIndex(); if (index >= 0 && static_cast<std::size_t>(index) < ids.size()) change(ids[static_cast<std::size_t>(index)]); }
        });
        section.updates.push_back([combo, ids, selected] {
            const auto value = selected(); const auto found = std::find(ids.begin(), ids.end(), value);
            combo.SelectedIndex(found == ids.end() ? -1 : static_cast<int>(found - ids.begin()));
        }); (parent ? parent : section.panel).Children().Append(combo); return combo;
    }
    void Slider(Section& section, const wchar_t* label, std::function<std::optional<double>()> read,
        std::function<void(double)> write, c::StackPanel parent = nullptr, std::string detail = {})
    {
        c::StackPanel block; block.Spacing(2);
        c::Grid heading;
        c::ColumnDefinition titleColumn; titleColumn.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
        c::ColumnDefinition valueColumn; valueColumn.Width(x::GridLengthHelper::FromPixels(48));
        heading.ColumnDefinitions().Append(titleColumn); heading.ColumnDefinitions().Append(valueColumn);
        auto title = Text(label); title.FontSize(12); title.Opacity(.75); heading.Children().Append(title);
        c::TextBlock percent; percent.FontSize(12); percent.TextAlignment(x::TextAlignment::Right); c::Grid::SetColumn(percent, 1);
        heading.Children().Append(percent); block.Children().Append(heading);
        c::Grid track; track.ColumnSpacing(10);
        track.ColumnDefinitions().Append(c::ColumnDefinition());
        c::ColumnDefinition detailColumn; detailColumn.Width(x::GridLengthHelper::Auto()); track.ColumnDefinitions().Append(detailColumn);
        c::Slider slider; slider.Minimum(0); slider.Maximum(100); slider.StepFrequency(1); Name(slider, label);
        track.Children().Append(slider); block.Children().Append(track);
        auto dragging = std::make_shared<bool>(false); const auto weak = weak_from_this();
        if (!detail.empty())
        {
            c::StackPanel commands;
            auto more = Button(commands, label, [weak, detail] { if (auto self = weak.lock()) self->Select(detail); });
            more.Content(Chevron()); more.Padding({8, 6, 8, 6});
            commands.VerticalAlignment(x::VerticalAlignment::Center); c::Grid::SetColumn(commands, 1); track.Children().Append(commands);
        }
        slider.AddHandler(x::UIElement::PointerPressedEvent(), winrt::box_value(x::Input::PointerEventHandler(
            [dragging](const auto&, const auto&) { *dragging = true; })), true);
        slider.AddHandler(x::UIElement::PointerReleasedEvent(), winrt::box_value(x::Input::PointerEventHandler(
            [dragging](const auto&, const auto&) { *dragging = false; })), true);
        slider.PointerCaptureLost([dragging](const auto&, const auto&) { *dragging = false; });
        slider.ValueChanged([weak, write, percent](const auto&, const c::Primitives::RangeBaseValueChangedEventArgs& args) {
            percent.Text(std::to_wstring(static_cast<int>(std::lround(args.NewValue()))) + L"%");
            if (const auto self = weak.lock(); self && !self->updating && !self->closed) write(args.NewValue());
        });
        section.updates.push_back([slider, percent, dragging, read] {
            const auto value = read(); slider.IsEnabled(value.has_value());
            if (value && !*dragging) slider.Value(*value);
            percent.Text(value ? std::to_wstring(static_cast<int>(std::lround(slider.Value()))) + L"%" : L"—");
        });
        (parent ? parent : section.panel).Children().Append(block);
    }
    void Toggle(Section& section, const wchar_t* label, std::function<std::optional<bool>()> read, std::function<void(bool)> write, c::StackPanel parent = nullptr)
    {
        auto commands = Row(parent ? parent : section.panel, Text(label));
        c::ToggleSwitch toggle; toggle.OnContent(winrt::box_value(L"")); toggle.OffContent(winrt::box_value(L"")); toggle.MinWidth(0);
        Name(toggle, label); const auto weak = weak_from_this();
        toggle.Toggled([weak, write](const auto& sender, const auto&) {
            if (const auto self = weak.lock(); self && !self->updating && !self->closed) write(sender.template as<c::ToggleSwitch>().IsOn());
        });
        section.updates.push_back([toggle, read] { const auto value = read(); toggle.IsEnabled(value.has_value()); if (value) toggle.IsOn(*value); });
        commands.Children().Append(toggle);
    }
    void Fallback(Section& section, const wchar_t* uri)
    {
        c::HyperlinkButton button; button.Content(winrt::box_value(_LW("settings.taskbar.systemSettings.open")));
        button.Padding({0, 6, 0, 6}); button.HorizontalAlignment(x::HorizontalAlignment::Left);
        const auto weak = weak_from_this();
        button.Click([weak, uri](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed) self->Settings(uri);
        });
        section.footer.Children().Append(button);
    }
    std::optional<std::pair<std::string, bool>> Radio(const std::string& key) const
    {
        const auto state = Current(key == "wifi" ? "network.wifi" : "bluetooth.devices");
        const auto& radios = Items(state, key == "wifi" ? "interfaces" : "radios");
        for (const auto& radio : radios)
            if (j::Flag(radio, "available") && (key != "wifi" || interfaceId.empty() || j::String(radio, "id") == interfaceId))
                return std::pair(j::String(radio, "id"), j::Flag(radio, "enabled"));
        return std::nullopt;
    }
    void AddSection(const char* id, const char* title, const char* topic, const wchar_t* glyph)
    {
        auto& section = sections[id]; section.topic = topic; section.title = title;
        section.panel = c::StackPanel(); section.panel.Spacing(12);
        section.panel.Visibility(x::Visibility::Collapsed); details.Children().Append(section.panel);
        section.footer = c::StackPanel(); section.footer.Spacing(8);
        section.footer.Visibility(x::Visibility::Collapsed); footers.Children().Append(section.footer);
        section.summary = Text(_LW("controlCenter.loading")); section.summary.FontSize(12); section.summary.Opacity(.75);
        section.summary.MaxLines(1); section.summary.TextTrimming(x::TextTrimming::CharacterEllipsis);
        Demand(id);
        const std::string key(id);
        if (key != "wifi" && key != "bluetooth") return;
        c::StackPanel tile; tile.Spacing(6);
        c::Grid buttons;
        buttons.ColumnDefinitions().Append(c::ColumnDefinition());
        c::ColumnDefinition tail; tail.Width(x::GridLengthHelper::FromPixels(36)); buttons.ColumnDefinitions().Append(tail);
        section.radio = c::Primitives::ToggleButton(); section.radio.MinHeight(56);
        section.radio.CornerRadius({4, 0, 0, 4});
        section.radio.HorizontalAlignment(x::HorizontalAlignment::Stretch); Name(section.radio, _LW(title));
        x::Automation::AutomationProperties::SetAutomationId(section.radio, winrt::to_hstring("control.radio." + key));
        c::StackPanel heading; heading.Spacing(8); heading.Orientation(c::Orientation::Horizontal);
        c::FontIcon icon; icon.Glyph(glyph); icon.FontSize(18); heading.Children().Append(icon);
        auto caption = Text(_LW(title)); caption.FontSize(14); caption.MaxLines(1); heading.Children().Append(caption);
        section.radio.Content(heading); buttons.Children().Append(section.radio);
        section.tile = c::Button(); section.tile.Content(Chevron()); section.tile.Padding({8, 8, 8, 8});
        section.tile.CornerRadius({0, 4, 4, 0});
        x::Automation::AutomationProperties::SetAutomationId(section.tile, winrt::to_hstring("control.detail." + key));
        section.tile.HorizontalAlignment(x::HorizontalAlignment::Stretch); section.tile.VerticalAlignment(x::VerticalAlignment::Stretch);
        Name(section.tile, _LW(title)); c::Grid::SetColumn(section.tile, 1); buttons.Children().Append(section.tile);
        tile.Children().Append(buttons); tile.Children().Append(section.summary);
        c::Grid::SetColumn(tile, static_cast<int>(tiles.Children().Size())); tiles.Children().Append(tile);
        const auto weak = weak_from_this();
        section.tile.Click([weak, key](const auto&, const auto&) { if (auto self = weak.lock(); self && !self->closed) self->Select(key); });
        section.radio.Click([weak, key](const auto&, const auto&) {
            if (auto self = weak.lock(); self && !self->closed && !self->updating)
                if (const auto radio = self->Radio(key))
                    self->Start(key == "wifi" ? "network.wifi.setRadio" : "bluetooth.setRadio",
                        {{key == "wifi" ? "interfaceId" : "radioId", radio->first}, {"enabled", radio->second ? "0" : "1"}});
        });
    }
    void Demand(const std::string& key)
    {
        std::vector<std::string> topics{sections.at(key).topic};
        if (key == "audio") topics = {"audio.devices", "audio.output.volume", "audio.input.volume"};
        if (key == "media")
        {
            topics = {"media.sessions"};
            topics.push_back("media.artwork");
        }
        for (const auto& topic : topics)
            source.subscribe(topic, std::chrono::milliseconds(key == "audio" || key == "media" ? 500 : 2000));
    }
    void Select(const std::string& selected)
    {
        const bool detail = !selected.empty() && sections.contains(selected);
        activeSection = detail ? selected : std::string{};
        overview.Visibility(detail ? x::Visibility::Collapsed : x::Visibility::Visible);
        navigation.Visibility(detail ? x::Visibility::Visible : x::Visibility::Collapsed);
        details.Visibility(detail ? x::Visibility::Visible : x::Visibility::Collapsed);
        footers.Visibility(detail ? x::Visibility::Visible : x::Visibility::Collapsed);
        detailRadio.Visibility(selected == "wifi" || selected == "bluetooth" ? x::Visibility::Visible : x::Visibility::Collapsed);
        bodyScroll.ChangeView(nullptr, 0., nullptr, true);
        for (auto& [key, section] : sections)
        {
            section.open = detail && key == selected;
            section.panel.Visibility(section.open ? x::Visibility::Visible : x::Visibility::Collapsed);
            section.footer.Visibility(section.open ? x::Visibility::Visible : x::Visibility::Collapsed);
            if (section.open) detailTitle.Text(_LW(section.title.c_str()));
            Demand(key);
        }
        if (detail && selected == "wifi") scanOnArrival = true;
        status.IsOpen(false);
        Refresh();
        if (layoutChanged) layoutChanged();
    }
    void Build(const StatusBarSettings& settings, StatusBarAction initial)
    {
        overview.Spacing(16); details.Spacing(8); status.IsClosable(true);
        for (int i = 0; i < 4; ++i) { c::RowDefinition row; row.Height(x::GridLengthHelper::Auto()); root.RowDefinitions().Append(row); }
        bodyScroll.MaxHeight(SystemControlViewportHeight - 96);
        bodyScroll.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled);
        bodyScroll.VerticalScrollBarVisibility(c::ScrollBarVisibility::Auto);
        x::Automation::AutomationProperties::SetAutomationId(bodyScroll, L"control.scroll");
        c::StackPanel body; body.Spacing(0); body.Margin({16, 0, 16, 12});
        body.Children().Append(overview); body.Children().Append(details); bodyScroll.Content(body);
        c::Grid::SetRow(bodyScroll, 1); root.Children().Append(bodyScroll);
        footers.Margin({16, 8, 16, 12}); c::Grid::SetRow(footers, 2); root.Children().Append(footers);
        status.Margin({16, 0, 16, 12}); c::Grid::SetRow(status, 3); root.Children().Append(status);
        tiles.RowSpacing(8); tiles.ColumnSpacing(8);
        tiles.ColumnDefinitions().Append(c::ColumnDefinition()); tiles.ColumnDefinitions().Append(c::ColumnDefinition());
        overview.Children().Append(tiles); overview.Margin({0, 16, 0, 0});
        navigation.Margin({16, 10, 16, 12}); navigation.ColumnSpacing(8);
        navigation.ColumnDefinitions().Append(c::ColumnDefinition());
        c::ColumnDefinition trailing; trailing.Width(x::GridLengthHelper::Auto()); navigation.ColumnDefinitions().Append(trailing);
        c::StackPanel heading; heading.Orientation(c::Orientation::Horizontal); heading.Spacing(8);
        const auto weak = weak_from_this();
        auto back = Button(heading, _LW("controlCenter.overview"), [weak] { if (const auto self = weak.lock()) self->Select({}); });
        back.Content(Chevron(true)); back.Padding({8, 6, 8, 6});
        x::Automation::AutomationProperties::SetAutomationId(back, L"control.back");
        detailTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); detailTitle.VerticalAlignment(x::VerticalAlignment::Center);
        heading.Children().Append(detailTitle); navigation.Children().Append(heading);
        detailRadio.OnContent(winrt::box_value(L"")); detailRadio.OffContent(winrt::box_value(L"")); detailRadio.MinWidth(0);
        detailRadio.VerticalAlignment(x::VerticalAlignment::Center); c::Grid::SetColumn(detailRadio, 1); navigation.Children().Append(detailRadio);
        detailRadio.Toggled([weak](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed && !self->updating &&
                (self->activeSection == "wifi" || self->activeSection == "bluetooth"))
                if (const auto radio = self->Radio(self->activeSection))
                    self->Start(self->activeSection == "wifi" ? "network.wifi.setRadio" : "bluetooth.setRadio",
                        {{self->activeSection == "wifi" ? "interfaceId" : "radioId", radio->first}, {"enabled", self->detailRadio.IsOn() ? "1" : "0"}});
        });
        root.Children().Append(navigation);
        if (settings.wifiControls) AddSection("wifi", "statusBar.wifiControls", "network.wifi", L"\uE701");
        if (settings.bluetoothControls) AddSection("bluetooth", "statusBar.bluetoothControls", "bluetooth.devices", L"\uE702");
        if (settings.audioControls) AddSection("audio", "statusBar.audioControls", "audio.devices", L"\uE767");
        if (settings.brightnessControls) AddSection("brightness", "statusBar.brightnessControls", "system.display.brightness", L"\uE706");
        if (settings.mediaControls) AddSection("media", "statusBar.mediaControls", "media.sessions", L"\uE768");
        if (settings.powerControls) AddSection("power", "statusBar.powerControls", "system.power.plans", L"\uE7E8");
        auto levels = Group(overview);
        overviewAudio.panel = levels; overviewBrightness.panel = levels;
        if (settings.audioControls)
        {
            Slider(overviewAudio, _LW("statusBar.volume"), [this]() -> std::optional<double> {
                const auto snapshot = source.current("audio.output.volume");
                return snapshot && snapshot->available ? std::optional(j::Numeric(snapshot->value, "volume") * 100) : std::nullopt;
            }, [this](double level) { Start("audio.output.setVolume", {{"volume", std::to_string(level / 100)}}); }, nullptr, "audio");
        }
        if (settings.brightnessControls)
        {
            Slider(overviewBrightness, _LW("statusBar.brightnessControls"), [this]() -> std::optional<double> {
                const auto value = Current("system.display.brightness");
                const auto& monitors = Items(value, "monitors");
                const auto supported = std::find_if(monitors.begin(), monitors.end(), [this](const auto& item) {
                    return j::Flag(item, "available") && (quickBrightnessId.empty() || j::String(item, "id") == quickBrightnessId);
                });
                if (supported != monitors.end()) quickBrightnessId = j::String(*supported, "id");
                return supported == monitors.end() ? std::nullopt : std::optional(j::Numeric(*supported, "brightness"));
            }, [this](double level) { if (!quickBrightnessId.empty()) Start("system.display.setBrightness", {{"monitorId", quickBrightnessId}, {"brightness", std::to_string(level)}}); }, nullptr, "brightness");
        }
        if (settings.mediaControls) { overviewMedia.panel = Group(overview); Media(overviewMedia, true); }
        const std::wstring batteryMarkup = L"<PathIcon xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Width='20' Height='20' Foreground='{ThemeResource SystemFillColorSuccessBrush}' Data='" + std::wstring(ChargingBatteryPath) + L"'/>";
        batteryCharging = x::Markup::XamlReader::Load(batteryMarkup).as<c::PathIcon>();
        batteryNormal.FontSize(20); batteryNormal.Glyph(L"\uE83F");
        batteryGlyph.Children().Append(batteryNormal); batteryGlyph.Children().Append(batteryCharging);
        batteryGlyph.VerticalAlignment(x::VerticalAlignment::Center);
        const auto footer = Row(overview, batterySummary, batteryGlyph);
        auto power = Button(footer, _LW("statusBar.powerControls"), [weak] { if (auto self = weak.lock()) self->Select("power"); });
        c::FontIcon powerIcon; powerIcon.Glyph(L"\uE7E8"); powerIcon.FontSize(16); power.Content(powerIcon);
        auto system = Button(footer, _LW("statusBar.systemSettings"), [this] { Settings(L"ms-settings:"); });
        system.Content(c::SymbolIcon(c::Symbol::Setting));
        Select(initial == StatusBarAction::Audio ? "audio" : initial == StatusBarAction::Network ? "wifi" : initial == StatusBarAction::Power ? "power" : "");
    }
    void Summaries()
    {
        for (auto& [key, section] : sections)
        {
            const auto state = source.current(section.topic);
            std::wstring label = _LW(!state ? "controlCenter.loading" : !state->available ? "controlCenter.unavailable" : "controlCenter.off");
            if (key == "media")
            {
                const auto media = source.media();
                label = media && media->available && !media->sessions.empty() ? Wide(media->sessions.front().title) : _LW("controlCenter.unavailable");
            }
            else if (state && state->available)
            {
                const auto& value = state->value;
                if (key == "wifi")
                {
                    for (const auto& adapter : Items(value, "interfaces"))
                    {
                        if (j::Flag(adapter, "enabled")) label = _LW("controlCenter.on");
                        for (const auto& network : Items(adapter, "networks"))
                            if (j::Flag(network, "connected")) label = Wide(j::String(network, "ssid"));
                    }
                    if (Items(value, "interfaces").empty()) label = _LW("controlCenter.unavailable");
                }
                else if (key == "bluetooth")
                {
                    for (const auto& radio : Items(value, "radios")) if (j::Flag(radio, "enabled")) label = _LW("controlCenter.on");
                    for (const auto& device : Items(value, "devices")) if (j::Flag(device, "connected")) { label = Wide(j::String(device, "name")); break; }
                    if (Items(value, "radios").empty()) label = _LW("controlCenter.unavailable");
                }
                else if (key == "audio")
                {
                    const auto audio = Current("audio.output.default");
                    label = Wide(j::String(audio, "name"));
                    if (label.empty()) for (const auto& device : Items(value, "devices"))
                        if (j::Flag(device, "isDefault") && j::String(device, "direction") == "output") { label = Wide(j::String(device, "name")); break; }
                    if (label.empty()) label = _LW("controlCenter.unavailable");
                }
                else if (key == "brightness")
                {
                    label = _LW("controlCenter.unavailable");
                    for (const auto& monitor : Items(value, "monitors")) if (j::Flag(monitor, "available")) { label = Wide(j::String(monitor, "name")); break; }
                }
                else if (key == "power")
                {
                    label = L"";
                    if (const auto* battery = value.Find("batteryPercent"); j::Flag(value, "batteryPresent") && battery && battery->IsNumber())
                        label = std::to_wstring(static_cast<int>(battery->number)) + L"% · ";
                    for (const auto& plan : Items(value, "plans")) if (j::Flag(plan, "active")) { label += Wide(j::String(plan, "name")); break; }
                }
            }
            if (section.summary.Text() != label)
            {
                section.summary.Text(label);
                if (section.tile) { Name(section.tile, std::wstring(_LW(section.title.c_str())) + L" · " + label); c::ToolTipService::SetToolTip(section.tile, winrt::box_value(label)); }
            }
            if (section.radio)
            {
                const auto radio = Radio(key); const bool enabled = radio && radio->second;
                section.radio.IsEnabled(radio.has_value()); section.radio.IsChecked(enabled);
                if (section.accentApplied != enabled)
                {
                    x::Style style{nullptr};
                    if (enabled)
                        if (const auto app = x::Application::Current())
                            if (const auto resource = app.Resources().TryLookup(winrt::box_value(L"AccentButtonStyle")))
                                style = resource.try_as<x::Style>();
                    section.tile.Style(style); section.accentApplied = enabled;
                }
            }
        }
        for (const auto& update : overviewAudio.updates) update();
        for (const auto& update : overviewBrightness.updates) update();
        if (overviewMedia.panel) { Media(overviewMedia, true); for (const auto& update : overviewMedia.updates) update(); }
        const auto power = Current("system.power.plans");
        const auto* battery = power.Find("batteryPercent");
        const bool present = j::Flag(power, "batteryPresent") && battery && battery->IsNumber();
        batterySummary.Text(present ? std::to_wstring(static_cast<int>(battery->number)) + L"%" : _LW("statusBar.powerControls"));
        batteryGlyph.Visibility(present ? x::Visibility::Visible : x::Visibility::Collapsed);
        const bool charging = j::Flag(power, "charging");
        batteryCharging.Visibility(charging ? x::Visibility::Visible : x::Visibility::Collapsed);
        batteryNormal.Visibility(charging ? x::Visibility::Collapsed : x::Visibility::Visible);
    }
    bool Begin(Section& section, const std::string& key)
    { if (section.key == key) return false; layoutDirty = true; section.key = key; section.updates.clear(); section.panel.Children().Clear(); if (section.footer) section.footer.Children().Clear(); return true; }
    void Audio(Section& section)
    {
        const auto value = Current("audio.devices"); const auto& devices = Items(value, "devices");
        std::string identity = Identity(devices); for (const auto& item : devices) identity += j::Flag(item, "available") ? "1" : "0";
        if (!Begin(section, "audio:" + identity)) return;
        for (const auto* direction : {"output", "input"})
        {
            const auto group = Group(section.panel, _LW(direction == std::string("output") ? "controlCenter.output" : "controlCenter.input"));
            const std::string prefix = std::string("audio.") + direction;
            std::vector<std::pair<std::string, std::wstring>> choices;
            for (const auto& item : devices) if (j::Flag(item, "available") && j::String(item, "direction") == direction)
                choices.emplace_back(j::String(item, "id"), Wide(j::String(item, "name")));
            c::ListView endpoints; endpoints.SelectionMode(c::ListViewSelectionMode::Single);
            endpoints.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
            c::ScrollViewer::SetVerticalScrollBarVisibility(endpoints, c::ScrollBarVisibility::Disabled);
            c::ScrollViewer::SetVerticalScrollMode(endpoints, c::ScrollMode::Disabled);
            for (const auto& [id, label] : choices)
            {
                (void)id;
                c::ListViewItem entry; entry.Padding({10, 12, 10, 12}); entry.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
                c::Grid row; row.ColumnSpacing(12); c::ColumnDefinition glyph; glyph.Width(x::GridLengthHelper::FromPixels(20));
                row.ColumnDefinitions().Append(glyph); row.ColumnDefinitions().Append(c::ColumnDefinition());
                c::FontIcon icon; icon.Glyph(direction == std::string("output") ? L"\uE767" : L"\uE720"); icon.FontSize(18);
                row.Children().Append(icon); auto name = Text(label); name.MaxLines(2); c::Grid::SetColumn(name, 1); row.Children().Append(name);
                entry.Content(row); endpoints.Items().Append(entry);
            }
            if (choices.empty()) group.Children().Append(Text(_LW("controlCenter.unavailable")));
            group.Children().Append(endpoints);
            const auto weak = weak_from_this();
            endpoints.SelectionChanged([weak, endpoints, choices, prefix](const auto&, const auto&) {
                if (const auto self = weak.lock(); self && !self->closed && !self->updating)
                {
                    const int index = endpoints.SelectedIndex();
                    if (index >= 0 && static_cast<std::size_t>(index) < choices.size())
                        self->Start(prefix + ".selectDevice", {{"endpointId", choices[index].first}});
                }
            });
            section.updates.push_back([this, endpoints, choices, prefix] {
                const auto id = j::String(Current((prefix + ".volume").c_str()), "endpointId");
                const auto found = std::find_if(choices.begin(), choices.end(), [&](const auto& item) { return item.first == id; });
                endpoints.SelectedIndex(found == choices.end() ? -1 : static_cast<int>(found - choices.begin()));
            });
            Slider(section, _LW("statusBar.volume"), [this, prefix]() -> std::optional<double> {
                const auto state = source.current(prefix + ".volume"); return state && state->available ? std::optional(j::Numeric(state->value, "volume") * 100) : std::nullopt;
            }, [this, prefix](double level) { Start(prefix + ".setVolume", {{"volume", std::to_string(level / 100)}}); }, group);
            Toggle(section, _LW("controlCenter.mute"), [this, prefix]() -> std::optional<bool> {
                const auto state = source.current(prefix + ".volume"); return state && state->available ? std::optional(j::Flag(state->value, "muted")) : std::nullopt;
            }, [this, prefix](bool muted) { Start(prefix + ".setMute", {{"muted", muted ? "1" : "0"}}); }, group);
        }
        Fallback(section, L"ms-settings:sound");
    }
    void Brightness(Section& section)
    {
        const auto value = Current("system.display.brightness"); const auto& monitors = Items(value, "monitors");
        if (!Begin(section, "brightness:" + Identity(monitors))) return;
        for (const auto& monitor : monitors)
        {
            const auto group = Group(section.panel);
            const auto id = j::String(monitor, "id");
            Slider(section, Wide(j::String(monitor, "name")).c_str(), [this, id]() -> std::optional<double> {
                const auto item = Find("system.display.brightness", "monitors", id);
                return j::Flag(item, "available") ? std::optional(j::Numeric(item, "brightness")) : std::nullopt;
            }, [this, id](double level) { Start("system.display.setBrightness", {{"monitorId", id}, {"brightness", std::to_string(level)}}); }, group);
        }
        if (monitors.empty() || std::any_of(monitors.begin(), monitors.end(), [](const auto& item) { return !j::Flag(item, "available"); }))
            section.panel.Children().Append(Text(_LW("controlCenter.unsupportedHint")));
        Fallback(section, L"ms-settings:display");
    }
    JsonValue WifiInterface() const { return Find("network.wifi", "interfaces", interfaceId); }
    std::vector<JsonValue> WifiNetworks() const { return system_control::WifiPresentationNetworks(WifiInterface()); }
    void HiddenNetwork()
    {
        if (dialog || interfaceId.empty() || !root.XamlRoot()) return;
        dialog = c::ContentDialog(); dialog.XamlRoot(root.XamlRoot()); dialog.Title(winrt::box_value(_LW("controlCenter.hiddenNetwork")));
        dialog.PrimaryButtonText(_LW("controlCenter.connect")); dialog.CloseButtonText(_LW("settings.dialog.cancel"));
        dialog.DefaultButton(c::ContentDialogButton::Close);
        c::StackPanel fields; fields.Spacing(8); c::TextBox ssid; ssid.Header(winrt::box_value(_LW("controlCenter.ssid"))); ssid.MaxLength(32);
        c::ComboBox security; security.Header(winrt::box_value(_LW("controlCenter.security")));
        security.Items().Append(winrt::box_value(_LW("controlCenter.openNetwork"))); security.Items().Append(winrt::box_value(L"WPA2-Personal")); security.Items().Append(winrt::box_value(L"WPA3-Personal")); security.SelectedIndex(1);
        c::PasswordBox password; password.Header(winrt::box_value(_LW("controlCenter.password"))); password.MaxLength(63);
        fields.Children().Append(ssid); fields.Children().Append(security); fields.Children().Append(password); dialog.Content(fields);
        const auto weak = weak_from_this(); const auto adapter = interfaceId;
        dialog.PrimaryButtonClick([weak, adapter, ssid, security, password](const auto&, const c::ContentDialogButtonClickEventArgs& args) {
            const auto name = winrt::to_string(ssid.Text()); if (name.empty() || name.size() > 32) { args.Cancel(true); return; }
            if (const auto self = weak.lock(); self && !self->closed)
            {
                auto request = std::make_shared<Request>(); request->name = "network.wifi.connect";
                request->arguments = {{"interfaceId", adapter}, {"ssid", name}, {"hidden", "1"},
                    {"security", security.SelectedIndex() == 0 ? "open" : security.SelectedIndex() == 2 ? "wpa3" : "wpa2"}};
                const auto text = password.Password(); request->password = system_control::Secret(std::wstring_view(text)); password.Password(L""); self->Submit(request);
            }
        });
        dialog.Closed([weak](const auto&, const auto&) { if (const auto self = weak.lock()) self->dialog = nullptr; }); dialog.ShowAsync();
    }
    void Wifi(Section& section)
    {
        const auto value = Current("network.wifi"); const auto& interfaces = Items(value, "interfaces");
        if (!interfaces.empty() && std::none_of(interfaces.begin(), interfaces.end(), [this](const auto& item) { return j::String(item, "id") == interfaceId; }))
            interfaceId = j::String(interfaces.front(), "id");
        const auto current = WifiInterface(); const auto networks = WifiNetworks();
        if (scanOnArrival && !interfaceId.empty()) { scanOnArrival = false; Start("network.wifi.scan", {{"interfaceId", interfaceId}}); }
        if (!Begin(section, "wifi:" + Identity(interfaces) + interfaceId + Identity(networks))) return;
        std::vector<std::pair<std::string, std::wstring>> adapters;
        for (const auto& item : interfaces) adapters.emplace_back(j::String(item, "id"), Wide(j::String(item, "name")));
        if (adapters.size() > 1)
            Choice(section, _LW("controlCenter.adapter"), adapters, [this] { return interfaceId; }, [this](const auto& id) {
                interfaceId = id; wifiSelection.clear(); scanOnArrival = true; sections.at("wifi").key.clear(); Refresh();
            });
        Fallback(section, L"ms-settings:network-wifi");
        const auto scan = Button(section.footer, _LW("controlCenter.scan"), [this] { Start("network.wifi.scan", {{"interfaceId", interfaceId}}); });
        section.updates.push_back([this, scan] { scan.IsEnabled(!interfaceId.empty() && j::Flag(WifiInterface(), "enabled")); });
        c::TextBlock error = Text(_LW("controlCenter.locationDenied")); error.Visibility(x::Visibility::Collapsed);
        section.panel.Children().Append(error);
        c::ListView available; available.SelectionMode(c::ListViewSelectionMode::Single);
        available.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
        c::ScrollViewer::SetVerticalScrollMode(available, c::ScrollMode::Disabled);
        c::ScrollViewer::SetVerticalScrollBarVisibility(available, c::ScrollBarVisibility::Disabled);
        x::Automation::AutomationProperties::SetAutomationId(available, L"control.wifi.networks");
        section.panel.Children().Append(available);
        std::vector<std::string> networkIds;
        if (networks.empty()) section.panel.Children().Append(Text(_LW("controlCenter.unavailable")));
        if (std::none_of(networks.begin(), networks.end(), [&](const auto& network) { return j::String(network, "id") == wifiSelection; }))
            wifiSelection = !networks.empty() && j::Flag(networks.front(), "connected") ? j::String(networks.front(), "id") : std::string{};
        for (const auto& network : networks)
        {
            const auto id = j::String(network, "id");
            networkIds.push_back(id);
            c::ListViewItem entry; entry.Padding({10, 12, 10, 12}); entry.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
            c::Grid row; row.ColumnSpacing(12); c::ColumnDefinition glyph; glyph.Width(x::GridLengthHelper::FromPixels(24));
            row.ColumnDefinitions().Append(glyph); row.ColumnDefinitions().Append(c::ColumnDefinition());
            c::FontIcon signal; signal.Glyph(L"\uE701"); signal.FontSize(22); signal.VerticalAlignment(x::VerticalAlignment::Top); row.Children().Append(signal);
            c::StackPanel content; content.Spacing(4); c::Grid::SetColumn(content, 1); row.Children().Append(content);
            auto text = Text(Wide(j::String(network, "ssid"))); text.MaxLines(2); content.Children().Append(text);
            auto summary = Text(L""); summary.FontSize(12); summary.Opacity(.7); content.Children().Append(summary);
            const auto button = Button(content, _LW("controlCenter.connect"), [this, id] {
                for (const auto& item : WifiNetworks()) if (j::String(item, "id") == id)
                {
                    if (j::Flag(item, "connected")) Start("network.wifi.disconnect", {{"interfaceId", interfaceId}});
                    else if (!j::String(item, "profileName").empty()) Start("network.wifi.connect", {{"interfaceId", interfaceId}, {"profileName", j::String(item, "profileName")}});
                    else if (j::String(item, "security") == "system") Settings(L"ms-settings:network-wifi");
                    else Start("network.wifi.connect", {{"interfaceId", interfaceId}, {"networkId", id}});
                    return;
                }
            });
            button.HorizontalAlignment(x::HorizontalAlignment::Right); button.MinWidth(128); button.Margin({0, 8, 0, 0});
            entry.Content(row); available.Items().Append(entry);
            section.updates.push_back([this, id, text, summary, button] {
                for (const auto& item : WifiNetworks()) if (j::String(item, "id") == id)
                {
                    const auto ssid = j::String(item, "ssid");
                    text.Text(ssid.empty() ? _LW("controlCenter.hiddenNetwork") : Wide(ssid));
                    summary.Text((j::Flag(item, "connected") ? std::wstring(_LW("controlCenter.connected")) + L" · " : L"") +
                        (j::String(item, "security") == "open" ? std::wstring(_LW("controlCenter.openNetwork")) + L" · " : L"") +
                        std::to_wstring(static_cast<int>(j::Numeric(item, "signal"))) + L"%");
                    button.Content(winrt::box_value(_LW(j::Flag(item, "connected") ? "controlCenter.disconnect" : "controlCenter.connect")));
                    button.Visibility(id == wifiSelection ? x::Visibility::Visible : x::Visibility::Collapsed);
                    button.IsEnabled(j::Flag(item, "connectable") || j::Flag(item, "connected")); break;
                }
            });
        }
        const auto selected = std::find(networkIds.begin(), networkIds.end(), wifiSelection);
        available.SelectedIndex(selected == networkIds.end() ? -1 : static_cast<int>(selected - networkIds.begin()));
        const auto weak = weak_from_this();
        available.SelectionChanged([weak, available, networkIds](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed && !self->updating)
            {
                const int index = available.SelectedIndex();
                self->wifiSelection = index >= 0 && static_cast<std::size_t>(index) < networkIds.size() ? networkIds[index] : std::string{};
                self->Refresh(); if (self->layoutChanged) self->layoutChanged();
            }
        });
        const auto hidden = Button(section.panel, _LW("controlCenter.hiddenNetwork"), [this] { HiddenNetwork(); });
        section.updates.push_back([this, hidden] { hidden.IsEnabled(!interfaceId.empty() && j::Flag(WifiInterface(), "enabled")); });
        const auto location = Button(section.footer, _LW("controlCenter.locationSettings"), [this] { Settings(L"ms-settings:privacy-location"); });
        location.Visibility(x::Visibility::Collapsed);
        section.updates.push_back([this, error, location] {
            const auto visibility = j::String(WifiInterface(), "error") == "accessDenied" ? x::Visibility::Visible : x::Visibility::Collapsed;
            if (error.Visibility() != visibility)
            {
                error.Visibility(visibility); location.Visibility(visibility); layoutDirty = true;
            }
        });
    }
    void Bluetooth(Section& section)
    {
        const auto value = Current("bluetooth.devices"); const auto& radios = Items(value, "radios"); const auto& devices = Items(value, "devices");
        if (!Begin(section, "bluetooth:" + Identity(radios) + Identity(devices))) return;
        if (radios.empty()) section.panel.Children().Append(Text(_LW("controlCenter.unavailable")));
        if (radios.size() > 1) for (const auto& radio : radios)
        {
            const auto id = j::String(radio, "id");
            Toggle(section, Wide(j::String(radio, "name")).c_str(), [this, id]() -> std::optional<bool> {
                const auto item = Find("bluetooth.devices", "radios", id); return j::Flag(item, "available") ? std::optional(j::Flag(item, "enabled")) : std::nullopt;
            }, [this, id](bool enabled) { Start("bluetooth.setRadio", {{"radioId", id}, {"enabled", enabled ? "1" : "0"}}); });
        }
        const auto deviceGroup = Group(section.panel, _LW("controlCenter.devices"));
        if (devices.empty())
        {
            const auto radio = Radio("bluetooth");
            deviceGroup.Children().Append(Text(_LW(!radio ? "controlCenter.unavailable" : !radio->second ? "controlCenter.off" : "controlCenter.noDevices")));
        }
        for (const auto& device : devices)
        {
            const auto id = j::String(device, "id"); const auto text = Text(Wide(j::String(device, "name")));
            auto row = Row(deviceGroup, text);
            const auto button = Button(row, _LW("controlCenter.connect"), [this, id] {
                const auto item = Find("bluetooth.devices", "devices", id);
                if (!j::Flag(item, "canConnect")) Settings(L"ms-settings:bluetooth");
                else Start(j::Flag(item, "connected") ? "bluetooth.disconnect" : "bluetooth.connect", {{"deviceId", id}});
            });
            section.updates.push_back([this, id, text, button] {
                const auto item = Find("bluetooth.devices", "devices", id); auto label = Wide(j::String(item, "name"));
                if (const auto* battery = item.Find("batteryPercent"); battery && battery->IsNumber()) label += L"  " + std::to_wstring(static_cast<int>(battery->number)) + L"%";
                text.Text(label); button.Content(winrt::box_value(_LW(!j::Flag(item, "canConnect") ? "settings.taskbar.systemSettings.open" :
                    j::Flag(item, "connected") ? "controlCenter.disconnect" : "controlCenter.connect")));
            });
        }
        Fallback(section, L"ms-settings:bluetooth");
    }
    void Power(Section& section)
    {
        const auto value = Current("system.power.plans"); const auto& plans = Items(value, "plans");
        if (!Begin(section, "power:" + Identity(plans) + (j::Flag(value, "modeSupported") ? "1" : "0"))) return;
        std::vector<std::pair<std::string, std::wstring>> choices;
        for (const auto& plan : plans) choices.emplace_back(j::String(plan, "id"), Wide(j::String(plan, "name")));
        const auto plansGroup = Group(section.panel);
        Choice(section, _LW("controlCenter.powerPlan"), choices, [this] { return j::String(Current("system.power.plans"), "activePlanId"); },
            [this](const auto& id) { Start("system.power.setPlan", {{"planId", id}}); }, plansGroup);
        if (j::Flag(value, "modeSupported")) Choice(section, _LW("controlCenter.powerMode"),
            {{"efficiency", _LW("controlCenter.efficiency")}, {"balanced", _LW("controlCenter.balanced")}, {"performance", _LW("controlCenter.performance")}},
            [this] { const auto state = Current("system.power.plans"); return j::String(state, j::Flag(state, "onAC") ? "acMode" : "dcMode"); },
            [this](const auto& mode) { Start("system.power.setMode", {{"mode", mode}}); }, plansGroup);
        const auto battery = Text(L""); plansGroup.Children().Append(battery);
        section.updates.push_back([this, battery] { const auto state = Current("system.power.plans"); const auto* level = state.Find("batteryPercent");
            battery.Text(level && level->IsNumber() ? std::wstring(_LW("statusBar.battery")) + L" " + std::to_wstring(static_cast<int>(level->number)) + L"%" : L""); });
        c::Grid commands; commands.ColumnSpacing(8); commands.RowSpacing(8);
        commands.ColumnDefinitions().Append(c::ColumnDefinition()); commands.ColumnDefinitions().Append(c::ColumnDefinition());
        commands.RowDefinitions().Append(c::RowDefinition()); commands.RowDefinitions().Append(c::RowDefinition());
        int index = 0;
        for (const auto* action : {"lock", "sleep", "restart", "shutdown"})
        {
            const std::string task = std::string("system.power.") + action;
            c::StackPanel cell;
            auto command = Button(cell, _LW((std::string("controlCenter.") + action).c_str()), [this, task] { Start(task); });
            command.HorizontalAlignment(x::HorizontalAlignment::Stretch); command.Height(40);
            c::Grid::SetRow(cell, index / 2); c::Grid::SetColumn(cell, index % 2); commands.Children().Append(cell); ++index;
        }
        section.panel.Children().Append(commands);
        Fallback(section, L"ms-settings:powersleep");
    }
    void Media(Section& section, bool compact = false)
    {
        const auto snapshot = source.media(); std::string key;
        if (snapshot) for (const auto& item : snapshot->sessions) key += item.id + ":";
        if (!Begin(section, "media:" + key)) return;
        std::vector<std::pair<std::string, std::wstring>> choices;
        if (snapshot) for (const auto& item : snapshot->sessions) choices.emplace_back(item.id, Wide(item.sourceName));
        if (snapshot && (mediaId.empty() || std::none_of(snapshot->sessions.begin(), snapshot->sessions.end(), [this](const auto& item) { return item.id == mediaId; })))
            mediaId = snapshot->sessions.empty() ? std::string{} : snapshot->sessions.front().id;
        if (!compact && choices.size() > 1)
            Choice(section, _LW("statusBar.mediaControls"), choices, [this] { return mediaId; }, [this](const auto& id) { mediaId = id; Refresh(); });
        c::Grid track; track.ColumnSpacing(12);
        c::ColumnDefinition cover; cover.Width(x::GridLengthHelper::FromPixels(64)); track.ColumnDefinitions().Append(cover);
        track.ColumnDefinitions().Append(c::ColumnDefinition());
        c::Grid coverBox; coverBox.Width(64); coverBox.Height(64);
        c::FontIcon fallback; fallback.Glyph(L"\uE8D6"); fallback.FontSize(28); fallback.Opacity(.6); coverBox.Children().Append(fallback);
        c::Image art; art.Width(64); art.Height(64); art.Stretch(x::Media::Stretch::UniformToFill); coverBox.Children().Append(art); track.Children().Append(coverBox);
        c::StackPanel description; description.Spacing(4); description.VerticalAlignment(x::VerticalAlignment::Center); c::Grid::SetColumn(description, 1);
        auto title = Text(L""); title.MaxLines(2); title.TextTrimming(x::TextTrimming::CharacterEllipsis);
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); description.Children().Append(title);
        auto artist = Text(L""); artist.FontSize(12); artist.Opacity(.7); artist.MaxLines(1); artist.TextTrimming(x::TextTrimming::CharacterEllipsis); description.Children().Append(artist);
        track.Children().Append(description); section.panel.Children().Append(track);
        auto token = std::make_shared<std::string>();
        section.updates.push_back([this, art, fallback, token, title, artist] {
            std::wstring heading = _LW("controlCenter.unavailable"), subtitle;
            if (const auto state = source.media()) for (const auto& item : state->sessions) if (item.id == mediaId)
            { heading = Wide(item.title); subtitle = Wide(item.artist.empty() ? item.sourceName : item.artist); }
            if (title.Text() != heading) title.Text(heading);
            if (artist.Text() != subtitle) artist.Text(subtitle);
            const auto image = source.artwork();
            if (!image || !image->available || !image->pixels || image->sessionId != mediaId)
            { if (!token->empty()) art.Source(nullptr); token->clear(); fallback.Visibility(x::Visibility::Visible); return; }
            if (*token == image->resourceToken) return;
            const auto& pixels = *image->pixels;
            if (!widget_runtime::IsValidWidgetRuntimeImage(pixels)) return;
            x::Media::Imaging::WriteableBitmap bitmap(static_cast<int>(pixels.width), static_cast<int>(pixels.height)); BYTE* bytes = nullptr;
            winrt::check_hresult(bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
            for (std::uint32_t row = 0; row < pixels.height; ++row) std::memcpy(bytes + row * pixels.width * 4, pixels.bgraPremultiplied.data() + row * pixels.stride, pixels.width * 4);
            bitmap.Invalidate(); art.Source(bitmap); *token = image->resourceToken; fallback.Visibility(x::Visibility::Collapsed);
        });
        c::StackPanel playback; playback.Orientation(c::Orientation::Horizontal); playback.Spacing(12);
        playback.HorizontalAlignment(x::HorizontalAlignment::Center); section.panel.Children().Append(playback);
        for (const auto* action : {"previous", "toggle", "next"})
        {
            const std::string task = std::string("media.") + action;
            const auto button = Button(playback, _LW((std::string("controlCenter.") + action).c_str()), [this, task] { Start(task, {{"sessionId", mediaId}}); });
            c::SymbolIcon glyph(task == "media.previous" ? c::Symbol::Previous : task == "media.next" ? c::Symbol::Next : c::Symbol::Play);
            button.Content(glyph); button.Width(48); button.Height(36);
            section.updates.push_back([this, button, glyph, task] {
                bool enabled = false;
                if (const auto state = source.media()) for (const auto& item : state->sessions) if (item.id == mediaId)
                {
                    enabled = task == "media.previous" ? item.controls.canPrevious : task == "media.next" ? item.controls.canNext : item.controls.canPlayPause;
                    if (task == "media.toggle")
                    { const auto icon = item.playbackStatus == "playing" ? c::Symbol::Pause : c::Symbol::Play; if (glyph.Symbol() != icon) glyph.Symbol(icon); }
                }
                button.IsEnabled(enabled);
            });
        }
        if (compact && choices.size() > 1)
            Button(section.panel, _LW("statusBar.mediaControls"), [this] { Select("media"); });
    }
    void Refresh()
    {
        if (closed || updating) return;
        updating = true;
        struct Reset { bool& value; ~Reset() { value = false; } } reset{updating};
        Summaries();
        for (auto& [key, section] : sections)
        {
            if (!section.open) continue;
            if (key == "audio") Audio(section);
            else if (key == "brightness") Brightness(section);
            else if (key == "wifi") Wifi(section);
            else if (key == "bluetooth") Bluetooth(section);
            else if (key == "power") Power(section);
            else if (key == "media") Media(section);
            for (const auto& update : section.updates) update();
        }
        if (activeSection == "wifi" || activeSection == "bluetooth")
        {
            const auto radio = Radio(activeSection); detailRadio.IsEnabled(radio.has_value()); detailRadio.IsOn(radio && radio->second);
            Name(detailRadio, _LW(sections.at(activeSection).title.c_str()));
        }
        for (const auto& completion : source.completions())
        {
            if (!feedback.Take(completion.id)) continue;
            if (completion.error == "canceled") continue;
            if (completion.ok) { status.IsOpen(false); continue; }
            Notify(completion.error == "accessDenied" ? "controlCenter.accessDenied" :
                completion.error == "timeout" ? "controlCenter.timeout" : completion.error == "passwordRequired" ? "controlCenter.invalidPassword" :
                completion.error == "systemSettingsRequired" || completion.error == "actionUnsupported" ? "controlCenter.unsupportedHint" : "controlCenter.failed", !completion.ok);
        }
        if (layoutDirty) { layoutDirty = false; if (layoutChanged) layoutChanged(); }
    }
    void Close()
    {
        if (closed) return; closed = true;
        feedback.Clear();
        if (dialog) { dialog.Hide(); dialog = nullptr; }
        source.close(); root.Children().Clear(); sections.clear();
    }
};
namespace
{
SystemControlViewSource DeviceSource(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data)
{
    auto service = data->Controls();
    SystemControlViewSource source;
    source.current = [service](std::string_view topic) { return service->Current(topic); };
    source.start = [service](Request request) { return service->Start("controlCenter", std::move(request)); };
    source.completions = [service] { return service->DrainCompletions("controlCenter"); };
    source.subscribe = [data](std::string topic, std::chrono::milliseconds period) { data->StartTopic("controlCenter", topic, period); };
    source.unsubscribe = [data](std::string_view topic) { data->StopTopic("controlCenter", std::string(topic)); };
    source.close = [data] { data->RemoveConsumer("controlCenter"); };
    source.media = [data] { return data->MediaSessions(); };
    source.artwork = [data] { return data->MediaArtwork(); };
    source.settings = [](const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); };
    return source;
}
}
SystemControlView::SystemControlView(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data,
    const StatusBarSettings& settings, StatusBarAction initial, std::function<void()> layoutChanged)
    : SystemControlView(DeviceSource(std::move(data)), settings, initial, std::move(layoutChanged)) {}
SystemControlView::SystemControlView(SystemControlViewSource source,
    const StatusBarSettings& settings, StatusBarAction initial, std::function<void()> layoutChanged)
    : impl_(std::make_shared<Impl>(std::move(source)))
{ impl_->Build(settings, initial); impl_->layoutChanged = std::move(layoutChanged); }
SystemControlView::~SystemControlView() { impl_->Close(); }
x::FrameworkElement SystemControlView::Root() const { return impl_->root; }
void SystemControlView::Refresh() { impl_->Refresh(); }
void SystemControlView::Select(std::string_view section) { impl_->Select(std::string(section)); }
void SystemControlView::SetViewportHeight(double height)
{
    const double chrome = impl_->navigation.ActualHeight() + impl_->footers.ActualHeight() +
        (impl_->status.IsOpen() ? impl_->status.ActualHeight() : 0) + 64;
    const double available = std::max(48., std::min(SystemControlViewportHeight, height) - std::max(96., chrome));
    if (impl_->bodyScroll.MaxHeight() != available) impl_->bodyScroll.MaxHeight(available);
}
void SystemControlView::Close() { impl_->Close(); }
}
