#pragma once

#include "dock/dock_layout_settings.h"
#include "theme/surface_theme.h"
#include "system/controls/system_quick_controls.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace snowdesktop
{
struct StatusBarAppearanceRule
{
    bool enabled = false;
    SurfaceTheme theme = [] {
        SurfaceTheme value;
        // Panel appearance persistence stores material fields, not a preset ID.
        value.appearance.backgroundPreset = kAppearancePresetCustom;
        return value;
    }();
    friend bool operator==(const StatusBarAppearanceRule&, const StatusBarAppearanceRule&) = default;
};

struct StatusBarSettings
{
    bool enabled = false;
    DockPosition position = DockPosition::Top;
    DockMonitorScope monitorScope = DockMonitorScope::First;
    float scale = 1.0f;
    SurfaceTheme theme;
    StatusBarAppearanceRule noWindow, maximizedWindow;
    // Retired scenes are read and round-tripped under their original JSON keys
    // only. Never reinterpret "has visible windows" as "has no windows".
    StatusBarAppearanceRule legacyShellUi, legacyVisibleWindow;
    bool menu = true, quickSearch = true, taskView = true;
    bool clockSystemPanel = false, controlCenterSystemPanel = false;
    bool clock = true, tray = true, network = true, volume = true, battery = true;
    bool controlCenter = true;
    bool inputMethod = true;
    bool cpu = false, memory = false, gpu = false, traffic = false;
    bool audioControls = true, brightnessControls = true, wifiControls = true;
    bool bluetoothControls = true, mediaControls = true, powerControls = true;
    std::vector<std::string> pinnedTrayItems;
    std::vector<std::string> trayOrder;
    std::vector<std::string> quickControlOrder = {"projection", "hotspot", "airplane", "microphone", "awake", "power"};
    std::vector<std::string> hiddenQuickControls;
    std::vector<std::string> leftOrder = {"menu", "quickSearch", "taskView"};
    std::vector<std::string> rightOrder = {"tray", "cpu", "memory", "gpu", "traffic", "network", "volume", "battery", "controlCenter"};
    friend bool operator==(const StatusBarSettings&, const StatusBarSettings&) = default;
};

template<class Visitor> void VisitStatusBarAppearanceRules(Visitor visit)
{
    visit("noWindow", &StatusBarSettings::noWindow);
    visit("maximizedWindow", &StatusBarSettings::maximizedWindow);
}

template<class Visitor> void VisitStatusBarStoredAppearanceRules(Visitor visit)
{
    VisitStatusBarAppearanceRules(visit);
    visit("shellUi", &StatusBarSettings::legacyShellUi);
    visit("visibleWindow", &StatusBarSettings::legacyVisibleWindow);
}

template<class Visitor> void VisitStatusBarFlags(Visitor visit)
{
    visit("enabled", &StatusBarSettings::enabled);
    visit("menu", &StatusBarSettings::menu);
    visit("quickSearch", &StatusBarSettings::quickSearch);
    visit("taskView", &StatusBarSettings::taskView);
    visit("inputMethod", &StatusBarSettings::inputMethod);
    visit("clockSystemPanel", &StatusBarSettings::clockSystemPanel);
    visit("controlCenterSystemPanel", &StatusBarSettings::controlCenterSystemPanel);
    visit("clock", &StatusBarSettings::clock);
    visit("tray", &StatusBarSettings::tray);
    visit("network", &StatusBarSettings::network);
    visit("volume", &StatusBarSettings::volume);
    visit("battery", &StatusBarSettings::battery);
    visit("controlCenter", &StatusBarSettings::controlCenter);
    visit("cpu", &StatusBarSettings::cpu);
    visit("memory", &StatusBarSettings::memory);
    visit("gpu", &StatusBarSettings::gpu);
    visit("traffic", &StatusBarSettings::traffic);
    visit("audioControls", &StatusBarSettings::audioControls);
    visit("brightnessControls", &StatusBarSettings::brightnessControls);
    visit("wifiControls", &StatusBarSettings::wifiControls);
    visit("bluetoothControls", &StatusBarSettings::bluetoothControls);
    visit("mediaControls", &StatusBarSettings::mediaControls);
    visit("powerControls", &StatusBarSettings::powerControls);
}

inline void NormalizeStatusBarSettings(StatusBarSettings& value)
{
    // Earlier development builds accepted side bars. Retain their other
    // preferences while migrating unsupported positions to the top edge.
    if (value.position != DockPosition::Bottom && value.position != DockPosition::Top)
        value.position = DockPosition::Top;
    value.monitorScope = static_cast<DockMonitorScope>(std::clamp(static_cast<int>(value.monitorScope), 0, 2));
    value.scale = std::isfinite(value.scale) ? std::clamp(value.scale, .75f, 3.0f) : 1.0f;
    const StatusBarSettings defaults;
    const auto normalizeOrder = [](auto& items, const auto& supported) {
        std::vector<std::string> result;
        for (const auto& item : items)
            if (std::find(supported.begin(), supported.end(), item) != supported.end() &&
                std::find(result.begin(), result.end(), item) == result.end()) result.push_back(item);
        for (const auto& item : supported)
            if (std::find(result.begin(), result.end(), item) == result.end()) result.push_back(item);
        items = std::move(result);
    };
    normalizeOrder(value.leftOrder, defaults.leftOrder);
    normalizeOrder(value.rightOrder, defaults.rightOrder);
    normalizeOrder(value.quickControlOrder, defaults.quickControlOrder);
    std::vector<std::string> hidden;
    for (const auto& id : value.hiddenQuickControls)
        if (FindSystemQuickControl(id) && std::find(hidden.begin(), hidden.end(), id) == hidden.end()) hidden.push_back(id);
    value.hiddenQuickControls = std::move(hidden);
    for (auto* items : { &value.pinnedTrayItems, &value.trayOrder })
    {
        std::vector<std::string> normalized;
        for (const auto& item : *items)
            if (!item.empty() && item.size() <= 4096 && normalized.size() < 512 &&
                std::find(normalized.begin(), normalized.end(), item) == normalized.end())
                normalized.push_back(item);
        *items = std::move(normalized);
    }
}

inline bool DecodeStatusBarSettings(const JsonValue& input, StatusBarSettings& output)
{
    if (!input.IsObject()) return false;
    StatusBarSettings value;
    bool valid = true;
    // Missing/1 is the old three-scene schema. Preserve its retired data, keep
    // maximizedWindow unchanged and leave the new noWindow rule disabled.
    if (const auto* version = input.Find("sceneRulesVersion"))
        if (!version->IsNumber() || (version->number != 1 && version->number != 2)) return false;
    VisitStatusBarFlags([&](const char* key, auto member) {
        if (const auto* field = input.Find(key))
        {
            if (!field->IsBoolean()) valid = false;
            else value.*member = field->boolean;
        }
    });
    if (const auto* field = input.Find("position"))
    {
        if (!field->IsNumber() || field->number < 0 || field->number > 3 ||
            std::floor(field->number) != field->number) return false;
        value.position = static_cast<DockPosition>(static_cast<int>(field->number));
    }
    if (const auto* field = input.Find("monitorScope"))
    {
        if (!field->IsNumber() || field->number < 0 || field->number > 2 ||
            std::floor(field->number) != field->number) return false;
        value.monitorScope = static_cast<DockMonitorScope>(static_cast<int>(field->number));
    }
    if (const auto* field = input.Find("scale"))
    {
        if (!field->IsNumber() || !std::isfinite(field->number)) return false;
        value.scale = static_cast<float>(field->number);
    }
    if (const auto* field = input.Find("theme"))
        valid = DecodeSurfaceTheme(*field, value.theme, true) && valid;
    VisitStatusBarStoredAppearanceRules([&](const char* key, auto member) {
        if (const auto* field = input.Find(key))
        {
            if (!field->IsObject()) { valid = false; return; }
            auto& rule = value.*member;
            if (const auto* enabled = field->Find("enabled"))
            {
                if (!enabled->IsBoolean()) valid = false;
                else rule.enabled = enabled->boolean;
            }
            if (const auto* theme = field->Find("theme"))
                valid = DecodeSurfaceTheme(*theme, rule.theme, true) && valid;
        }
    });
    const auto readList = [&](const char* key, auto& list) {
        if (const auto* field = input.Find(key))
        {
            if (!field->IsArray() || field->array.size() > 512) { valid = false; return; }
            list.clear();
            for (const auto& item : field->array)
                if (!item.IsString() || item.string.size() > 4096) valid = false;
                else list.push_back(item.string);
        }
    };
    readList("pinnedTrayItems", value.pinnedTrayItems);
    readList("trayOrder", value.trayOrder);
    readList("quickControlOrder", value.quickControlOrder);
    readList("hiddenQuickControls", value.hiddenQuickControls);
    readList("leftOrder", value.leftOrder);
    readList("rightOrder", value.rightOrder);
    NormalizeStatusBarSettings(value);
    if (valid) output = std::move(value);
    return valid;
}

inline std::string EncodeStatusBarSettings(StatusBarSettings value)
{
    NormalizeStatusBarSettings(value);
    const auto theme = EncodeSurfaceTheme(value.theme, true);
    if (theme.empty()) return {};
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "{\"sceneRulesVersion\":2,\"position\":" << static_cast<int>(value.position)
         << ",\"monitorScope\":" << static_cast<int>(value.monitorScope)
         << ",\"scale\":" << value.scale << ",\"theme\":" << theme;
    bool valid = true;
    VisitStatusBarStoredAppearanceRules([&](const char* key, auto member) {
        const auto& rule = value.*member;
        const auto encoded = EncodeSurfaceTheme(rule.theme, true);
        if (encoded.empty()) { valid = false; return; }
        text << ",\"" << key << "\":{\"enabled\":" << (rule.enabled ? "true" : "false")
             << ",\"theme\":" << encoded << '}';
    });
    if (!valid) return {};
    VisitStatusBarFlags([&](const char* key, auto member) {
        text << ",\"" << key << "\":" << (value.*member ? "true" : "false");
    });
    const auto writeList = [&](const char* key, const auto& list) {
        text << ",\"" << key << "\":[";
        bool first = true;
        for (const auto& item : list)
        {
            if (!first) text << ',';
            first = false;
            text << '"';
            for (const unsigned char c : item)
            {
                if (c == '"' || c == '\\') text << '\\' << static_cast<char>(c);
                else if (c < 0x20)
                {
                    constexpr char hex[] = "0123456789abcdef";
                    text << "\\u00" << hex[c >> 4] << hex[c & 15];
                }
                else text << static_cast<char>(c);
            }
            text << '"';
        }
        text << ']';
    };
    writeList("pinnedTrayItems", value.pinnedTrayItems);
    writeList("trayOrder", value.trayOrder);
    writeList("quickControlOrder", value.quickControlOrder);
    writeList("hiddenQuickControls", value.hiddenQuickControls);
    writeList("leftOrder", value.leftOrder);
    writeList("rightOrder", value.rightOrder);
    text << '}';
    return text.str();
}
}
