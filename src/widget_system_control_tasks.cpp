#include "widget_system_control_tasks.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <utility>

namespace snowdesktop::widget_runtime
{
bool IsSystemControlTask(std::string_view name)
{
    return name == "audio.output.selectDevice" || name == "audio.input.selectDevice" ||
        name == "audio.input.setVolume" || name == "audio.input.setMute" ||
        name == "system.display.setBrightness" || name == "network.wifi.setRadio" ||
        name == "network.wifi.scan" || name == "network.wifi.connect" ||
        name == "network.wifi.disconnect" || name == "network.wifi.forget" ||
        name == "bluetooth.setRadio" || name == "bluetooth.connect" || name == "bluetooth.disconnect" ||
        name == "system.power.setPlan" || name == "system.power.setMode" ||
        name == "system.power.lock" || name == "system.power.sleep" ||
        name == "system.power.restart" || name == "system.power.shutdown";
}
SystemControlArgumentKind SystemControlArgumentType(std::string_view key)
{
    if (key == "volume" || key == "brightness") return SystemControlArgumentKind::Number;
    if (key == "muted" || key == "enabled" || key == "hidden") return SystemControlArgumentKind::Boolean;
    if (key == "endpointId" || key == "monitorId" || key == "interfaceId" ||
        key == "networkId" || key == "profileName" || key == "ssid" || key == "security" ||
        key == "radioId" || key == "deviceId" || key == "planId" || key == "mode")
        return SystemControlArgumentKind::Text;
    return SystemControlArgumentKind::Invalid;
}
bool MakeSystemControlRequest(std::string_view name,
    const std::unordered_map<std::string, std::string>& arguments, system_control::Request& request)
{
    if (!IsSystemControlTask(name)) return false;
    request = {}; request.name = name;
    for (const auto& [key, value] : arguments)
    {
        const auto type = SystemControlArgumentType(key);
        if (type == SystemControlArgumentKind::Invalid || value.empty() ||
            value.size() > 4096 || value.find('\0') != std::string::npos) return false;
        if (type == SystemControlArgumentKind::Number)
        {
            double number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(number)) return false;
            request.arguments[key] = key == "volume" && number < 0 ? "0" : key == "volume" && number > 1 ? "1" : value;
        }
        else request.arguments[key] = value;
    }
    if (name == "system.power.setMode")
    {
        const auto mode = request.arguments.find("mode");
        if (mode == request.arguments.end() || (mode->second != "efficiency" && mode->second != "balanced" && mode->second != "performance")) return false;
    }
    if (name == "network.wifi.connect" && request.arguments.contains("ssid") && !request.arguments.contains("security")) return false;
    return system_control::ValidateRequest(request);
}
WidgetSystemControlTasks::WidgetSystemControlTasks(std::shared_ptr<system_control::Service> service)
    : service_(std::move(service)) {}
system_control::Result DispatchSystemControlTask(system_control::Request& request, bool preview,
    const std::function<system_control::Result(system_control::Request&)>& live)
{
    if (!IsSystemControlTask(request.name) || !system_control::ValidateRequest(request)) return {false, "invalidArguments", 0};
    if (preview) return {true, {}, 0};
    return live ? live(request) : system_control::Result{false, "providerUnavailable", 0};
}
WidgetSystemControlTasks::~WidgetSystemControlTasks()
{
    while (!active_.empty()) Forget(active_.begin()->second.owner);
}
std::string WidgetSystemControlTasks::Consumer(std::uint64_t owner)
{ return "lua-controls:" + std::to_string(owner); }
bool WidgetSystemControlTasks::Start(std::uint64_t id, std::uint64_t owner, system_control::Request request)
{
    if (!service_ || !id || !owner || active_.contains(id) || !IsSystemControlTask(request.name)) return false;
    const auto serviceId = service_->Start(Consumer(owner), std::move(request));
    if (!serviceId) return false;
    active_.emplace(id, Active{serviceId, owner}); return true;
}
void WidgetSystemControlTasks::ReleaseIdle(std::uint64_t owner)
{
    if (service_ && std::none_of(active_.begin(), active_.end(), [owner](const auto& entry) { return entry.second.owner == owner; }))
        service_->RemoveConsumer(Consumer(owner));
}
bool WidgetSystemControlTasks::Cancel(std::uint64_t id)
{
    const auto found = active_.find(id); if (found == active_.end()) return false;
    const auto owner = found->second.owner;
    (void)service_->Cancel(found->second.serviceId);
    active_.erase(found); canceled_.push_back({id, false, "canceled"}); ReleaseIdle(owner); return true;
}
void WidgetSystemControlTasks::Forget(std::uint64_t owner)
{
    std::vector<std::uint64_t> ids;
    for (const auto& [id, entry] : active_) if (entry.owner == owner) ids.push_back(id);
    for (const auto id : ids) Cancel(id);
}
std::vector<WidgetSystemControlTasks::Completion> WidgetSystemControlTasks::Drain()
{
    auto result = std::exchange(canceled_, {});
    std::set<std::uint64_t> owners;
    for (const auto& [id, entry] : active_) { (void)id; owners.insert(entry.owner); }
    for (const auto owner : owners)
    {
        for (auto& completion : service_->DrainCompletions(Consumer(owner)))
        {
            const auto found = std::find_if(active_.begin(), active_.end(), [&](const auto& entry) {
                return entry.second.owner == owner && entry.second.serviceId == completion.id;
            });
            if (found == active_.end()) continue;
            result.push_back({found->first, completion.ok, std::move(completion.error)});
            active_.erase(found);
        }
        ReleaseIdle(owner);
    }
    return result;
}
}
