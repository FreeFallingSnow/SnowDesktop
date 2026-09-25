#include "pch.h"
#include "system_control_view.h"
#include "../widget_system_data_provider.h"
#include "../l10n.h"
#include "../system_control_feedback.h"
#include <shellapi.h>
#include <robuffer.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <map>

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
void Settings(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }
}
struct SystemControlView::Impl : std::enable_shared_from_this<Impl>
{
    struct Section
    {
        std::string topic, key, title;
        c::StackPanel panel{nullptr};
        c::Button tile{nullptr};
        c::TextBlock summary{nullptr};
        std::vector<std::function<void()>> updates;
        bool open = false;
    };
    std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data;
    std::shared_ptr<system_control::Service> service;
    c::StackPanel root;
    c::StackPanel overview, details, navigation;
    c::Grid tiles;
    Section overviewAudio, overviewBrightness;
    c::TextBlock detailTitle;
    c::InfoBar status;
    c::ContentDialog dialog{nullptr};
    std::map<std::string, Section> sections;
    std::string interfaceId, mediaId, quickBrightnessId;
    system_control::ControlFeedback feedback;
    std::function<void()> layoutChanged;
    bool updating = false, closed = false, scanOnArrival = false;
    static constexpr const char* Consumer = "controlCenter";
    explicit Impl(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> provider) : data(std::move(provider)), service(data->Controls()) {}
    JsonValue Current(const char* topic) const
    { const auto value = service->Current(topic); return value && value->available ? value->value : j::Object(); }
    JsonValue Find(const char* topic, const char* collection, const std::string& id) const
    {
        const auto value = Current(topic);
        for (const auto& item : Items(value, collection)) if (j::String(item, "id") == id) return item;
        return j::Object();
    }
    void Notify(const char* key, bool error)
    { status.Message(_LW(key)); status.Severity(error ? c::InfoBarSeverity::Error : c::InfoBarSeverity::Informational); status.IsOpen(true); }
    c::Button Button(c::StackPanel parent, const wchar_t* label, std::function<void()> action)
    {
        c::Button button; button.Content(winrt::box_value(label)); Name(button, label);
        const auto weak = weak_from_this(); button.Click([weak, action = std::move(action)](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed && !self->updating) action();
        }); parent.Children().Append(button); return button;
    }
    c::StackPanel Row(c::StackPanel parent, c::TextBlock label)
    {
        c::Grid row; row.ColumnSpacing(8);
        c::ColumnDefinition content; content.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
        c::ColumnDefinition commands; commands.Width(x::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(content); row.ColumnDefinitions().Append(commands);
        label.MaxLines(2); label.TextTrimming(x::TextTrimming::CharacterEllipsis); label.VerticalAlignment(x::VerticalAlignment::Center);
        row.Children().Append(label);
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
        const auto id = service->Start(Consumer, std::move(*request));
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
        std::function<std::string()> selected, std::function<void(std::string)> change)
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
        }); section.panel.Children().Append(combo); return combo;
    }
    void Slider(Section& section, const wchar_t* label, std::function<std::optional<double>()> read,
        std::function<void(double)> write)
    {
        c::Slider slider; slider.Header(winrt::box_value(label)); slider.Minimum(0); slider.Maximum(100); slider.StepFrequency(1); Name(slider, label);
        auto dragging = std::make_shared<bool>(false); const auto weak = weak_from_this();
        slider.AddHandler(x::UIElement::PointerPressedEvent(), winrt::box_value(x::Input::PointerEventHandler(
            [dragging](const auto&, const auto&) { *dragging = true; })), true);
        slider.AddHandler(x::UIElement::PointerReleasedEvent(), winrt::box_value(x::Input::PointerEventHandler(
            [dragging](const auto&, const auto&) { *dragging = false; })), true);
        slider.PointerCaptureLost([dragging](const auto&, const auto&) { *dragging = false; });
        slider.ValueChanged([weak, write](const auto&, const c::Primitives::RangeBaseValueChangedEventArgs& args) {
            if (const auto self = weak.lock(); self && !self->updating && !self->closed) write(args.NewValue());
        });
        section.updates.push_back([slider, dragging, read] { const auto value = read(); slider.IsEnabled(value.has_value()); if (value && !*dragging) slider.Value(*value); });
        section.panel.Children().Append(slider);
    }
    void Toggle(Section& section, const wchar_t* label, std::function<std::optional<bool>()> read, std::function<void(bool)> write)
    {
        c::ToggleSwitch toggle; toggle.Header(winrt::box_value(label)); Name(toggle, label); const auto weak = weak_from_this();
        toggle.Toggled([weak, write](const auto& sender, const auto&) {
            if (const auto self = weak.lock(); self && !self->updating && !self->closed) write(sender.template as<c::ToggleSwitch>().IsOn());
        });
        section.updates.push_back([toggle, read] { const auto value = read(); toggle.IsEnabled(value.has_value()); if (value) toggle.IsOn(*value); });
        section.panel.Children().Append(toggle);
    }
    void Fallback(Section& section, const wchar_t* uri)
    { Button(section.panel, _LW("settings.taskbar.systemSettings.open"), [uri] { Settings(uri); }); }
    void AddSection(const char* id, const char* title, const char* topic, const wchar_t* glyph)
    {
        auto& section = sections[id]; section.topic = topic; section.title = title;
        section.panel = c::StackPanel(); section.panel.Spacing(8);
        section.panel.Visibility(x::Visibility::Collapsed);
        details.Children().Append(section.panel);
        c::StackPanel label; label.Spacing(4);
        c::StackPanel heading; heading.Orientation(c::Orientation::Horizontal); heading.Spacing(8);
        c::FontIcon icon; icon.Glyph(glyph); icon.FontSize(18); heading.Children().Append(icon);
        auto caption = Text(_LW(title)); caption.FontSize(14); caption.MaxLines(1); caption.TextTrimming(x::TextTrimming::CharacterEllipsis);
        heading.Children().Append(caption); label.Children().Append(heading);
        section.summary = Text(_LW("controlCenter.loading")); section.summary.FontSize(12); section.summary.Opacity(.7);
        section.summary.MaxLines(1); section.summary.TextTrimming(x::TextTrimming::CharacterEllipsis); label.Children().Append(section.summary);
        section.tile = c::Button(); section.tile.Content(label); section.tile.Padding({12, 10, 12, 10});
        section.tile.HorizontalAlignment(x::HorizontalAlignment::Stretch); section.tile.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
        section.tile.MinHeight(72); Name(section.tile, _LW(title));
        const int index = static_cast<int>(sections.size()) - 1;
        if (index % 2 == 0) tiles.RowDefinitions().Append(c::RowDefinition());
        c::Grid::SetRow(section.tile, index / 2); c::Grid::SetColumn(section.tile, index % 2);
        tiles.Children().Append(section.tile);
        const auto weak = weak_from_this(); const std::string key(id);
        section.tile.Click([weak, key](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed) self->Select(key);
        });
        Demand(key);
    }
    void Demand(const std::string& key)
    {
        std::vector<std::string> topics{sections.at(key).topic};
        if (key == "audio") topics = {"audio.devices", "audio.output.volume", "audio.input.volume"};
        if (key == "media")
        {
            topics = {"media.sessions"};
            if (sections.at(key).open) topics.push_back("media.artwork");
            else data->StopTopic(Consumer, "media.artwork");
        }
        for (const auto& topic : topics)
            data->StartTopic(Consumer, topic, std::chrono::milliseconds(key == "audio" || key == "media" ? 500 : 2000));
    }
    void Select(const std::string& selected)
    {
        const bool detail = !selected.empty() && sections.contains(selected);
        overview.Visibility(detail ? x::Visibility::Collapsed : x::Visibility::Visible);
        navigation.Visibility(detail ? x::Visibility::Visible : x::Visibility::Collapsed);
        details.Visibility(detail ? x::Visibility::Visible : x::Visibility::Collapsed);
        for (auto& [key, section] : sections)
        {
            section.open = detail && key == selected;
            section.panel.Visibility(section.open ? x::Visibility::Visible : x::Visibility::Collapsed);
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
        root.Spacing(12); overview.Spacing(12); details.Spacing(8); status.IsClosable(true);
        tiles.RowSpacing(8); tiles.ColumnSpacing(8);
        tiles.ColumnDefinitions().Append(c::ColumnDefinition()); tiles.ColumnDefinitions().Append(c::ColumnDefinition());
        overview.Children().Append(tiles); root.Children().Append(overview);
        navigation.Orientation(c::Orientation::Horizontal); navigation.Spacing(8);
        const auto weak = weak_from_this();
        auto back = Button(navigation, _LW("controlCenter.overview"), [weak] { if (const auto self = weak.lock()) self->Select({}); });
        back.Content(c::SymbolIcon(c::Symbol::Back)); back.Padding({8, 6, 8, 6});
        detailTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); detailTitle.VerticalAlignment(x::VerticalAlignment::Center);
        navigation.Children().Append(detailTitle); root.Children().Append(navigation); root.Children().Append(details); root.Children().Append(status);
        if (settings.wifiControls) AddSection("wifi", "statusBar.wifiControls", "network.wifi", L"\uE701");
        if (settings.bluetoothControls) AddSection("bluetooth", "statusBar.bluetoothControls", "bluetooth.devices", L"\uE702");
        if (settings.audioControls) AddSection("audio", "statusBar.audioControls", "audio.devices", L"\uE767");
        if (settings.brightnessControls) AddSection("brightness", "statusBar.brightnessControls", "system.display.brightness", L"\uE706");
        if (settings.mediaControls) AddSection("media", "statusBar.mediaControls", "media.sessions", L"\uE768");
        if (settings.powerControls) AddSection("power", "statusBar.powerControls", "system.power.plans", L"\uE7E8");
        overviewAudio.panel = c::StackPanel(); overviewBrightness.panel = c::StackPanel();
        if (settings.audioControls)
        {
            Slider(overviewAudio, _LW("statusBar.volume"), [this]() -> std::optional<double> {
                const auto snapshot = service->Current("audio.output.volume");
                return snapshot && snapshot->available ? std::optional(j::Numeric(snapshot->value, "volume") * 100) : std::nullopt;
            }, [this](double level) { Start("audio.output.setVolume", {{"volume", std::to_string(level / 100)}}); });
            overview.Children().Append(overviewAudio.panel);
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
            }, [this](double level) { if (!quickBrightnessId.empty()) Start("system.display.setBrightness", {{"monitorId", quickBrightnessId}, {"brightness", std::to_string(level)}}); });
            overview.Children().Append(overviewBrightness.panel);
        }
        Select(initial == StatusBarAction::Audio ? "audio" : initial == StatusBarAction::Network ? "wifi" : initial == StatusBarAction::Power ? "power" : "");
    }
    void Summaries()
    {
        for (auto& [key, section] : sections)
        {
            const auto state = service->Current(section.topic);
            std::wstring label = _LW(!state ? "controlCenter.loading" : !state->available ? "controlCenter.unavailable" : "controlCenter.off");
            if (key == "media")
            {
                const auto media = data->MediaSessions();
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
            section.summary.Text(label);
            Name(section.tile, std::wstring(_LW(section.title.c_str())) + L" · " + label);
            c::ToolTipService::SetToolTip(section.tile, winrt::box_value(label));
        }
        for (const auto& update : overviewAudio.updates) update();
        for (const auto& update : overviewBrightness.updates) update();
    }
    bool Begin(Section& section, const std::string& key)
    { if (section.key == key) return false; section.key = key; section.updates.clear(); section.panel.Children().Clear(); return true; }
    void Audio(Section& section)
    {
        const auto value = Current("audio.devices"); const auto& devices = Items(value, "devices");
        std::string identity = Identity(devices); for (const auto& item : devices) identity += j::Flag(item, "available") ? "1" : "0";
        if (!Begin(section, "audio:" + identity)) return;
        for (const auto* direction : {"output", "input"})
        {
            const std::string prefix = std::string("audio.") + direction;
            std::vector<std::pair<std::string, std::wstring>> choices;
            for (const auto& item : devices) if (j::Flag(item, "available") && j::String(item, "direction") == direction)
                choices.emplace_back(j::String(item, "id"), Wide(j::String(item, "name")));
            Choice(section, _LW(direction == std::string("output") ? "controlCenter.output" : "controlCenter.input"), choices,
                [this, prefix] { return j::String(Current((prefix + ".volume").c_str()), "endpointId"); },
                [this, prefix](const auto& id) { Start(prefix + ".selectDevice", {{"endpointId", id}}); });
            Slider(section, _LW("statusBar.volume"), [this, prefix]() -> std::optional<double> {
                const auto state = service->Current(prefix + ".volume"); return state && state->available ? std::optional(j::Numeric(state->value, "volume") * 100) : std::nullopt;
            }, [this, prefix](double level) { Start(prefix + ".setVolume", {{"volume", std::to_string(level / 100)}}); });
            Toggle(section, _LW("controlCenter.mute"), [this, prefix]() -> std::optional<bool> {
                const auto state = service->Current(prefix + ".volume"); return state && state->available ? std::optional(j::Flag(state->value, "muted")) : std::nullopt;
            }, [this, prefix](bool muted) { Start(prefix + ".setMute", {{"muted", muted ? "1" : "0"}}); });
        }
        Fallback(section, L"ms-settings:sound");
    }
    void Brightness(Section& section)
    {
        const auto value = Current("system.display.brightness"); const auto& monitors = Items(value, "monitors");
        if (!Begin(section, "brightness:" + Identity(monitors))) return;
        for (const auto& monitor : monitors)
        {
            const auto id = j::String(monitor, "id");
            Slider(section, Wide(j::String(monitor, "name")).c_str(), [this, id]() -> std::optional<double> {
                const auto item = Find("system.display.brightness", "monitors", id);
                return j::Flag(item, "available") ? std::optional(j::Numeric(item, "brightness")) : std::nullopt;
            }, [this, id](double level) { Start("system.display.setBrightness", {{"monitorId", id}, {"brightness", std::to_string(level)}}); });
        }
        section.panel.Children().Append(Text(_LW("controlCenter.unsupportedHint"))); Fallback(section, L"ms-settings:display");
    }
    JsonValue WifiInterface() const { return Find("network.wifi", "interfaces", interfaceId); }
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
        const auto current = WifiInterface(); const auto& networks = Items(current, "networks"); const auto& profiles = Items(current, "profiles");
        if (scanOnArrival && !interfaceId.empty()) { scanOnArrival = false; Start("network.wifi.scan", {{"interfaceId", interfaceId}}); }
        if (!Begin(section, "wifi:" + Identity(interfaces) + interfaceId + Identity(networks) + Identity(profiles, "name"))) return;
        std::vector<std::pair<std::string, std::wstring>> adapters;
        for (const auto& item : interfaces) adapters.emplace_back(j::String(item, "id"), Wide(j::String(item, "name")));
        Choice(section, _LW("controlCenter.adapter"), adapters, [this] { return interfaceId; }, [this](const auto& id) {
            interfaceId = id; scanOnArrival = true; sections.at("wifi").key.clear(); Refresh();
        });
        Toggle(section, _LW("statusBar.wifiControls"), [this]() -> std::optional<bool> {
            const auto item = WifiInterface(); const auto* enabled = item.Find("enabled"); return enabled && enabled->IsBoolean() ? std::optional(enabled->boolean) : std::nullopt;
        }, [this](bool enabled) { Start("network.wifi.setRadio", {{"interfaceId", interfaceId}, {"enabled", enabled ? "1" : "0"}}); });
        const auto scan = Button(section.panel, _LW("controlCenter.scan"), [this] { Start("network.wifi.scan", {{"interfaceId", interfaceId}}); });
        section.updates.push_back([this, scan] { scan.IsEnabled(!interfaceId.empty() && j::Flag(WifiInterface(), "enabled")); });
        c::TextBlock error = Text(L""); section.panel.Children().Append(error);
        section.updates.push_back([this, error] { const auto state = WifiInterface(); error.Text(j::String(state, "error") == "accessDenied" ? _LW("controlCenter.locationDenied") : L""); });
        for (const auto& network : networks)
        {
            const auto id = j::String(network, "id");
            auto text = Text(Wide(j::String(network, "ssid")));
            c::ToolTipService::SetToolTip(text, winrt::box_value(Wide(j::String(network, "ssid"))));
            auto row = Row(section.panel, text);
            const auto button = Button(row, _LW("controlCenter.connect"), [this, id] {
                const auto current = WifiInterface();
                for (const auto& item : Items(current, "networks")) if (j::String(item, "id") == id)
                {
                    if (j::Flag(item, "connected")) Start("network.wifi.disconnect", {{"interfaceId", interfaceId}});
                    else if (!j::String(item, "profileName").empty()) Start("network.wifi.connect", {{"interfaceId", interfaceId}, {"profileName", j::String(item, "profileName")}});
                    else if (j::String(item, "security") == "system") Settings(L"ms-settings:network-wifi");
                    else Start("network.wifi.connect", {{"interfaceId", interfaceId}, {"networkId", id}});
                    return;
                }
            });
            section.updates.push_back([this, id, text, button] {
                const auto current = WifiInterface();
                for (const auto& item : Items(current, "networks")) if (j::String(item, "id") == id)
                {
                    text.Text(Wide(j::String(item, "ssid")) + L"  " + std::to_wstring(static_cast<int>(j::Numeric(item, "signal"))) + L"%");
                    button.Content(winrt::box_value(_LW(j::Flag(item, "connected") ? "controlCenter.disconnect" : "controlCenter.connect")));
                    button.IsEnabled(j::Flag(item, "connectable") || j::Flag(item, "connected")); break;
                }
            });
        }
        if (!profiles.empty()) section.panel.Children().Append(Text(_LW("controlCenter.savedNetworks")));
        for (const auto& profile : profiles)
        {
            const auto name = j::String(profile, "name");
            auto row = Row(section.panel, Text(Wide(name)));
            Button(row, _LW("controlCenter.connect"), [this, name] { Start("network.wifi.connect", {{"interfaceId", interfaceId}, {"profileName", name}}); });
            if (!j::Flag(profile, "managed"))
            {
                auto forget = Button(row, _LW("controlCenter.forget"), [this, name] { Start("network.wifi.forget", {{"interfaceId", interfaceId}, {"profileName", name}}); });
                forget.Content(c::SymbolIcon(c::Symbol::Delete)); forget.Padding({8, 6, 8, 6});
                c::ToolTipService::SetToolTip(forget, winrt::box_value(_LW("controlCenter.forget")));
            }
        }
        const auto hidden = Button(section.panel, _LW("controlCenter.hiddenNetwork"), [this] { HiddenNetwork(); });
        section.updates.push_back([this, hidden] { hidden.IsEnabled(!interfaceId.empty() && j::Flag(WifiInterface(), "enabled")); });
        Fallback(section, L"ms-settings:network-wifi");
        Button(section.panel, _LW("controlCenter.locationSettings"), [] { Settings(L"ms-settings:privacy-location"); });
    }
    void Bluetooth(Section& section)
    {
        const auto value = Current("bluetooth.devices"); const auto& radios = Items(value, "radios"); const auto& devices = Items(value, "devices");
        if (!Begin(section, "bluetooth:" + Identity(radios) + Identity(devices))) return;
        for (const auto& radio : radios)
        {
            const auto id = j::String(radio, "id");
            Toggle(section, Wide(j::String(radio, "name")).c_str(), [this, id]() -> std::optional<bool> {
                const auto item = Find("bluetooth.devices", "radios", id); return j::Flag(item, "available") ? std::optional(j::Flag(item, "enabled")) : std::nullopt;
            }, [this, id](bool enabled) { Start("bluetooth.setRadio", {{"radioId", id}, {"enabled", enabled ? "1" : "0"}}); });
        }
        for (const auto& device : devices)
        {
            const auto id = j::String(device, "id"); const auto text = Text(Wide(j::String(device, "name")));
            auto row = Row(section.panel, text);
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
        Choice(section, _LW("controlCenter.powerPlan"), choices, [this] { return j::String(Current("system.power.plans"), "activePlanId"); },
            [this](const auto& id) { Start("system.power.setPlan", {{"planId", id}}); });
        if (j::Flag(value, "modeSupported")) Choice(section, _LW("controlCenter.powerMode"),
            {{"efficiency", _LW("controlCenter.efficiency")}, {"balanced", _LW("controlCenter.balanced")}, {"performance", _LW("controlCenter.performance")}},
            [this] { const auto state = Current("system.power.plans"); return j::String(state, j::Flag(state, "onAC") ? "acMode" : "dcMode"); },
            [this](const auto& mode) { Start("system.power.setMode", {{"mode", mode}}); });
        const auto battery = Text(L""); section.panel.Children().Append(battery);
        section.updates.push_back([this, battery] { const auto state = Current("system.power.plans"); const auto* level = state.Find("batteryPercent");
            battery.Text(level && level->IsNumber() ? std::wstring(_LW("statusBar.battery")) + L" " + std::to_wstring(static_cast<int>(level->number)) + L"%" : L""); });
        for (const auto* action : {"lock", "sleep", "restart", "shutdown"})
        {
            const std::string task = std::string("system.power.") + action;
            Button(section.panel, _LW((std::string("controlCenter.") + action).c_str()), [this, task] { Start(task); });
        }
        Fallback(section, L"ms-settings:powersleep");
    }
    void Media(Section& section)
    {
        const auto snapshot = data->MediaSessions(); std::string key;
        if (snapshot) for (const auto& item : snapshot->sessions) key += item.id + ":";
        if (!Begin(section, "media:" + key)) return;
        std::vector<std::pair<std::string, std::wstring>> choices;
        if (snapshot) for (const auto& item : snapshot->sessions) choices.emplace_back(item.id, Wide(item.sourceName));
        if (snapshot && (mediaId.empty() || std::none_of(snapshot->sessions.begin(), snapshot->sessions.end(), [this](const auto& item) { return item.id == mediaId; })))
            mediaId = snapshot->sessions.empty() ? std::string{} : snapshot->sessions.front().id;
        Choice(section, _LW("statusBar.mediaControls"), choices, [this] { return mediaId; }, [this](const auto& id) { mediaId = id; Refresh(); });
        c::Image art; art.Height(100); art.Stretch(x::Media::Stretch::Uniform); section.panel.Children().Append(art);
        auto token = std::make_shared<std::string>(); const auto title = Text(L""); section.panel.Children().Append(title);
        section.updates.push_back([this, art, token, title] {
            if (const auto state = data->MediaSessions()) for (const auto& item : state->sessions) if (item.id == mediaId) title.Text(Wide(item.title + "\n" + item.artist));
            const auto image = data->MediaArtwork();
            if (!image || !image->available || !image->pixels || image->sessionId != mediaId) { art.Source(nullptr); token->clear(); return; }
            if (*token == image->resourceToken) return; *token = image->resourceToken;
            const auto& pixels = *image->pixels;
            if (!widget_runtime::IsValidWidgetRuntimeImage(pixels)) return;
            x::Media::Imaging::WriteableBitmap bitmap(static_cast<int>(pixels.width), static_cast<int>(pixels.height)); BYTE* bytes = nullptr;
            winrt::check_hresult(bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
            for (std::uint32_t row = 0; row < pixels.height; ++row) std::memcpy(bytes + row * pixels.width * 4, pixels.bgraPremultiplied.data() + row * pixels.stride, pixels.width * 4);
            bitmap.Invalidate(); art.Source(bitmap);
        });
        c::StackPanel playback; playback.Orientation(c::Orientation::Horizontal); playback.Spacing(12);
        playback.HorizontalAlignment(x::HorizontalAlignment::Center); section.panel.Children().Append(playback);
        for (const auto* action : {"previous", "toggle", "next"})
        {
            const std::string task = std::string("media.") + action;
            const auto button = Button(playback, _LW((std::string("controlCenter.") + action).c_str()), [this, task] { Start(task, {{"sessionId", mediaId}}); });
            button.Content(c::SymbolIcon(task == "media.previous" ? c::Symbol::Previous : task == "media.next" ? c::Symbol::Next : c::Symbol::Play));
            button.Width(52); button.Height(40);
            section.updates.push_back([this, button, task] {
                bool enabled = false;
                if (const auto state = data->MediaSessions()) for (const auto& item : state->sessions) if (item.id == mediaId)
                {
                    enabled = task == "media.previous" ? item.controls.canPrevious : task == "media.next" ? item.controls.canNext : item.controls.canPlayPause;
                    if (task == "media.toggle") button.Content(c::SymbolIcon(item.playbackStatus == "playing" ? c::Symbol::Pause : c::Symbol::Play));
                }
                button.IsEnabled(enabled);
            });
        }
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
        for (const auto& completion : service->DrainCompletions(Consumer))
        {
            if (!feedback.Take(completion.id)) continue;
            if (completion.error == "canceled") continue;
            if (completion.ok) { status.IsOpen(false); continue; }
            Notify(completion.error == "accessDenied" ? "controlCenter.accessDenied" :
                completion.error == "timeout" ? "controlCenter.timeout" : completion.error == "passwordRequired" ? "controlCenter.invalidPassword" :
                completion.error == "systemSettingsRequired" || completion.error == "actionUnsupported" ? "controlCenter.unsupportedHint" : "controlCenter.failed", !completion.ok);
        }
    }
    void Close()
    {
        if (closed) return; closed = true;
        feedback.Clear();
        if (dialog) { dialog.Hide(); dialog = nullptr; }
        data->RemoveConsumer(Consumer); root.Children().Clear(); sections.clear();
    }
};
SystemControlView::SystemControlView(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data,
    const StatusBarSettings& settings, StatusBarAction initial, std::function<void()> layoutChanged)
    : impl_(std::make_shared<Impl>(std::move(data)))
{ impl_->Build(settings, initial); impl_->layoutChanged = std::move(layoutChanged); }
SystemControlView::~SystemControlView() { impl_->Close(); }
x::FrameworkElement SystemControlView::Root() const { return impl_->root; }
void SystemControlView::Refresh() { impl_->Refresh(); }
void SystemControlView::Close() { impl_->Close(); }
}
