#pragma once
#include "system_controls.h"
#include <utility>

namespace snowdesktop::system_control
{
struct WifiRadioReadback
{
    bool available = false, enabled = false, hardwareEnabled = false;
    std::string error;
};
struct WifiNetworksReadback
{
    JsonValue items = json::Array();
    std::string error;
    bool radioOff = false;
};

// Radio availability and nearby-network enumeration are different boundaries.
// A software-disabled interface must keep its identity and usable radio switch.
template<class ReadRadio, class ReadNetworks>
void SampleWifiInterface(JsonValue& value, ReadRadio readRadio, ReadNetworks readNetworks)
{
    auto radio = readRadio();
    auto networks = radio.available && radio.enabled && radio.hardwareEnabled ? readNetworks() : WifiNetworksReadback{};
    if (networks.radioOff)
    {
        // The radio may have changed between the two OS queries. Read its state
        // again instead of interpreting POWER_STATE_INVALID as missing hardware.
        radio = readRadio();
        if (radio.available && (!radio.enabled || !radio.hardwareEnabled)) networks = {};
    }
    value.object["available"] = json::Boolean(radio.available);
    value.object.erase("enabled"); value.object.erase("hardwareEnabled"); value.object.erase("error");
    if (radio.available)
    {
        value.object["enabled"] = json::Boolean(radio.enabled);
        value.object["hardwareEnabled"] = json::Boolean(radio.hardwareEnabled);
        if (!radio.enabled || !radio.hardwareEnabled) value.object["connected"] = json::Boolean(false);
    }
    const auto& error = radio.error.empty() ? networks.error : radio.error;
    if (!error.empty()) value.object["error"] = json::Text(error);
    value.object["networks"] = std::move(networks.items);
}
}
