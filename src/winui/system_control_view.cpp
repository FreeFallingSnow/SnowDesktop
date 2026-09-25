#include "pch.h"
#include "system_control_view.h"
#include "../widget_system_data_provider.h"
#include "../l10n.h"
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
        std::string topic, key;
        c::StackPanel panel{nullptr};
        c::Expander expander{nullptr};
        std::vector<std::function<void()>> updates;
        bool open = false;
    };
    std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data;
    std::shared_ptr<system_control::Service> service;
    c::StackPanel root;
    c::InfoBar status;
    c::ContentDialog dialog{nullptr};
    std::map<std::string, Section> sections;
    std::string interfaceId, mediaId;
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
    void Start(std::string task, Arguments arguments = {})
    {
        auto request = std::make_shared<Request>(); request->name = std::move(task); request->arguments = std::move(arguments);
        if (system_control::RequiresConfirmation(request->name) || system_control::RequiresPasswordPrompt(*request)) { Prompt(request); return; }
        Submit(request);
    }
    void Submit(const std::shared_ptr<Request>& request)
    {
        if (closed) return;
        const auto id = service->Start(Consumer, std::move(*request));
        Notify(id ? "controlCenter.working" : "controlCenter.failed", !id);
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
        slider.PointerPressed([dragging](const auto&, const auto&) { *dragging = true; });
        slider.PointerReleased([dragging](const auto&, const auto&) { *dragging = false; });
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
    void AddSection(const char* id, const char* title, const char* topic, bool expanded)
    {
        auto& section = sections[id]; section.topic = topic; section.open = expanded;
        section.panel = c::StackPanel(); section.panel.Spacing(8);
        section.expander = c::Expander(); section.expander.Header(winrt::box_value(_LW(title)));
        section.expander.HorizontalAlignment(x::HorizontalAlignment::Stretch); section.expander.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
        section.expander.Content(section.panel); section.expander.IsExpanded(expanded);
        const auto weak = weak_from_this(); const std::string key(id);
        section.expander.Expanding([weak, key](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed) { self->sections.at(key).open = true; self->Demand(key, true); self->Refresh(); }
        });
        section.expander.Collapsed([weak, key](const auto&, const auto&) {
            if (const auto self = weak.lock()) { self->sections.at(key).open = false; self->Demand(key, false); }
        });
        root.Children().Append(section.expander); if (expanded) Demand(key, true);
    }
    void Demand(const std::string& key, bool enable)
    {
        std::vector<std::string> topics{sections.at(key).topic};
        if (key == "audio") topics = {"audio.devices", "audio.output.volume", "audio.input.volume"};
        if (key == "media") topics = {"media.sessions", "media.artwork"};
        for (const auto& topic : topics)
            if (enable) data->StartTopic(Consumer, topic, std::chrono::milliseconds(key == "audio" || key == "media" ? 500 : 2000));
            else data->StopTopic(Consumer, topic);
        if (key == "wifi" && enable) scanOnArrival = true;
    }
    void Build(const StatusBarSettings& settings)
    {
        root.Spacing(8); status.IsClosable(true); root.Children().Append(status);
        if (settings.audioControls) AddSection("audio", "statusBar.audioControls", "audio.devices", true);
        if (settings.brightnessControls) AddSection("brightness", "statusBar.brightnessControls", "system.display.brightness", true);
        if (settings.wifiControls) AddSection("wifi", "statusBar.wifiControls", "network.wifi", false);
        if (settings.bluetoothControls) AddSection("bluetooth", "statusBar.bluetoothControls", "bluetooth.devices", false);
        if (settings.mediaControls) AddSection("media", "statusBar.mediaControls", "media.sessions", false);
        if (settings.powerControls) AddSection("power", "statusBar.powerControls", "system.power.plans", false);
        Refresh();
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
        Button(section.panel, _LW("controlCenter.scan"), [this] { Start("network.wifi.scan", {{"interfaceId", interfaceId}}); });
        c::TextBlock error = Text(L""); section.panel.Children().Append(error);
        section.updates.push_back([this, error] { const auto state = WifiInterface(); error.Text(j::String(state, "error") == "accessDenied" ? _LW("controlCenter.locationDenied") : L""); });
        for (const auto& network : networks)
        {
            const auto id = j::String(network, "id"); c::StackPanel row; row.Spacing(4);
            auto text = Text(Wide(j::String(network, "ssid"))); row.Children().Append(text);
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
            }); section.panel.Children().Append(row);
        }
        if (!profiles.empty()) section.panel.Children().Append(Text(_LW("controlCenter.savedNetworks")));
        for (const auto& profile : profiles)
        {
            const auto name = j::String(profile, "name"); section.panel.Children().Append(Text(Wide(name)));
            Button(section.panel, _LW("controlCenter.connect"), [this, name] { Start("network.wifi.connect", {{"interfaceId", interfaceId}, {"profileName", name}}); });
            if (!j::Flag(profile, "managed")) Button(section.panel, _LW("controlCenter.forget"), [this, name] { Start("network.wifi.forget", {{"interfaceId", interfaceId}, {"profileName", name}}); });
        }
        Button(section.panel, _LW("controlCenter.hiddenNetwork"), [this] { HiddenNetwork(); });
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
            const auto id = j::String(device, "id"); const auto text = Text(Wide(j::String(device, "name"))); section.panel.Children().Append(text);
            const auto button = Button(section.panel, _LW("controlCenter.connect"), [this, id] {
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
        for (const auto* action : {"previous", "toggle", "next"})
        {
            const std::string task = std::string("media.") + action;
            const auto button = Button(section.panel, _LW((std::string("controlCenter.") + action).c_str()), [this, task] { Start(task, {{"sessionId", mediaId}}); });
            section.updates.push_back([this, button, task] {
                bool enabled = false;
                if (const auto state = data->MediaSessions()) for (const auto& item : state->sessions) if (item.id == mediaId)
                    enabled = task == "media.previous" ? item.controls.canPrevious : task == "media.next" ? item.controls.canNext : item.controls.canPlayPause;
                button.IsEnabled(enabled);
            });
        }
    }
    void Refresh()
    {
        if (closed || updating) return;
        updating = true;
        struct Reset { bool& value; ~Reset() { value = false; } } reset{updating};
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
            if (completion.error == "canceled") continue;
            Notify(completion.ok ? "controlCenter.done" : completion.error == "accessDenied" ? "controlCenter.accessDenied" :
                completion.error == "timeout" ? "controlCenter.timeout" : completion.error == "passwordRequired" ? "controlCenter.invalidPassword" :
                completion.error == "systemSettingsRequired" || completion.error == "actionUnsupported" ? "controlCenter.unsupportedHint" : "controlCenter.failed", !completion.ok);
        }
    }
    void Close()
    {
        if (closed) return; closed = true;
        if (dialog) { dialog.Hide(); dialog = nullptr; }
        data->RemoveConsumer(Consumer); sections.clear(); root.Children().Clear();
    }
};
SystemControlView::SystemControlView(std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data, const StatusBarSettings& settings)
    : impl_(std::make_shared<Impl>(std::move(data))) { impl_->Build(settings); }
SystemControlView::~SystemControlView() { impl_->Close(); }
x::FrameworkElement SystemControlView::Root() const { return impl_->root; }
void SystemControlView::Refresh() { impl_->Refresh(); }
void SystemControlView::Close() { impl_->Close(); }
}
