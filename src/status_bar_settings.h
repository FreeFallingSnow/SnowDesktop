#pragma once

#include "dock_layout_settings.h"
#include "surface_theme.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace snowdesktop
{
struct StatusBarSettings
{
    bool enabled = false;
    DockPosition position = DockPosition::Top;
    DockMonitorScope monitorScope = DockMonitorScope::First;
    float scale = 1.0f;
    SurfaceTheme theme;
    bool menu = true, quickSearch = true;
    bool clock = true, tray = true, network = true, volume = true, battery = true;
    bool controlCenter = true;
    bool cpu = false, memory = false, gpu = false, traffic = false;
    bool audioControls = true, brightnessControls = true, wifiControls = true;
    bool bluetoothControls = true, mediaControls = true, powerControls = true;
    std::vector<std::string> pinnedTrayItems;
    std::vector<std::string> trayOrder;
    std::vector<std::string> leftOrder = {"menu", "quickSearch"};
    std::vector<std::string> rightOrder = {"tray", "cpu", "memory", "gpu", "traffic", "network", "volume", "battery", "controlCenter"};
    friend bool operator==(const StatusBarSettings&, const StatusBarSettings&) = default;
};

template<class Visitor> void VisitStatusBarFlags(Visitor visit)
{
    visit("enabled", &StatusBarSettings::enabled);
    visit("menu", &StatusBarSettings::menu);
    visit("quickSearch", &StatusBarSettings::quickSearch);
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
        valid = DecodeSurfaceTheme(*field, value.theme) && valid;
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
    readList("leftOrder", value.leftOrder);
    readList("rightOrder", value.rightOrder);
    NormalizeStatusBarSettings(value);
    if (valid) output = std::move(value);
    return valid;
}

inline std::string EncodeStatusBarSettings(StatusBarSettings value)
{
    NormalizeStatusBarSettings(value);
    const auto theme = EncodeSurfaceTheme(value.theme);
    if (theme.empty()) return {};
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "{\"position\":" << static_cast<int>(value.position)
         << ",\"monitorScope\":" << static_cast<int>(value.monitorScope)
         << ",\"scale\":" << value.scale << ",\"theme\":" << theme;
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
    writeList("leftOrder", value.leftOrder);
    writeList("rightOrder", value.rightOrder);
    text << '}';
    return text.str();
}
}
