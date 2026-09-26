#pragma once
#include "json_value.h"

namespace snowdesktop::widget_runtime
{
// Only these six data contracts expose the structured control-service payload.
// Existing audio output topics keep their published typed fields and defaults.
inline bool IsSystemControlDataTopic(std::string_view topic)
{
    return topic == "audio.devices" || topic == "audio.input.volume" ||
        topic == "system.display.brightness" || topic == "network.wifi" ||
        topic == "bluetooth.devices" || topic == "system.power.plans";
}

inline JsonValue PreviewSystemControlData(std::string_view topic, bool empty = false)
{
    std::string_view source = "{}";
    if (topic == "audio.devices")
        source = empty ? R"({"devices":[]})" : R"({"devices":[
            {"id":"audio-output-preview","name":"Preview Speakers","direction":"output","isDefault":true,"available":true,"state":"active"},
            {"id":"audio-input-preview","name":"Preview Microphone","direction":"input","isDefault":true,"available":true,"state":"active"}]})";
    else if (topic == "audio.input.volume")
        source = empty ? "{}" : R"({"endpointId":"audio-input-preview","volume":0.75,"muted":false,"minimum":0,"maximum":1})";
    else if (topic == "system.display.brightness")
        source = empty ? R"({"monitors":[]})" : R"({"monitors":[
            {"id":"brightness-preview","name":"Preview Display","kind":"internal","available":true,"brightness":65}]})";
    else if (topic == "network.wifi")
        source = empty ? R"({"interfaces":[]})" : R"({"interfaces":[
            {"id":"wifi-preview","name":"Preview Wi-Fi","connected":true,"enabled":true,"hardwareEnabled":true,"available":true,
             "networks":[{"id":"network-preview","ssid":"Preview Network","signal":82,"security":"wpa2","connected":true,"connectable":true,"profileName":"Preview Network"}],
             "profiles":[{"name":"Preview Network","managed":false}]}]})";
    else if (topic == "bluetooth.devices")
        source = empty ? R"({"radios":[],"devices":[]})" : R"({
            "radios":[{"id":"bluetooth-radio-preview","name":"Preview Bluetooth","enabled":true,"available":true}],
            "devices":[{"id":"bluetooth-device-preview","name":"Preview Headphones","address":"00:00:00:00:00:00","connected":true,"paired":true,"canConnect":true,"canDisconnect":true,"lowEnergy":false,"batteryPercent":76}]})";
    else if (topic == "system.power.plans")
        source = empty ? R"({"plans":[],"activePlanId":"","modeSupported":false,"batteryPresent":false})" : R"({
            "plans":[{"id":"power-plan-preview","name":"Preview Balanced","active":true}],"activePlanId":"power-plan-preview",
            "modeSupported":true,"acMode":"balanced","dcMode":"balanced","batteryPresent":true,"onAC":false,"batteryPercent":73,"charging":false})";
    JsonValue value;
    (void)ParseJson(source, value);
    return value;
}
}
