#pragma once
#include "system_controls.h"
#include <algorithm>
#include <utility>

namespace snowdesktop::system_control
{
inline std::string AudioEndpointName(std::string friendly, std::string adapter = {}, std::string description = {})
{
    const auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    };
    friendly = trim(std::move(friendly));
    if (!friendly.empty()) return friendly;
    adapter = trim(std::move(adapter)); description = trim(std::move(description));
    if (description.empty()) return adapter;
    if (adapter.empty() || adapter == description) return description;
    return description + " (" + adapter + ")";
}

// Presentation filtering does not narrow the public audio.devices snapshot:
// inactive endpoints remain available to Lua callers. Endpoint identity, not
// display name or hardware/virtual classification, determines duplicates.
inline std::vector<JsonValue> AudioPresentationDevices(const JsonValue& value, std::string_view direction)
{
    std::vector<JsonValue> result;
    const auto* devices = value.Find("devices");
    if (!devices || !devices->IsArray()) return result;
    for (const auto& device : devices->array)
    {
        const auto id = json::String(device, "id");
        const auto name = AudioEndpointName(json::String(device, "name"));
        if (id.empty() || name.empty() || json::String(device, "direction") != direction ||
            !json::Flag(device, "available") || json::String(device, "state") != "active") continue;
        const auto existing = std::find_if(result.begin(), result.end(), [&](const auto& item) {
            return json::String(item, "id") == id;
        });
        if (existing == result.end())
        { result.push_back(device); result.back().object["name"] = json::Text(name); }
        else if (json::Flag(device, "isDefault")) existing->object["isDefault"] = json::Boolean(true);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& first, const auto& second) {
        if (json::Flag(first, "isDefault") != json::Flag(second, "isDefault")) return json::Flag(first, "isDefault");
        return json::String(first, "name") < json::String(second, "name");
    });
    return result;
}
}
