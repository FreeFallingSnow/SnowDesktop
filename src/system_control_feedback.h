#pragma once
#include "system_controls.h"
#include <algorithm>
#include <cmath>
#include <charconv>
#include <thread>
#include <utility>

namespace snowdesktop::system_control
{
struct ControlReadback
{
    std::optional<double> value;
    Result failure;
};
// Only the device read and clock wait are replaceable in tests. A successful
// write is not sufficient: observe the requested value after hardware settles.
template<class Read, class Pause>
Result ConfirmControlValue(double target, double tolerance, const Cancellation& cancel,
    Read&& read, Pause&& pause)
{
    Result failure{false, "stateMismatch", 0};
    for (unsigned attempt = 0; attempt < 20; ++attempt)
    {
        if (cancel.Stop()) return cancel.Failure();
        const auto actual = read();
        if (cancel.Stop()) return cancel.Failure();
        if (!actual.value)
        {
            failure = actual.failure;
            if (failure.ok || failure.error.empty()) failure = {false, "unavailable", 0};
            // A temporarily unreadable value can settle after a successful
            // write. Permission loss/removal/unsupported controls cannot.
            if (failure.error != "unavailable") return failure;
        }
        else if (std::isfinite(*actual.value) && std::abs(*actual.value - target) <= tolerance)
            return {true, {}, 0};
        else failure = {false, "stateMismatch", 0};
        if (attempt != 19) pause();
    }
    return failure;
}
inline void ControlReadbackPause()
{ std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
using BrightnessReadback = ControlReadback;
template<class Read, class Pause>
Result ConfirmBrightness(double target, double tolerance, const Cancellation& cancel,
    Read&& read, Pause&& pause)
{ return ConfirmControlValue(target, tolerance, cancel, std::forward<Read>(read), std::forward<Pause>(pause)); }
inline void BrightnessReadbackPause() { ControlReadbackPause(); }

// A delayed device transition can finish between the backend's last check and
// the service's fresh readback. Only explicit targets can prove that outcome;
// default-volume requests have no bound endpoint and must not borrow another
// device's value. Callers only use this to reconcile stateMismatch, never an OS
// rejection, timeout, cancellation or failed read.
inline bool ControlReadbackMatches(const Request& request, const std::map<std::string, Snapshot>& snapshots)
{
    const auto argument = [&](const char* key) {
        const auto found = request.arguments.find(key);
        return found == request.arguments.end() ? std::string{} : found->second;
    };
    const auto snapshot = [&](const char* topic) -> const JsonValue* {
        const auto found = snapshots.find(topic);
        return found != snapshots.end() && found->second.available && found->second.error.empty() ? &found->second.value : nullptr;
    };
    const auto item = [&](const JsonValue* value, const char* collection, const std::string& id) -> const JsonValue* {
        const auto* list = value ? value->Find(collection) : nullptr;
        if (id.empty() || !list || !list->IsArray()) return nullptr;
        for (const auto& entry : list->array)
            if (json::String(entry, "id") == id && json::Flag(entry, "available") && json::String(entry, "error").empty()) return &entry;
        return nullptr;
    };
    if (request.name == "audio.output.selectDevice")
    {
        const auto* actual = snapshot("audio.output.default"); const auto id = argument("endpointId");
        return actual && !id.empty() && json::String(*actual, "id") == id && json::String(*actual, "state") == "active";
    }
    if (request.name == "audio.input.selectDevice")
    {
        const auto* actual = snapshot("audio.input.volume"); const auto id = argument("endpointId");
        return actual && !id.empty() && json::String(*actual, "endpointId") == id;
    }
    if (request.name == "system.display.setBrightness")
    {
        const auto* actual = item(snapshot("system.display.brightness"), "monitors", argument("monitorId"));
        const auto* level = actual ? actual->Find("brightness") : nullptr;
        const auto wanted = argument("brightness"); double target = 0;
        const auto parsed = std::from_chars(wanted.data(), wanted.data() + wanted.size(), target);
        return parsed.ec == std::errc{} && parsed.ptr == wanted.data() + wanted.size() && std::isfinite(target) &&
            level && level->IsNumber() && std::isfinite(level->number) && std::abs(level->number - target) <= 0.01;
    }
    if (request.name == "network.wifi.setRadio")
    {
        const auto* actual = item(snapshot("network.wifi"), "interfaces", argument("interfaceId"));
        const auto* enabled = actual ? actual->Find("enabled") : nullptr; const auto wanted = argument("enabled");
        return enabled && enabled->IsBoolean() && (wanted == "0" || wanted == "1") && enabled->boolean == (wanted == "1");
    }
    if (request.name == "system.power.setPlan")
    {
        const auto* actual = snapshot("system.power.plans"); const auto id = argument("planId");
        return actual && !id.empty() && json::String(*actual, "activePlanId") == id;
    }
    if (request.name == "system.power.setMode")
    {
        const auto* actual = snapshot("system.power.plans"); const auto mode = argument("mode");
        return actual && (mode == "balanced" || mode == "efficiency" || mode == "performance") && json::Flag(*actual, "modeSupported") &&
            json::String(*actual, "acMode") == mode && json::String(*actual, "dcMode") == mode;
    }
    return false;
}

// UI feedback belongs to the newest request for this control and device, even
// when older completions were already queued before a replacement was issued.
class ControlFeedback
{
    std::map<std::string, std::uint64_t> latest_;
public:
    static std::string Key(const Request& request)
    {
        std::string result = request.name;
        for (const auto* field : {"monitorId", "endpointId", "interfaceId", "deviceId", "radioId", "sessionId"})
            if (const auto it = request.arguments.find(field); it != request.arguments.end())
                result += ":" + std::string(field) + ":" + std::to_string(it->second.size()) + ":" + it->second;
        return result;
    }
    void Track(std::string key, std::uint64_t id)
    { if (id) latest_[std::move(key)] = id; else latest_.erase(key); }
    bool Take(std::uint64_t id)
    {
        const auto found = std::find_if(latest_.begin(), latest_.end(),
            [id](const auto& entry) { return entry.second == id; });
        if (found == latest_.end()) return false;
        latest_.erase(found); return true;
    }
    void Clear() { latest_.clear(); }
};
}
