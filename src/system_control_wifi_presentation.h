#pragma once
#include "system_controls.h"
#include <algorithm>

namespace snowdesktop::system_control
{
// A per-interface presentation list. Keep raw provider data and profile APIs
// intact; callbacks resolve this same list so a duplicate cannot pick a
// different profile from the one represented by the visible row.
inline std::vector<JsonValue> WifiPresentationNetworks(const JsonValue& adapter)
{
    const auto* networks = adapter.Find("networks");
    std::vector<JsonValue> result;
    if (!networks || !networks->IsArray()) return result;
    for (const auto& network : networks->array)
    {
        const auto id = json::String(network, "id");
        if (id.empty() || (json::String(network, "ssid").empty() && !json::Flag(network, "connected"))) continue;
        const auto existing = std::find_if(result.begin(), result.end(), [&](const auto& item) { return json::String(item, "id") == id; });
        if (existing == result.end()) { result.push_back(network); continue; }
        const auto rank = [](const auto& value) {
            return (json::Flag(value, "connected") ? 4 : 0) + (json::Flag(value, "connectable") ? 2 : 0) +
                (json::String(value, "profileName").empty() ? 0 : 1);
        };
        const double signal = (std::max)(json::Numeric(*existing, "signal"), json::Numeric(network, "signal"));
        const bool connectable = json::Flag(*existing, "connectable") || json::Flag(network, "connectable");
        if (rank(network) > rank(*existing)) *existing = network;
        existing->object["signal"] = json::Number(signal);
        existing->object["connectable"] = json::Boolean(connectable);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& first, const auto& second) {
        if (json::Flag(first, "connected") != json::Flag(second, "connected")) return json::Flag(first, "connected");
        return json::String(first, "ssid") < json::String(second, "ssid");
    });
    return result;
}
}
