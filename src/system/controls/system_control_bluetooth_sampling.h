#pragma once
#include "system_controls.h"
#include <algorithm>

namespace snowdesktop::system_control
{
struct BluetoothCollection
{
    JsonValue items = json::Array();
    std::string error;
};

// Radio and paired-device reads have separate failure boundaries. A switched
// off radio is still a controllable device; enumerating peers while it is off
// can fail and must not erase the radio's identity or disable its switch.
template<class ReadRadios, class ReadDevices>
Snapshot SampleBluetooth(ReadRadios readRadios, ReadDevices readDevices, const Cancellation& cancel)
{
    if (cancel.Stop()) return {false, {}, cancel.Failure().error, 0, 0};
    auto radios = readRadios();
    if (!radios.error.empty()) return {false, {}, std::move(radios.error), 0, 0};
    if (cancel.Stop()) return {false, {}, cancel.Failure().error, 0, 0};
    const bool powered = std::any_of(radios.items.array.begin(), radios.items.array.end(), [](const auto& radio) {
        return json::Flag(radio, "available") && json::Flag(radio, "enabled");
    });
    auto devices = powered ? readDevices() : BluetoothCollection{};
    if (cancel.Stop()) return {false, {}, cancel.Failure().error, 0, 0};
    auto value = json::Object();
    value.object["radios"] = std::move(radios.items);
    value.object["devices"] = std::move(devices.items);
    return {true, std::move(value), std::move(devices.error), 0, 0};
}
}
