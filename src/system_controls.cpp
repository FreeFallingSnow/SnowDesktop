#include "system_controls.h"
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace snowdesktop::system_control
{
Secret::Secret(std::wstring_view text) : bytes_(text.begin(), text.end()) {}
Secret::~Secret() { Clear(); }
Secret::Secret(Secret&& other) noexcept : bytes_(std::move(other.bytes_)) {}
Secret& Secret::operator=(Secret&& other) noexcept
{ if (this != &other) { Clear(); bytes_ = std::move(other.bytes_); } return *this; }
std::wstring_view Secret::View() const { return {bytes_.data(), bytes_.size()}; }
void Secret::Clear() { if (!bytes_.empty()) SecureZeroMemory(bytes_.data(), bytes_.size() * sizeof(wchar_t)); bytes_.clear(); }

namespace
{
using Clock = std::chrono::steady_clock;
const std::set<std::string_view> topics{
    "audio.devices", "audio.output.default", "audio.output.volume", "audio.input.volume",
    "system.display.brightness", "network.wifi", "bluetooth.devices", "system.power.plans"};
struct TaskRule { const char* name; std::vector<std::string_view> required, optional; };
const std::vector<TaskRule> rules{
    {"audio.output.setVolume", {"volume"}, {}}, {"audio.output.setMute", {"muted"}, {}},
    {"audio.output.selectDevice", {"endpointId"}, {}}, {"audio.input.selectDevice", {"endpointId"}, {}},
    {"audio.input.setVolume", {"volume"}, {}}, {"audio.input.setMute", {"muted"}, {}},
    {"system.display.setBrightness", {"monitorId", "brightness"}, {}},
    {"network.wifi.setRadio", {"interfaceId", "enabled"}, {}},
    {"network.wifi.scan", {"interfaceId"}, {}},
    {"network.wifi.connect", {"interfaceId"}, {"networkId", "profileName", "ssid", "hidden", "security"}},
    {"network.wifi.disconnect", {"interfaceId"}, {}}, {"network.wifi.forget", {"interfaceId", "profileName"}, {}},
    {"bluetooth.setRadio", {"radioId", "enabled"}, {}},
    {"bluetooth.connect", {"deviceId"}, {}}, {"bluetooth.disconnect", {"deviceId"}, {}},
    {"system.power.setPlan", {"planId"}, {}}, {"system.power.setMode", {"mode"}, {}},
    {"system.power.lock", {}, {}}, {"system.power.sleep", {}, {}},
    {"system.power.restart", {}, {}}, {"system.power.shutdown", {}, {}},
    {"media.play", {}, {"sessionId"}}, {"media.pause", {}, {"sessionId"}},
    {"media.toggle", {}, {"sessionId"}}, {"media.stop", {}, {"sessionId"}},
    {"media.next", {}, {"sessionId"}}, {"media.previous", {}, {"sessionId"}},
    {"media.seek", {"positionMs"}, {"sessionId"}}, {"media.setRate", {"rate"}, {"sessionId"}},
    {"media.setShuffle", {"enabled"}, {"sessionId"}}, {"media.setRepeat", {"mode"}, {"sessionId"}}
};
bool Slider(std::string_view task)
{ return task.ends_with(".setVolume") || task.ends_with(".setBrightness"); }
std::string Target(const Request& request)
{
    for (const auto* key : {"monitorId", "endpointId", "interfaceId", "deviceId", "radioId"})
        if (const auto found = request.arguments.find(key); found != request.arguments.end()) return found->second;
    return {};
}
std::int64_t Timestamp()
{ return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}
bool SupportsTopic(std::string_view topic) { return topics.contains(topic); }
bool SupportsTask(std::string_view task)
{ return std::any_of(rules.begin(), rules.end(), [&](const auto& rule) { return rule.name == task; }); }
std::string_view Source(std::string_view name)
{
    if (name.starts_with("audio.")) return "audio";
    if (name.starts_with("system.display.")) return "brightness";
    if (name.starts_with("network.wifi")) return "wifi";
    if (name.starts_with("bluetooth.")) return "bluetooth";
    if (name.starts_with("system.power.")) return "power";
    if (name.starts_with("media.")) return "media";
    return {};
}
bool RequiresConfirmation(std::string_view name)
{ return name == "network.wifi.forget" || name == "system.power.sleep" || name == "system.power.restart" || name == "system.power.shutdown"; }
bool RequiresPasswordPrompt(const Request& request)
{ return request.name == "network.wifi.connect" && !request.arguments.contains("profileName"); }
bool ValidateRequest(const Request& request)
{
    const auto rule = std::find_if(rules.begin(), rules.end(), [&](const auto& value) { return value.name == request.name; });
    if (rule == rules.end()) return false;
    for (const auto key : rule->required)
    {
        const auto found = request.arguments.find(std::string(key));
        if (found == request.arguments.end() || found->second.empty()) return false;
    }
    for (const auto& [key, value] : request.arguments)
    {
        if (std::find(rule->required.begin(), rule->required.end(), key) == rule->required.end() &&
            std::find(rule->optional.begin(), rule->optional.end(), key) == rule->optional.end()) return false;
        if (value.size() > 4096 || value.find('\0') != std::string::npos) return false;
        if (key == "enabled" || key == "muted" || key == "hidden")
        { if (value != "0" && value != "1") return false; }
        if (key == "volume" || key == "brightness" || key == "positionMs" || key == "rate")
        {
            double number = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !std::isfinite(number)) return false;
            if ((key == "brightness" && (number < 0 || number > 100)) ||
                (key == "positionMs" && (number < 0 || std::floor(number) != number)) ||
                (key == "rate" && number <= 0)) return false;
        }
    }
    if (request.name == "network.wifi.connect")
    {
        int targets = 0;
        for (const auto* key : {"networkId", "profileName", "ssid"})
            if (const auto found = request.arguments.find(key); found != request.arguments.end())
            { if (found->second.empty()) return false; ++targets; }
        if (targets != 1) return false;
        if (const auto found = request.arguments.find("ssid"); found != request.arguments.end() && found->second.size() > 32) return false;
        if (const auto found = request.arguments.find("security"); found != request.arguments.end() &&
            found->second != "open" && found->second != "wpa2" && found->second != "wpa3") return false;
    }
    return true;
}

struct Service::Impl
{
    struct Demand { std::map<std::string, std::chrono::milliseconds> consumers; };
    struct Work
    {
        std::uint64_t id = 0, generation = 0;
        std::string consumer;
        Request request;
        Cancellation cancel;
        Clock::time_point ready;
    };
    std::shared_ptr<Backend> backend;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, Demand> demands;
    std::map<std::string, Clock::time_point> due;
    std::map<std::string, Snapshot> snapshots;
    std::map<std::string, std::uint64_t> generations;
    std::map<std::string, std::vector<Completion>> completed;
    std::set<std::string> changedTopics;
    std::deque<std::shared_ptr<Work>> pending;
    std::map<std::uint64_t, std::shared_ptr<Work>> active;
    std::shared_ptr<std::atomic_bool> sampleCanceled = std::make_shared<std::atomic_bool>(false);
    std::jthread sampler;
    std::vector<std::jthread> executors;
    // Reserve one physical source across Sample, Execute/readback and Release.
    // Backend locks are not an ownership boundary: a release queued before an
    // executor's readback could otherwise close and then recreate its resources.
    std::set<std::string> busySources;
    Wake wake;
    std::uint64_t next = 0, revision = 0;
    bool stopping = false;
    explicit Impl(std::shared_ptr<Backend> value) : backend(value ? std::move(value) : CreateWindowsBackend()) {}
    std::map<std::string, std::chrono::milliseconds> Sources()
    {
        std::map<std::string, std::chrono::milliseconds> result;
        for (const auto& [topic, demand] : demands)
            for (const auto& [consumer, interval] : demand.consumers)
            {
                (void)consumer;
                const std::string source(Source(topic));
                const auto found = result.find(source);
                if (found == result.end()) result[source] = interval;
                else found->second = (std::min)(found->second, interval);
            }
        return result;
    }
    void Publish(std::map<std::string, Snapshot> values)
    {
        Wake notify;
        {
            std::lock_guard guard(mutex);
            if (stopping) return;
            for (auto& [topic, value] : values)
            {
                value.timestampMs = Timestamp(); value.revision = ++revision;
                snapshots[topic] = std::move(value); changedTopics.insert(topic);
            }
            notify = wake;
        }
        if (notify) notify();
    }
    std::map<std::string, Snapshot> Sample(const std::string& source, const Cancellation& cancel)
    {
        try { return backend->Sample(source, cancel); }
        catch (...)
        {
            std::map<std::string, Snapshot> values;
            for (const auto topic : topics) if (Source(topic) == source) values[std::string(topic)].error = "unavailable";
            return values;
        }
    }
    void Samples(std::stop_token token)
    {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!token.stop_requested())
        {
            std::string source;
            bool release = false;
            {
                std::unique_lock guard(mutex);
                if (stopping) break;
                const auto sources = Sources();
                const auto now = Clock::now();
                const auto readyControl = [&](std::string_view key) {
                    return std::any_of(pending.begin(), pending.end(), [&](const auto& task) {
                        return Source(task->request.name) == key &&
                            (task->ready <= now || task->cancel.Stop());
                    });
                };
                for (auto it = due.begin(); it != due.end(); ++it)
                {
                    if (!sources.contains(it->first) && !busySources.contains(it->first) && !readyControl(it->first))
                    { source = it->first; release = true; due.erase(it); break; }
                }
                auto nextDue = Clock::time_point::max();
                auto selectedDue = Clock::time_point::max();
                for (const auto& [key, interval] : sources)
                {
                    (void)interval;
                    // Slow samples can leave their next deadline overdue. Give
                    // ready/canceled controls the next turn instead of sampling
                    // continuously until those requests reach their deadline.
                    if (busySources.contains(key) || readyControl(key)) continue;
                    auto& time = due[key];
                    if (!release && time <= now && time < selectedDue) { source = key; selectedDue = time; }
                    nextDue = (std::min)(nextDue, time);
                }
                if (source.empty())
                {
                    if (nextDue == Clock::time_point::max()) changed.wait(guard);
                    else changed.wait_until(guard, nextDue);
                    continue;
                }
                if (!release) due[source] = now + sources.at(source);
                busySources.insert(source);
            }
            if (release) backend->Release(source);
            else if (!token.stop_requested())
                Publish(Sample(source, {sampleCanceled, Clock::now() + std::chrono::seconds(5)}));
            {
                std::lock_guard guard(mutex);
                busySources.erase(source);
            }
            changed.notify_all();
        }
        // Shutdown releases only after both sampling and executors have joined.
        if (SUCCEEDED(apartment)) CoUninitialize();
    }
    void Finish(const std::shared_ptr<Work>& work, Result result)
    {
        Wake notify;
        {
            std::lock_guard guard(mutex);
            active.erase(work->id);
            const std::string source(Source(work->request.name));
            busySources.erase(source);
            // A direct task may have no subscription. Keep its cleanup visible
            // to the sampler even when the last consumer left during readback.
            due.try_emplace(source, Clock::now());
            if (work->generation == generations[work->consumer])
            {
                auto& values = completed[work->consumer];
                if (values.size() < 512) values.push_back({std::move(result), work->id});
            }
            notify = wake;
        }
        work->request.password.Clear();
        changed.notify_all();
        if (notify) notify();
    }
    void Execute(std::stop_token token)
    {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!token.stop_requested())
        {
            std::shared_ptr<Work> work;
            {
                std::unique_lock guard(mutex);
                if (stopping) break;
                auto selected = pending.end();
                auto nextDue = Clock::time_point::max();
                for (auto it = pending.begin(); it != pending.end(); ++it)
                {
                    if (busySources.contains(std::string(Source((*it)->request.name)))) continue;
                    if ((*it)->ready <= Clock::now() || (*it)->cancel.Stop()) { selected = it; break; }
                    nextDue = (std::min)(nextDue, (*it)->ready);
                }
                if (selected == pending.end())
                {
                    if (nextDue == Clock::time_point::max()) changed.wait(guard);
                    else changed.wait_until(guard, nextDue);
                    continue;
                }
                work = std::move(*selected); pending.erase(selected);
                busySources.insert(std::string(Source(work->request.name)));
            }
            Result result;
            if (work->cancel.Stop()) result = work->cancel.Failure();
            else
            {
                try { result = backend->Execute(work->request, work->cancel); }
                catch (...) { result = {false, "unavailable", 0}; }
                if (work->cancel.Stop()) result = work->cancel.Failure();
                // Read actual state even after a rejected request. An OS API may
                // time out after applying part of a device transition.
                if (!token.stop_requested() && !work->cancel.Canceled() && Source(work->request.name) != "media")
                {
                    const std::string source(Source(work->request.name));
                    Publish(Sample(source, {work->cancel.canceled, Clock::now() + std::chrono::seconds(5)}));
                    std::lock_guard guard(mutex);
                    const auto sources = Sources();
                    const auto demand = sources.find(source);
                    if (demand != sources.end()) due[source] = Clock::now() + demand->second;
                }
            }
            Finish(work, std::move(result));
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
    }
};
Service::Service(std::shared_ptr<Backend> backend) : impl_(std::make_unique<Impl>(std::move(backend))) {}
Service::~Service() { Shutdown(); }
bool Service::Subscribe(std::string consumer, std::string topic, std::chrono::milliseconds interval)
{
    if (consumer.empty() || !SupportsTopic(topic) || interval < std::chrono::milliseconds(10) || interval > std::chrono::hours(24)) return false;
    std::lock_guard guard(impl_->mutex);
    if (impl_->stopping) return false;
    impl_->demands[topic].consumers[std::move(consumer)] = interval;
    const std::string source(Source(topic));
    const auto scheduled = impl_->due.find(source);
    if (scheduled == impl_->due.end()) impl_->due[source] = Clock::now();
    else scheduled->second = (std::min)(scheduled->second, Clock::now() + interval);
    if (!impl_->sampler.joinable()) impl_->sampler = std::jthread([this](std::stop_token token) { impl_->Samples(token); });
    impl_->changed.notify_all(); return true;
}
bool Service::Unsubscribe(std::string_view consumer, std::string_view topic)
{
    std::lock_guard guard(impl_->mutex);
    const auto found = impl_->demands.find(std::string(topic));
    if (found == impl_->demands.end() || !found->second.consumers.erase(std::string(consumer))) return false;
    if (found->second.consumers.empty()) impl_->demands.erase(found);
    impl_->changed.notify_all();
    return true;
}
void Service::RemoveConsumer(std::string_view consumer)
{
    std::lock_guard guard(impl_->mutex);
    const std::string key(consumer);
    for (auto it = impl_->demands.begin(); it != impl_->demands.end();)
    {
        it->second.consumers.erase(key);
        if (it->second.consumers.empty()) it = impl_->demands.erase(it); else ++it;
    }
    for (const auto& [id, work] : impl_->active) { (void)id; if (work->consumer == key) work->cancel.canceled->store(true); }
    ++impl_->generations[key]; impl_->completed.erase(key); impl_->changed.notify_all();
}
std::optional<Snapshot> Service::Current(std::string_view topic) const
{
    std::lock_guard guard(impl_->mutex); const auto found = impl_->snapshots.find(std::string(topic));
    return found == impl_->snapshots.end() ? std::nullopt : std::optional(found->second);
}
std::vector<std::string> Service::DrainChangedTopics()
{
    std::lock_guard guard(impl_->mutex);
    std::vector<std::string> result(impl_->changedTopics.begin(), impl_->changedTopics.end()); impl_->changedTopics.clear(); return result;
}
std::size_t Service::ActiveTopicCount() const { std::lock_guard guard(impl_->mutex); return impl_->demands.size(); }
std::optional<std::chrono::milliseconds> Service::EffectiveInterval(std::string_view topic) const
{
    std::lock_guard guard(impl_->mutex); const auto found = impl_->demands.find(std::string(topic));
    if (found == impl_->demands.end()) return {};
    auto result = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours(24));
    for (const auto& [consumer, interval] : found->second.consumers) { (void)consumer; result = (std::min)(result, interval); }
    return result;
}
void Service::StopAll()
{
    // Host lifecycle operation, serialized with provider start/stop calls.
    auto backend = impl_->backend; Shutdown(); impl_ = std::make_unique<Impl>(std::move(backend));
}
std::uint64_t Service::Start(std::string consumer, Request request)
{
    if (consumer.empty() || !ValidateRequest(request) || (RequiresConfirmation(request.name) && !request.hostConfirmed)) return 0;
    std::lock_guard guard(impl_->mutex);
    if (impl_->stopping || impl_->active.size() >= 256) return 0;
    auto work = std::make_shared<Impl::Work>(); work->id = ++impl_->next;
    work->consumer = std::move(consumer); work->generation = impl_->generations[work->consumer];
    work->request = std::move(request);
    work->cancel = {std::make_shared<std::atomic_bool>(false), Clock::now() + std::chrono::seconds(40)};
    work->ready = Clock::now() + (Slider(work->request.name) ? std::chrono::milliseconds(100) : std::chrono::milliseconds(0));
    if (Slider(work->request.name)) for (const auto& queued : impl_->pending)
        if (queued->consumer == work->consumer && queued->request.name == work->request.name &&
            Target(queued->request) == Target(work->request)) queued->cancel.canceled->store(true);
    impl_->active[work->id] = work; impl_->pending.push_back(work);
    if (!impl_->sampler.joinable()) impl_->sampler = std::jthread([this](std::stop_token token) { impl_->Samples(token); });
    if (impl_->executors.empty()) for (unsigned index = 0; index < 3; ++index)
        impl_->executors.emplace_back([this](std::stop_token token) { impl_->Execute(token); });
    impl_->changed.notify_all(); return work->id;
}
bool Service::Cancel(std::uint64_t id)
{
    std::lock_guard guard(impl_->mutex); const auto found = impl_->active.find(id);
    if (found == impl_->active.end()) return false;
    found->second->cancel.canceled->store(true); impl_->changed.notify_all(); return true;
}
std::vector<Completion> Service::DrainCompletions(std::string_view consumer)
{
    std::lock_guard guard(impl_->mutex);
    const auto found = impl_->completed.find(std::string(consumer));
    if (found == impl_->completed.end()) return {};
    auto result = std::move(found->second); impl_->completed.erase(found); return result;
}
void Service::SetWake(Wake wake) { std::lock_guard guard(impl_->mutex); impl_->wake = std::move(wake); }
void Service::Invalidate(std::string_view source)
{
    std::lock_guard guard(impl_->mutex);
    for (auto& [key, time] : impl_->due) if (source.empty() || key == source) time = Clock::now();
    impl_->changed.notify_all();
}
void Service::Shutdown()
{
    {
        std::lock_guard guard(impl_->mutex);
        if (impl_->stopping) return;
        impl_->stopping = true; impl_->wake = {}; impl_->sampleCanceled->store(true);
        for (const auto& [id, work] : impl_->active) { (void)id; work->cancel.canceled->store(true); }
        impl_->sampler.request_stop();
        for (auto& executor : impl_->executors) executor.request_stop();
        impl_->changed.notify_all();
    }
    if (impl_->sampler.joinable()) impl_->sampler.join();
    for (auto& executor : impl_->executors) if (executor.joinable()) executor.join();
    for (const auto* source : {"audio", "brightness", "wifi", "bluetooth", "power", "media"}) impl_->backend->Release(source);
    impl_->pending.clear(); impl_->active.clear();
}
}
