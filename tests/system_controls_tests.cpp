#include "system_controls.h"
#include "system_control_feedback.h"
#include "system_control_bluetooth_sampling.h"
#include "system_control_wifi_presentation.h"
#include "system_control_wifi_sampling.h"
#include "system_control_audio_presentation.h"
#include "system_control_brightness_identity.h"
#include "system_control_windows.h"
#include "widget_system_control_tasks.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace snowdesktop::system_control;
void Require(bool condition, const char* text)
{ if (!condition) { std::cerr << "FAIL: " << text << '\n'; std::exit(1); } }
struct FakeBackend final : Backend
{
    std::mutex mutex;
    std::condition_variable changed;
    unsigned samples = 0, released = 0, executed = 0;
    bool blockSample = false, sampleEntered = false, wifiEntered = false;
    std::string lastVolume;
    std::string planReadback, taskError = "deviceGone";
    bool planAvailable = true;
    std::map<std::string, Snapshot> Sample(std::string_view source, const Cancellation& cancel) override
    {
        std::unique_lock guard(mutex); ++samples; sampleEntered = true; changed.notify_all();
        while (blockSample && !cancel.Stop()) changed.wait_for(guard, 10ms);
        if (source == "power")
        {
            auto value = json::Object(); value.object["activePlanId"] = json::Text(planReadback);
            return {{"system.power.plans", {planAvailable, std::move(value), {}, 0, 0}}};
        }
        if (source != "audio") return {};
        auto value = json::Object(); value.object["volume"] = json::Number(0.25);
        return {{"audio.output.volume", {true, value, {}, 0, 0}}, {"audio.devices", {true, json::Object(), {}, 0, 0}}};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        std::unique_lock guard(mutex); ++executed;
        if (request.name == "network.wifi.scan")
        {
            wifiEntered = true; changed.notify_all();
            while (!cancel.Stop()) changed.wait_for(guard, 10ms);
            return cancel.Failure();
        }
        if (request.name == "audio.output.setVolume") lastVolume = request.arguments.at("volume");
        changed.notify_all(); return {false, taskError, 1167};
    }
    void Release(std::string_view) override { std::lock_guard guard(mutex); ++released; changed.notify_all(); }
    template<class Predicate> void Wait(Predicate predicate, const char* message)
    { std::unique_lock guard(mutex); Require(changed.wait_for(guard, 3s, predicate), message); }
};
void SharedSourceAndRelease()
{
    auto backend = std::make_shared<FakeBackend>(); backend->blockSample = true;
    Service service(backend);
    Require(service.Subscribe("widget", "audio.output.volume", 60s), "first consumer subscribes");
    backend->Wait([&] { return backend->sampleEntered; }, "production sampler enters backend");
    Require(service.Subscribe("native", "audio.devices", 60s), "native consumer subscribes to the same physical source");
    { std::lock_guard guard(backend->mutex); backend->blockSample = false; backend->changed.notify_all(); }
    std::mutex mutex; std::condition_variable changed; bool published = false;
    service.SetWake([&] { std::lock_guard guard(mutex); published = true; changed.notify_all(); });
    { std::unique_lock guard(mutex); Require(changed.wait_for(guard, 3s, [&] { return published || service.Current("audio.devices").has_value(); }), "sample published"); }
    Require(service.Current("audio.output.volume")->value.Find("volume")->number == 0.25, "actual backend state reaches consumers");
    service.RemoveConsumer("native");
    service.RemoveConsumer("widget");
    backend->Wait([&] { return backend->released > 0; }, "last demand releases device resources");
    { std::lock_guard guard(backend->mutex); Require(backend->samples == 1, "adding a related topic must not resample the physical source"); }
    service.SetWake({}); service.Shutdown();
}
void ControlQueueCancellationAndReadback()
{
    auto backend = std::make_shared<FakeBackend>(); Service service(backend);
    Request scan; scan.name = "network.wifi.scan"; scan.arguments["interfaceId"] = "adapter";
    const auto scanId = service.Start("widget", std::move(scan)); Require(scanId != 0, "explicit scan starts");
    backend->Wait([&] { return backend->wifiEntered; }, "slow wireless control starts");
    std::mutex mutex; std::condition_variable changed; unsigned wakes = 0;
    service.SetWake([&] { std::lock_guard guard(mutex); ++wakes; changed.notify_all(); });
    Request volume; volume.name = "audio.output.setVolume"; volume.arguments["volume"] = "0.8";
    const auto volumeId = service.Start("native", std::move(volume));
    std::vector<Completion> completed;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (completed.empty() && std::chrono::steady_clock::now() < deadline)
    {
        completed = service.DrainCompletions("native"); if (!completed.empty()) break;
        std::unique_lock guard(mutex); changed.wait_until(guard, deadline, [&] { return wakes != 0; }); wakes = 0;
    }
    Require(completed.size() == 1 && completed[0].id == volumeId && !completed[0].ok && completed[0].error == "deviceGone",
        "slow WLAN must not block audio and failed device actions must not report success");
    Require(service.Current("audio.output.volume")->value.Find("volume")->number == 0.25, "failed controls reread actual state");
    service.RemoveConsumer("widget");
    service.SetWake({}); service.Shutdown();
    Require(service.DrainCompletions("widget").empty(), "destroyed components must not receive late task results");
}
// Deliberately does not serialize backend calls: the production service must
// keep Release exclusive even when an OS call is still returning after cancel.
struct LifecycleBackend final : Backend
{
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string,unsigned> samples, executions, releases, releaseEntries, users;
    std::set<std::string> resources;
    bool holdExecute = false, holdReadback = false, holdRelease = false;
    std::string slowSource;
    bool holdFirstSample = false;
    bool publishReadback = false;
    std::shared_ptr<std::atomic_bool> executionCancellation;
    void Enter(const std::string& source)
    {
        Require(users[source] == 0, "sampling, execution and release must not overlap for one physical source");
        ++users[source];
    }
    template<class Predicate> void Await(std::unique_lock<std::mutex>& guard, Predicate predicate, const char* message)
    { Require(changed.wait_for(guard, 3s, predicate), message); }
    template<class Predicate> void Wait(Predicate predicate, const char* message)
    { std::unique_lock guard(mutex); Await(guard, predicate, message); }
    std::map<std::string, Snapshot> Sample(std::string_view name, const Cancellation&) override
    {
        const std::string source(name); std::unique_lock guard(mutex); Enter(source);
        resources.insert(source); ++samples[source]; changed.notify_all();
        if (source == slowSource)
        {
            if (samples[source] == 1)
                Await(guard, [&] { return !holdFirstSample; }, "slow sample gate was not released");
            else Require(executions[source] != 0, "an overdue sample cannot run again before an already-ready control");
        }
        if (source == "brightness" && executions[source])
            Await(guard, [&] { return !holdReadback; }, "readback gate was not released");
        --users[source]; changed.notify_all();
        if (publishReadback && source == "brightness")
            return {{"system.display.brightness", {true, json::Object(), {}, 0, 0}}};
        return {};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        const std::string source(Source(request.name)); std::unique_lock guard(mutex); Enter(source);
        resources.insert(source); ++executions[source]; changed.notify_all();
        if (source == "brightness")
        {
            executionCancellation = cancel.canceled;
            Await(guard, [&] { return !holdExecute; }, "execution gate was not released");
        }
        --users[source]; changed.notify_all(); return {false, "deviceGone", 1167};
    }
    void Release(std::string_view name) override
    {
        const std::string source(name); std::unique_lock guard(mutex); Enter(source);
        ++releaseEntries[source]; changed.notify_all();
        if (source == "brightness") Await(guard, [&] { return !holdRelease; }, "release gate was not released");
        resources.erase(source); ++releases[source]; --users[source]; changed.notify_all();
    }
};
Request BrightnessRequest()
{ Request request; request.name = "system.display.setBrightness"; request.arguments = {{"monitorId", "monitor"}, {"brightness", "60"}}; return request; }
void LastConsumerDuringExecutionAndReadback(bool removeConsumer)
{
    auto backend = std::make_shared<LifecycleBackend>();
    backend->holdExecute = true; backend->holdReadback = !removeConsumer;
    Service service(backend);
    Require(service.Subscribe("panel", "system.display.brightness", 60s), "brightness source subscribes");
    backend->Wait([&] { return backend->samples["brightness"] == 1 && !backend->users["brightness"]; }, "initial brightness sample finishes");
    Require(service.Start("panel", BrightnessRequest()) != 0, "brightness request starts");
    backend->Wait([&] { return backend->executions["brightness"] == 1; }, "brightness execution reaches the gate");
    if (removeConsumer) service.RemoveConsumer("panel");
    else Require(service.Unsubscribe("panel", "system.display.brightness"), "page switch drops brightness demand");
    // Observing a second source proves the sampler processed the changed demand
    // while brightness was held. No timing sleep is used to infer that ordering.
    Require(service.Subscribe("probe", "system.power.plans", 60s), "independent source subscribes");
    backend->Wait([&] { return backend->samples["power"] == 1; }, "sampler processes last-consumer removal");
    {
        std::lock_guard guard(backend->mutex);
        Require(!backend->releases["brightness"], "last consumer cannot release an executing source");
        backend->holdExecute = false; backend->changed.notify_all();
    }
    if (!removeConsumer)
    {
        backend->Wait([&] { return backend->samples["brightness"] == 2; }, "failed request enters actual readback");
        service.Invalidate("power");
        backend->Wait([&] { return backend->samples["power"] == 2; }, "sampler runs while readback is held");
        std::lock_guard guard(backend->mutex);
        Require(!backend->releases["brightness"], "last consumer cannot release during readback");
        backend->holdReadback = false; backend->changed.notify_all();
    }
    backend->Wait([&] { return backend->releases["brightness"] == 1 && !backend->resources.contains("brightness"); },
        "the final executor must release resources recreated by readback without a remaining subscriber");
    if (removeConsumer)
    {
        std::lock_guard guard(backend->mutex);
        Require(backend->samples["brightness"] == 1, "removed consumers do not start a new readback");
    }
    service.RemoveConsumer("probe"); service.Shutdown();
}
void NewSubscriberDuringRelease()
{
    auto backend = std::make_shared<LifecycleBackend>(); Service service(backend);
    Require(service.Subscribe("old", "system.display.brightness", 60s), "old brightness consumer subscribes");
    backend->Wait([&] { return backend->samples["brightness"] == 1 && !backend->users["brightness"]; }, "initial source sample finishes");
    { std::lock_guard guard(backend->mutex); backend->holdRelease = true; }
    service.RemoveConsumer("old");
    backend->Wait([&] { return backend->releaseEntries["brightness"] == 1; }, "last consumer enters resource release");
    Require(service.Subscribe("new", "system.display.brightness", 60s), "new consumer subscribes during release");
    Require(service.Start("new", BrightnessRequest()) != 0, "new action queues during release");
    Request audio; audio.name = "audio.output.setMute"; audio.arguments["muted"] = "0";
    Require(service.Start("other", std::move(audio)) != 0, "different source starts while brightness releases");
    backend->Wait([&] { return backend->executions["audio"] == 1; }, "source release does not hold the service lock or block another device");
    {
        std::lock_guard guard(backend->mutex);
        Require(!backend->executions["brightness"] && backend->samples["brightness"] == 1,
            "a new subscriber cannot use resources while their release is in progress");
        backend->holdRelease = false; backend->changed.notify_all();
    }
    backend->Wait([&] { return backend->executions["brightness"] == 1 && backend->samples["brightness"] >= 2 && !backend->users["brightness"]; },
        "new subscriber proceeds after release and reads actual state");
    {
        std::lock_guard guard(backend->mutex);
        Require(backend->releases["brightness"] == 1 && backend->resources.contains("brightness"),
            "the new consumer keeps its resources after readback");
    }
    service.RemoveConsumer("new");
    backend->Wait([&] { return backend->releases["brightness"] == 2 && !backend->resources.contains("brightness"); }, "replacement consumer also releases its resources");
    service.Shutdown();
}
void DirectTaskWithoutSubscriptionReleases()
{
    auto backend = std::make_shared<LifecycleBackend>(); Service service(backend);
    Request request; request.name = "audio.output.setMute"; request.arguments["muted"] = "1";
    Require(service.Start("direct", std::move(request)) != 0, "unsubscribed control request starts");
    backend->Wait([&] { return backend->releases["audio"] == 1 && !backend->resources.contains("audio"); },
        "a direct task without any sampling subscription still releases its backend");
    { std::lock_guard guard(backend->mutex); Require(backend->samples["audio"] == 1, "unsubscribed failed requests retain one actual readback"); }
    service.Shutdown();
}
void SlowSampleYieldsToReadyControl()
{
    auto backend = std::make_shared<LifecycleBackend>();
    backend->slowSource = "audio"; backend->holdFirstSample = true;
    Service service(backend);
    Require(service.Subscribe("panel", "audio.output.volume", 10ms), "fast sampling source subscribes");
    backend->Wait([&] { return backend->samples["audio"] == 1; }, "slow sample enters its controlled gate");
    Request request; request.name = "audio.output.setMute"; request.arguments["muted"] = "1";
    Require(service.Start("panel", std::move(request)) != 0, "ready control queues behind the held sample");
    // Force the scheduled deadline to be overdue while Sample owns the source.
    // This is the scheduling state of a sample slower than its interval, without
    // guessing scheduler progress from a sleep or depending on clock granularity.
    service.Invalidate("audio");
    { std::lock_guard guard(backend->mutex); backend->holdFirstSample = false; backend->changed.notify_all(); }
    backend->Wait([&] { return backend->executions["audio"] == 1; }, "overdue sampling yields to the ready control");
    service.RemoveConsumer("panel"); service.Shutdown();
}
struct AsyncProbe
{
    std::mutex mutex; std::condition_variable changed;
    winrt::Windows::Foundation::AsyncStatus status = winrt::Windows::Foundation::AsyncStatus::Started;
    unsigned polls = 0, cancels = 0, results = 0;
};
struct StalledAsyncOperation
{
    std::shared_ptr<AsyncProbe> probe;
    auto Status() const
    { std::lock_guard guard(probe->mutex); ++probe->polls; probe->changed.notify_all(); return probe->status; }
    void Cancel() const
    { std::lock_guard guard(probe->mutex); ++probe->cancels; } // Deliberately never completes.
    int GetResults() const
    { std::lock_guard guard(probe->mutex); ++probe->results; return 17; }
};
void CancellableWinRtSamplingWait()
{
    auto probe = std::make_shared<AsyncProbe>();
    Cancellation budget{std::make_shared<std::atomic_bool>(false), std::chrono::steady_clock::now() + 3s};
    bool canceled = false, finished = false;
    std::jthread worker([&](std::stop_token token) {
        std::stop_callback stopped(token, [flag = budget.canceled] { flag->store(true); });
        try { (void)snowdesktop::system_control::windows::Await(StalledAsyncOperation{probe}, budget); }
        catch (const winrt::hresult_canceled&) { canceled = true; }
        { std::lock_guard guard(probe->mutex); finished = true; probe->changed.notify_all(); }
    });
    { std::unique_lock guard(probe->mutex); Require(probe->changed.wait_for(guard, 3s, [&] { return probe->polls != 0; }), "WinRT waiter enters a stalled operation"); }
    worker.request_stop();
    { std::unique_lock guard(probe->mutex); Require(probe->changed.wait_for(guard, 3s, [&] { return finished; }), "stopping the worker must finish a stalled WinRT wait"); }
    worker.join();
    Require(canceled && probe->cancels == 1 && !probe->results, "worker stop cancels without waiting for the WinRT operation to acknowledge cancellation");
    probe = std::make_shared<AsyncProbe>(); probe->status = winrt::Windows::Foundation::AsyncStatus::Completed;
    budget = {std::make_shared<std::atomic_bool>(false), std::chrono::steady_clock::now() + 3s};
    Require(snowdesktop::system_control::windows::Await(StalledAsyncOperation{probe}, budget) == 17 && probe->results == 1,
        "completed WinRT operations return their actual result");
    // Later operations receive the same deadline, rather than restarting a
    // per-operation timeout for every session or thumbnail read in the batch.
    probe = std::make_shared<AsyncProbe>(); budget.deadline = std::chrono::steady_clock::now() - 1ms;
    canceled = false;
    try { (void)snowdesktop::system_control::windows::Await(StalledAsyncOperation{probe}, budget); }
    catch (const winrt::hresult_canceled&) { canceled = true; }
    Require(canceled && probe->cancels == 1 && !probe->results && budget.Failure().error == "timeout",
        "the shared sampling deadline cancels a later operation without fabricating results");
}
void HostOnlyCredentialsAndConfirmation()
{
    auto backend = std::make_shared<FakeBackend>(); Service service(backend);
    Request connect; connect.name = "network.wifi.connect"; connect.arguments = {{"interfaceId", "adapter"}, {"ssid", "test"}, {"password", "secret"}};
    Require(service.Start("widget", std::move(connect)) == 0, "password is not a task argument");
    for (const auto* task : {"system.power.shutdown", "system.power.restart", "system.power.sleep"})
    {
        Request request; request.name = task;
        Require(service.Start("widget", std::move(request)) == 0, "power task cannot bypass host confirmation");
    }
    Request volume; volume.name = "audio.output.setVolume"; volume.arguments["volume"] = "2";
    Require(ValidateRequest(volume), "existing finite out-of-range volume keeps clamp compatibility");
    volume.arguments["volume"] = "nan"; Require(!ValidateRequest(volume), "invalid numeric values cannot reach device APIs");
    Secret first(L"private"); Secret moved(std::move(first));
    Require(moved.View() == L"private", "host secret can be transferred without serialization"); moved.Clear();
    Require(moved.View().empty(), "host secret clears after completion");
}
void WidgetControlArgumentsStayWithinTheirContract()
{
    using namespace snowdesktop::widget_runtime;
    using Args = std::unordered_map<std::string, std::string>;
    struct Case { const char* name; Args arguments; };
    const Case cases[]{
        {"audio.output.selectDevice", {{"endpointId", "output"}}},
        {"audio.input.selectDevice", {{"endpointId", "input"}}},
        {"audio.input.setVolume", {{"volume", "0.25"}}},
        {"audio.input.setMute", {{"muted", "1"}}},
        {"system.display.setBrightness", {{"monitorId", "display"}, {"brightness", "60"}}},
        {"network.wifi.setRadio", {{"interfaceId", "adapter"}, {"enabled", "0"}}},
        {"network.wifi.scan", {{"interfaceId", "adapter"}}},
        {"network.wifi.connect", {{"interfaceId", "adapter"}, {"networkId", "network"}}},
        {"network.wifi.disconnect", {{"interfaceId", "adapter"}}},
        {"network.wifi.forget", {{"interfaceId", "adapter"}, {"profileName", "saved"}}},
        {"bluetooth.setRadio", {{"radioId", "radio"}, {"enabled", "1"}}},
        {"bluetooth.connect", {{"deviceId", "device"}}},
        {"bluetooth.disconnect", {{"deviceId", "device"}}},
        {"system.power.setPlan", {{"planId", "plan"}}},
        {"system.power.setMode", {{"mode", "balanced"}}},
        {"system.power.lock", {}}, {"system.power.sleep", {}},
        {"system.power.restart", {}}, {"system.power.shutdown", {}}
    };
    for (const auto& item : cases)
    {
        Request request; request.hostConfirmed = true; request.password = Secret(L"stale-host-secret");
        Require(MakeSystemControlRequest(item.name, item.arguments, request) && request.name == item.name &&
            !request.hostConfirmed && request.password.View().empty(),
            "the 19 widget controls convert only public parameters and never inherit host authorization or secrets");
        unsigned liveCalls = 0;
        const auto live = [&](Request& dispatched) {
            ++liveCalls;
            Require(&dispatched == &request, "live dispatch keeps the validated request and its host-only state");
            return Result{false, "fixtureRejected", 123};
        };
        const auto preview = DispatchSystemControlTask(request, true, live);
        Require(preview.ok && preview.error.empty() && preview.platformCode == 0 && liveCalls == 0,
            "preview accepts each valid widget control without opening confirmation UI or entering the live executor");
        const auto executed = DispatchSystemControlTask(request, false, live);
        Require(!executed.ok && executed.error == "fixtureRejected" && executed.platformCode == 123 && liveCalls == 1,
            "live control dispatch calls its executor exactly once and preserves its failure unchanged");
        liveCalls = 0;
        for (const auto* forbidden : {"password", "hostConfirmed", "unknown"})
        {
            request.arguments[forbidden] = "1";
            const auto rejected = DispatchSystemControlTask(request, true, live);
            Require(!rejected.ok && rejected.error == "invalidArguments" && liveCalls == 0,
                "preview still rejects forbidden parameters before any live confirmation or system effect");
            request.arguments.erase(forbidden);
            auto supplied = item.arguments; supplied[forbidden] = "1";
            Request invalid;
            Require(!MakeSystemControlRequest(item.name, supplied, invalid),
                "a widget cannot smuggle password, confirmation or unknown arguments into any control");
        }
    }
    Request request;
    Require(!IsSystemControlTask("audio.output.setVolume") && !IsSystemControlTask("audio.output.setMute") &&
        !MakeSystemControlRequest("audio.output.setVolume", {{"volume", "0.5"}, {"endpointId", "other"}}, request) &&
        !MakeSystemControlRequest("audio.output.setMute", {{"muted", "1"}}, request),
        "existing output-volume tasks retain their old executor and cannot acquire device-selection power through the new bridge");
    Require(!MakeSystemControlRequest("audio.input.setVolume", {{"volume", "0.5"}, {"endpointId", "other"}}, request) &&
        !MakeSystemControlRequest("network.wifi.scan", {{"interfaceId", "adapter"}, {"deviceId", "other"}}, request),
        "arguments known to another action are still forbidden for this action");
    for (const auto* invalid : {"nan", "inf", "0.5suffix", "", " 0.5"})
        Require(!MakeSystemControlRequest("audio.input.setVolume", {{"volume", invalid}}, request),
            "non-finite, partial and empty control numbers cannot reach a device executor");
    Require(MakeSystemControlRequest("audio.input.setVolume", {{"volume", "2"}}, request) &&
        std::stod(request.arguments.at("volume")) == 1.0,
        "input volume preserves finite clamp behavior without altering brightness bounds");
    Require(!MakeSystemControlRequest("system.display.setBrightness", {{"monitorId", "display"}, {"brightness", "101"}}, request) &&
        !MakeSystemControlRequest("audio.input.setMute", {{"muted", "true"}}, request),
        "brightness range and canonical boolean encoding are enforced after Lua conversion");
    Require(!MakeSystemControlRequest("system.power.setMode", {{"mode", "unknown"}}, request) &&
        !MakeSystemControlRequest("bluetooth.connect", {{"deviceId", std::string(4097, 'x')}}, request) &&
        !MakeSystemControlRequest("bluetooth.connect", {{"deviceId", std::string("a\0b", 3)}}, request),
        "unknown power modes, oversized identities and embedded NULs are rejected");
    for (const auto* target : {"networkId", "profileName", "ssid"})
    {
        Args arguments{{"interfaceId", "adapter"}, {target, "chosen"}};
        if (std::string_view(target) == "ssid") arguments["security"] = "open";
        Require(MakeSystemControlRequest("network.wifi.connect", arguments, request),
            "each of the three exclusive Wi-Fi target forms remains usable");
        for (const auto* second : {"networkId", "profileName", "ssid"}) if (std::string_view(second) != target)
        {
            auto ambiguous = arguments; ambiguous[second] = "another";
            Require(!MakeSystemControlRequest("network.wifi.connect", ambiguous, request),
                "a Wi-Fi control cannot choose between two conflicting target identities");
        }
    }
    Require(!MakeSystemControlRequest("network.wifi.connect", {{"interfaceId", "adapter"}}, request) &&
        !MakeSystemControlRequest("network.wifi.connect", {{"interfaceId", "adapter"}, {"ssid", "chosen"}}, request) &&
        !MakeSystemControlRequest("network.wifi.connect", {{"interfaceId", "adapter"}, {"ssid", std::string(33, 'x')}, {"security", "open"}}, request) &&
        !MakeSystemControlRequest("network.wifi.connect", {{"interfaceId", "adapter"}, {"ssid", "chosen"}, {"security", "unknown"}}, request),
        "explicit SSIDs need supported security and the WLAN byte limit, and targetless connections are invalid");
}
void WidgetControlBridgeOwnsTaskLifetimes()
{
    using Bridge = snowdesktop::widget_runtime::WidgetSystemControlTasks;
    const auto brightness = [](const char* target) { auto request = BrightnessRequest(); request.arguments["monitorId"] = target; return request; };
    for (const bool forget : {false, true})
    {
        auto backend = std::make_shared<LifecycleBackend>(); backend->holdExecute = true;
        auto service = std::make_shared<Service>(backend);
        std::mutex mutex; std::condition_variable changed; unsigned wakes = 0;
        service->SetWake([&] { std::lock_guard guard(mutex); ++wakes; changed.notify_all(); });
        Bridge bridge(service);
        Require(bridge.Start(1001, 11, brightness("first")), "widget bridge accepts a broker ID different from the service ID");
        backend->Wait([&] { return backend->executions["brightness"] == 1; }, "bridge execution reaches a controllable gate");
        Require(bridge.Start(1002, 11, brightness("second")) && !bridge.Start(1002, 22, brightness("duplicate")) &&
            !bridge.Cancel(9999), "the same owner may have independent tasks, while duplicate and unknown broker IDs cannot affect them");
        Request native; native.name = "system.power.setPlan"; native.arguments["planId"] = "native";
        const auto nativeId = service->Start("native-panel-fixture", std::move(native));
        Require(nativeId != 0, "native consumer shares the bridge service");
        if (forget)
        {
            Require(bridge.Start(2001, 22, brightness("other-owner")), "another component owns its own queued task");
            bridge.Forget(11);
            Require(bridge.ActiveCount() == 1 && !bridge.Cancel(1001) && !bridge.Cancel(1002),
                "forget cancels exactly the disposed owner's tasks");
            Require(bridge.Start(1003, 11, brightness("new-generation")),
                "a later owner generation can start while a canceled OS call is still returning");
        }
        else
        {
            Require(bridge.Cancel(1001) && bridge.ActiveCount() == 1,
                "cancel translates a broker ID without dropping a sibling task for the same owner");
        }
        {
            std::lock_guard guard(backend->mutex);
            Require(backend->executionCancellation && backend->executionCancellation->load(),
                "broker cancellation must reach the executing service task, not an unrelated numeric ID");
            backend->holdExecute = false; backend->changed.notify_all();
        }
        std::vector<Bridge::Completion> completed;
        const std::size_t expected = forget ? 4u : 2u;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (completed.size() < expected && std::chrono::steady_clock::now() < deadline)
        {
            auto batch = bridge.Drain(); completed.insert(completed.end(), batch.begin(), batch.end());
            if (completed.size() == expected) break;
            std::unique_lock guard(mutex); changed.wait_until(guard, deadline, [&] { return wakes != 0; }); wakes = 0;
        }
        Require(completed.size() == expected && bridge.ActiveCount() == 0,
            "canceled and surviving widget tasks each finish exactly once under their broker IDs");
        std::set<std::uint64_t> identities;
        for (const auto& completion : completed)
        {
            const bool canceled = completion.id == 1001 || (forget && completion.id == 1002);
            Require(identities.insert(completion.id).second && !completion.ok &&
                completion.error == (canceled ? "canceled" : "deviceGone"),
                "a canceled task's late backend failure cannot replace its cancellation or another owner's result");
        }
        const std::set<std::uint64_t> wanted = forget ? std::set<std::uint64_t>{1001, 1002, 1003, 2001} : std::set<std::uint64_t>{1001, 1002};
        Require(identities == wanted && bridge.Drain().empty(), "service-generated IDs and duplicate late results never leak to widgets");
        backend->Wait([&] { return backend->releases["power"] > 0; }, "native task completes independently of widget cancellation");
        const auto nativeResults = service->DrainCompletions("native-panel-fixture");
        Require(nativeResults.size() == 1 && nativeResults.front().id == nativeId && nativeResults.front().error == "deviceGone",
            "draining widget results cannot steal another service consumer's completion");
        service->SetWake({}); service->Shutdown();
    }
    auto backend = std::make_shared<LifecycleBackend>(); backend->holdExecute = true;
    auto service = std::make_shared<Service>(backend);
    {
        Bridge bridge(service); Require(bridge.Start(3001, 33, brightness("destroyed")), "bridge destruction fixture starts");
        backend->Wait([&] { return backend->executions["brightness"] == 1; }, "destruction fixture reaches execution");
    }
    {
        std::lock_guard guard(backend->mutex);
        Require(backend->executionCancellation && backend->executionCancellation->load(),
            "destroying the bridge cancels its in-flight OS work before losing the ID mapping");
        backend->holdExecute = false; backend->changed.notify_all();
    }
    service->Shutdown();
}
void ReusedConsumerCannotReceiveDetachedWork()
{
    auto backend = std::make_shared<LifecycleBackend>(); backend->holdExecute = true;
    Service service(backend); std::mutex mutex; std::condition_variable changed; unsigned wakes = 0;
    service.SetWake([&] { std::lock_guard guard(mutex); ++wakes; changed.notify_all(); });
    const auto retired = service.Start("reused-consumer", BrightnessRequest());
    backend->Wait([&] { return backend->executions["brightness"] == 1; }, "retired consumer is still inside its OS call");
    service.RemoveConsumer("reused-consumer");
    auto request = BrightnessRequest(); request.arguments["monitorId"] = "replacement-monitor";
    const auto current = service.Start("reused-consumer", std::move(request));
    Require(retired && current && retired != current, "same consumer name starts independent work before its retired call returns");
    { std::lock_guard guard(backend->mutex); backend->holdExecute = false; backend->changed.notify_all(); }
    std::vector<Completion> completed;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (completed.empty() && std::chrono::steady_clock::now() < deadline)
    {
        completed = service.DrainCompletions("reused-consumer"); if (!completed.empty()) break;
        std::unique_lock guard(mutex); changed.wait_until(guard, deadline, [&] { return wakes != 0; }); wakes = 0;
    }
    Require(completed.size() == 1 && completed.front().id == current && completed.front().error == "deviceGone",
        "a detached old call cannot reappear in the new consumer's completion queue");
    service.SetWake({}); service.Shutdown();
    Require(service.DrainCompletions("reused-consumer").empty(), "no retired completion leaks after shutdown joins the old worker");
}
void BrightnessSettlingAndStaleFeedback()
{
    Cancellation cancel{std::make_shared<std::atomic_bool>(false), std::chrono::steady_clock::now() + 3s};
    unsigned reads = 0, pauses = 0;
    auto result = ConfirmBrightness(70, 2, cancel, [&]() -> BrightnessReadback {
        return {++reads < 3 ? 30. : 70., {}};
    }, [&] { ++pauses; });
    Require(result.ok && reads == 3 && pauses == 2, "delayed brightness readback must settle before reporting success");
    reads = pauses = 0;
    result = ConfirmBrightness(70, 2, cancel, [&]() -> BrightnessReadback {
        ++reads; return {30., {}};
    }, [&] { ++pauses; });
    Require(!result.ok && result.error == "stateMismatch" && reads == 20 && pauses == 19,
        "accepted writes without matching readback must fail within the bounded attempt count");
    result = ConfirmBrightness(70, 2, cancel, [&]() -> BrightnessReadback { return {30., {}}; },
        [&] { cancel.canceled->store(true); });
    Require(!result.ok && result.error == "canceled", "replacement slider requests cancel brightness settling");
    cancel.canceled->store(false);
    result = ConfirmBrightness(70, 2, cancel, []() -> BrightnessReadback { return {{}, {false, "deviceGone", 1167}}; },
        [] { Require(false, "device removal must not be retried"); });
    Require(result.error == "deviceGone", "device removal remains a distinct failed result");
    ControlFeedback feedback;
    Request a; a.name = "system.display.setBrightness"; a.arguments = {{"monitorId", "a"}, {"brightness", "30"}};
    const auto key = ControlFeedback::Key(a); feedback.Track(key, 1);
    a.arguments["brightness"] = "70"; feedback.Track(ControlFeedback::Key(a), 2);
    a.arguments["monitorId"] = "b"; feedback.Track(ControlFeedback::Key(a), 3);
    Require(!feedback.Take(1) && feedback.Take(3) && feedback.Take(2) && !feedback.Take(2),
        "queued old failures cannot override new controls, and different displays retain independent results");
    feedback.Track(key, 4); feedback.Clear();
    Require(!feedback.Take(4), "closed panels discard late control feedback");
}
void AudioPresentationKeepsRealEndpoints()
{
    Require(AudioEndpointName("  Speakers  ", "Adapter", "Device") == "Speakers", "the actual endpoint name takes precedence");
    Require(AudioEndpointName(" \t", " Audio Adapter ", " Speakers ") == "Speakers (Audio Adapter)",
        "unnamed active endpoints fall back to real adapter and device properties");
    Require(AudioEndpointName({}, {}, " Microphone ") == "Microphone" &&
        AudioEndpointName({}, "Virtual Audio", {}) == "Virtual Audio", "either reliable fallback name keeps a usable endpoint visible");
    auto value = json::Object(), devices = json::Array();
    const auto device = [](const char* id, const char* name, const char* state, bool available, bool isDefault, const char* direction = "output") {
        auto item = json::Object(); item.object["id"] = json::Text(id); item.object["name"] = json::Text(name);
        item.object["state"] = json::Text(state); item.object["available"] = json::Boolean(available);
        item.object["isDefault"] = json::Boolean(isDefault); item.object["direction"] = json::Text(direction); return item;
    };
    devices.array = {device("old", "Speakers", "unplugged", false, false),
        device("disabled", "Speakers", "disabled", false, false), device("missing", "Speakers", "notPresent", false, false),
        device("live", "Speakers", "active", true, false), device("live", "Speakers", "active", true, true),
        device("other-live", "Speakers", "active", true, false), device("virtual", "Virtual Audio", "active", true, false),
        device("nameless", " \t", "active", true, false), device("wrong-availability", "Device", "active", false, false),
        device("microphone", "Microphone", "active", true, true, "input")};
    value.object["devices"] = devices;
    const auto output = AudioPresentationDevices(value, "output"), input = AudioPresentationDevices(value, "input");
    Require(output.size() == 3 && json::String(output.front(), "id") == "live" && json::Flag(output.front(), "isDefault"),
        "only active usable named endpoints are presented and duplicate identities merge their default state");
    Require(json::String(output[1], "id") == "other-live" && json::String(output[2], "id") == "virtual" &&
        input.size() == 1 && json::String(input.front(), "id") == "microphone", "same-name real and virtual endpoints remain independently selectable in their own direction");
    Require(value.Find("devices")->array.size() == 10, "presentation filtering preserves the public inactive-endpoint data contract");
}
void WifiOffStillHasManageableInterface()
{
    auto value = json::Object(); value.object["id"] = json::Text("adapter"); value.object["connected"] = json::Boolean(true);
    unsigned networkReads = 0;
    const auto failNetworks = [&]() -> WifiNetworksReadback { ++networkReads; return {json::Array(), "accessDenied", false}; };
    SampleWifiInterface(value, [] { return WifiRadioReadback{true, false, true, {}}; }, failNetworks);
    Require(json::Flag(value, "available") && !json::Flag(value, "enabled") && json::Flag(value, "hardwareEnabled") &&
        !json::Flag(value, "connected") && !value.Find("error") && networkReads == 0 && json::String(value, "id") == "adapter",
        "software radio off must keep a usable switch and identity without attempting unavailable network enumeration");
    SampleWifiInterface(value, [] { return WifiRadioReadback{true, true, true, {}}; }, failNetworks);
    Require(json::Flag(value, "available") && json::Flag(value, "enabled") && json::String(value, "error") == "accessDenied" && networkReads == 1,
        "network-list denial remains visible without disabling the independently readable radio");
    SampleWifiInterface(value, [] { return WifiRadioReadback{false, false, false, "accessDenied"}; }, failNetworks);
    Require(!json::Flag(value, "available") && !value.Find("enabled") && json::String(value, "error") == "accessDenied" && networkReads == 1,
        "actual radio access failure must not reuse stale switch state or become a successful off state");
    unsigned radioReads = 0;
    SampleWifiInterface(value, [&] { return WifiRadioReadback{true, ++radioReads == 1, true, {}}; },
        [] { return WifiNetworksReadback{json::Array(), "unavailable", true}; });
    Require(radioReads == 2 && json::Flag(value, "available") && !json::Flag(value, "enabled") && !value.Find("error"),
        "power-state-invalid during network enumeration rereads the changed radio instead of declaring missing hardware");
    SampleWifiInterface(value, [] { return WifiRadioReadback{false, false, false, "actionUnsupported"}; }, failNetworks);
    Require(!json::Flag(value, "available") && json::String(value, "error") == "actionUnsupported" && networkReads == 1,
        "a radio with no controllable PHY stays unavailable");
}
void ControlReadbackMustReallySettle()
{
    Cancellation cancel{std::make_shared<std::atomic_bool>(false), std::chrono::steady_clock::now() + 3s};
    unsigned reads = 0, pauses = 0;
    auto result = ConfirmControlValue(0.8, 0.02, cancel, [&]() -> ControlReadback {
        ++reads; if (reads == 1) return {{}, {false, "unavailable", 123}};
        return {reads == 2 ? 0.4 : 0.8, {}};
    }, [&] { ++pauses; });
    Require(result.ok && reads == 3 && pauses == 2, "a temporary read failure and delayed audio value settle before success is reported");
    reads = pauses = 0;
    result = ConfirmControlValue(1, 0, cancel, [&]() -> ControlReadback {
        ++reads; return {{}, {false, "unavailable", 123}};
    }, [&] { ++pauses; });
    Require(!result.ok && result.error == "unavailable" && result.platformCode == 123 && reads == 20 && pauses == 19,
        "persistently failed readback retains its real error instead of treating an accepted write as success");
    result = ConfirmControlValue(1, 0, cancel, []() -> ControlReadback { return {{}, {false, "accessDenied", 5}}; },
        [] { Require(false, "access denial must not be retried"); });
    Require(!result.ok && result.error == "accessDenied", "readback access denial remains an immediate real failure");
}
void NewSliderCancelsExecutingOldTarget()
{
    auto backend = std::make_shared<LifecycleBackend>(); backend->holdExecute = true;
    Service service(backend); std::mutex mutex; std::condition_variable changed; unsigned wakes = 0;
    service.SetWake([&] { std::lock_guard guard(mutex); ++wakes; changed.notify_all(); });
    const auto first = service.Start("panel", BrightnessRequest());
    backend->Wait([&] { return backend->executions["brightness"] == 1; }, "old slider request enters controlled execution gate");
    auto request = BrightnessRequest(); request.arguments["brightness"] = "80";
    const auto key = ControlFeedback::Key(request);
    const auto second = service.Start("panel", std::move(request));
    Require(first && second && first != second, "replacement slider receives a distinct identity");
    {
        std::lock_guard guard(backend->mutex);
        Require(backend->executionCancellation && backend->executionCancellation->load(),
            "replacement must cancel the already executing old slider, not only pending requests");
        backend->holdExecute = false; backend->changed.notify_all();
    }
    std::vector<Completion> completions;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (completions.size() < 2 && std::chrono::steady_clock::now() < deadline)
    {
        auto batch = service.DrainCompletions("panel"); completions.insert(completions.end(), batch.begin(), batch.end());
        if (completions.size() == 2) break;
        std::unique_lock guard(mutex); changed.wait_until(guard, deadline, [&] { return wakes != 0; }); wakes = 0;
    }
    Require(completions.size() == 2, "both replaced and current slider tasks finish");
    ControlFeedback feedback; feedback.Track(key, first); feedback.Track(key, second);
    unsigned acceptedFeedback = 0;
    for (const auto& completion : completions)
    {
        if (completion.id == first) Require(!completion.ok && completion.error == "canceled" && !feedback.Take(completion.id),
            "obsolete slider execution cannot flash its previous failure");
        else if (feedback.Take(completion.id))
        { ++acceptedFeedback; Require(completion.id == second && !completion.ok && completion.error == "deviceGone", "newest genuine device failure remains reportable"); }
    }
    Require(acceptedFeedback == 1, "only the newest slider result supplies UI feedback");
    service.SetWake({}); service.Shutdown();
}
void CancellationDuringReadbackDiscardsOldState()
{
    auto backend = std::make_shared<LifecycleBackend>(); backend->holdReadback = true; backend->publishReadback = true;
    Service service(backend);
    const auto id = service.Start("panel", BrightnessRequest());
    backend->Wait([&] { return backend->samples["brightness"] == 1; }, "old task reaches controlled readback gate");
    Require(service.Cancel(id), "readback can be canceled after the OS action has returned");
    { std::lock_guard guard(backend->mutex); backend->holdReadback = false; backend->changed.notify_all(); }
    backend->Wait([&] { return backend->releases["brightness"] == 1; }, "canceled readback finishes and releases its source");
    const auto completions = service.DrainCompletions("panel");
    Require(completions.size() == 1 && completions.front().id == id && completions.front().error == "canceled" &&
        !service.Current("system.display.brightness"), "late readback of a canceled task neither publishes old state nor leaks the previous failure to UI");
    service.Shutdown();
}
void QueuedCancellationDoesNotWaitForOrReleaseAnotherControl()
{
    auto backend = std::make_shared<LifecycleBackend>(); backend->holdExecute = true;
    Service service(backend); std::mutex mutex; std::condition_variable changed; unsigned wakes = 0;
    service.SetWake([&] { std::lock_guard guard(mutex); ++wakes; changed.notify_all(); });
    Require(service.Start("busy", BrightnessRequest()) != 0, "source ownership fixture starts its old OS call");
    backend->Wait([&] { return backend->executions["brightness"] == 1; }, "old OS call holds the physical source");
    auto request = BrightnessRequest(); request.arguments["monitorId"] = "queued-monitor";
    const auto queued = service.Start("queued", std::move(request));
    Require(queued && service.Cancel(queued), "cancel a queued control while the same source is occupied");
    std::vector<Completion> completed;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (completed.empty() && std::chrono::steady_clock::now() < deadline)
    {
        completed = service.DrainCompletions("queued"); if (!completed.empty()) break;
        std::unique_lock guard(mutex); changed.wait_until(guard, deadline, [&] { return wakes != 0; }); wakes = 0;
    }
    Require(completed.size() == 1 && completed.front().id == queued && completed.front().error == "canceled",
        "queued cancellation clears pending without waiting for an unrelated in-flight OS call to return");
    request = BrightnessRequest(); request.arguments["monitorId"] = "next-monitor";
    Require(service.Start("next", std::move(request)) != 0, "another device still waits for the old source owner");
    Request plan; plan.name = "system.power.setPlan"; plan.arguments["planId"] = "independent";
    Require(service.Start("power", std::move(plan)) != 0, "an independent source remains executable");
    backend->Wait([&] { return backend->releases["power"] > 0; }, "independent work finishes while the old source stays reserved");
    {
        std::lock_guard guard(backend->mutex);
        Require(backend->executions["brightness"] == 1 && backend->users["brightness"] == 1,
            "finishing a canceled queued task cannot unlock a source owned by another executor");
        backend->holdExecute = false; backend->changed.notify_all();
    }
    backend->Wait([&] { return backend->executions["brightness"] == 2 && backend->releases["brightness"] > 0; },
        "the remaining live task runs once after the original source owner finishes");
    Require(service.DrainCompletions("queued").empty(), "canceled queued work cannot produce a second late completion");
    service.SetWake({}); service.Shutdown();
}
void WirelessNotificationsNeedActualState()
{
    const auto now = std::chrono::steady_clock::now();
    Cancellation cancel{std::make_shared<std::atomic_bool>(false), now + 40s};
    Require(WifiScanCancellation(cancel, now).deadline == now + 4s &&
        WifiScanCancellation({cancel.canceled, now + 1s}, now).deadline == now + 1s,
        "scan notification waits cannot monopolize the source for forty seconds or extend caller deadlines");
    unsigned reads = 0, pauses = 0;
    auto result = ConfirmNotifiedControl(cancel, [&]() -> ControlReadback {
        return {++reads == 3 ? 1. : 0., {}};
    }, []() -> std::optional<Result> { return {}; }, [&] { ++pauses; });
    Require(result.ok && reads == 3 && pauses == 2,
        "a missing WLAN notification does not hide an actually established target connection");
    reads = pauses = 0;
    result = ConfirmNotifiedControl(cancel, [&]() -> ControlReadback { return {++reads == 2 ? 1. : 0., {}}; },
        []() -> std::optional<Result> { return Result{false, "connectionFailed", 123}; }, [&] { ++pauses; });
    Require(result.ok && reads == 2 && pauses == 1,
        "a failed or early notification is reconciled only after the exact target actually becomes connected");
    reads = pauses = 0;
    result = ConfirmNotifiedControl(cancel, [&]() -> ControlReadback { ++reads; return {0., {}}; },
        []() -> std::optional<Result> { return Result{false, "connectionFailed", 123}; }, [&] { ++pauses; });
    Require(!result.ok && result.error == "connectionFailed" && result.platformCode == 123 && reads == 20 && pauses == 19,
        "a genuinely failed connection keeps its reason after the bounded settling interval");
    reads = pauses = 0;
    result = ConfirmNotifiedControl(cancel, [&]() -> ControlReadback { ++reads; return {0., {}}; },
        []() -> std::optional<Result> { return Result{true, {}, 0}; }, [&] { ++pauses; });
    Require(!result.ok && result.error == "stateMismatch" && reads == 20 && pauses == 19,
        "a success notification cannot stand in for connection to the requested target");
    reads = 0;
    result = ConfirmNotifiedControl(cancel, [&]() -> ControlReadback { ++reads; return {1., {}}; },
        []() -> std::optional<Result> { return Result{false, "accessDenied", 5}; }, [] {});
    Require(!result.ok && result.error == "accessDenied" && reads == 0,
        "matching state cannot override a permission rejection");
    cancel.canceled->store(true);
    result = ConfirmNotifiedControl(cancel, [&]() -> ControlReadback { ++reads; return {1., {}}; },
        []() -> std::optional<Result> { return {}; }, [] {});
    Require(!result.ok && result.error == "canceled" && reads == 0,
        "superseded or canceled connections cannot report success from late readback");
    cancel.canceled->store(false); cancel.deadline = now - 1s;
    result = ConfirmNotifiedControl(cancel, [&]() -> ControlReadback { ++reads; return {1., {}}; },
        []() -> std::optional<Result> { return {}; }, [] {});
    Require(!result.ok && result.error == "timeout" && reads == 0,
        "a deadline with no completion evidence stays a timeout instead of an accepted-write success");
}
void FreshReadbackOnlyConfirmsTheRequestedTarget()
{
    for (const auto* error : {"stateMismatch", "accessDenied", "timeout", "deviceGone"})
    {
        auto backend = std::make_shared<FakeBackend>(); backend->planReadback = "chosen-plan"; backend->taskError = error;
        Service service(backend); Request request; request.name = "system.power.setPlan"; request.arguments["planId"] = "chosen-plan";
        const auto id = service.Start("panel", std::move(request));
        backend->Wait([&] { return backend->released > 0; }, "finished plan task releases after its fresh readback");
        const auto completed = service.DrainCompletions("panel");
        Require(completed.size() == 1 && completed.front().id == id, "readback reconciliation preserves task identity");
        if (std::string_view(error) == "stateMismatch") Require(completed.front().ok && completed.front().error.empty(),
            "the new backend sample may prove a previously unsettled plan reached its actual target");
        else Require(!completed.front().ok && completed.front().error == error,
            "matching sampled state cannot erase an actual rejection, timeout or device-removal error");
        service.Shutdown();
    }
    for (const bool available : {true, false})
    {
        auto backend = std::make_shared<FakeBackend>(); backend->planReadback = available ? "different-plan" : "chosen-plan";
        backend->planAvailable = available; backend->taskError = "stateMismatch";
        Service service(backend); Request request; request.name = "system.power.setPlan"; request.arguments["planId"] = "chosen-plan";
        Require(service.Start("panel", std::move(request)) != 0, "readback mismatch scenario starts");
        backend->Wait([&] { return backend->released > 0; }, "unconfirmed plan task finishes");
        const auto completed = service.DrainCompletions("panel");
        Require(completed.size() == 1 && !completed.front().ok && completed.front().error == "stateMismatch",
            "different targets and failed samples cannot turn a mismatch into success");
        service.Shutdown();
    }
    auto monitor = json::Object(), value = json::Object(), monitors = json::Array();
    monitor.object["id"] = json::Text("other-monitor"); monitor.object["available"] = json::Boolean(true);
    monitor.object["brightness"] = json::Number(60); monitors.array.push_back(monitor); value.object["monitors"] = monitors;
    std::map<std::string, Snapshot> snapshots{{"system.display.brightness", {true, value, {}, 0, 0}}};
    auto request = BrightnessRequest();
    Require(!ControlReadbackMatches(request, snapshots), "a matching number on another monitor is not confirmation");
    snapshots.begin()->second.value.object["monitors"].array.front().object["id"] = json::Text("monitor");
    Require(ControlReadbackMatches(request, snapshots), "fresh matching brightness belongs to the explicit monitor identity");
    snapshots.begin()->second.error = "unavailable";
    Require(!ControlReadbackMatches(request, snapshots), "errored snapshots cannot confirm even when their retained payload matches");
    request.name = "audio.output.setVolume"; request.arguments = {{"volume", "0.8"}};
    value = json::Object(); value.object["volume"] = json::Number(0.8); value.object["endpointId"] = json::Text("replacement-endpoint");
    snapshots = {{"audio.output.volume", {true, value, {}, 0, 0}}};
    Require(!ControlReadbackMatches(request, snapshots), "unbound default-volume requests cannot claim another endpoint's matching state");
    request.name = "network.wifi.setRadio"; request.arguments = {{"interfaceId", "adapter"}, {"enabled", "1"}};
    auto adapter = json::Object(), interfaces = json::Array(); value = json::Object();
    adapter.object["id"] = json::Text("adapter"); adapter.object["available"] = json::Boolean(true);
    adapter.object["enabled"] = json::Boolean(true); interfaces.array.push_back(std::move(adapter)); value.object["interfaces"] = std::move(interfaces);
    snapshots = {{"network.wifi", {true, value, {}, 0, 0}}};
    Require(!ControlReadbackMatches(request, snapshots),
        "a radio presentation flag meaning any PHY is on cannot prove that all requested PHY changes succeeded");
}
void BrightnessIdentityAndLegacyTargets()
{
    const auto panel = BrightnessMonitorIdentity(L"DISPLAY\\BOE0D55\\5&fixture&0&UID8448_0", true);
    const auto panelPath = BrightnessMonitorIdentity(L"\\\\?\\display#boe0d55#5&fixture&0&uid8448#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}");
    const auto external = BrightnessMonitorIdentity(L"DISPLAY\\XMI3005\\5&fixture&0&UID8449_0", true);
    const auto secondPanel = BrightnessMonitorIdentity(L"DISPLAY\\BOE0D55\\5&fixture&0&UID8450_0", true);
    Require(!panel.empty() && panel == panelPath && panel != secondPanel,
        "WMI and monitor-interface forms identify the same complete PnP instance, not every panel of the same model");
    Require(BrightnessMonitorIdentity(L"Generic PnP Monitor").empty() &&
        BrightnessMonitorIdentity(L"DISPLAY\\BOE0D55\\").empty(), "names and incomplete paths cannot become physical identities");
    std::vector<BrightnessDisplayTarget> targets{{panel, L"Generic PnP Monitor", true}};
    Require(BrightnessDdcIdentity(targets, 1) == panel, "a sole active target and sole physical handle have an unambiguous mapping");
    Require(BrightnessDdcIdentity(targets, 2).empty(), "one logical display cannot identify several physical handles by array order");
    targets.push_back({secondPanel, L"Generic PnP Monitor", true});
    Require(BrightnessDdcIdentity(targets, 1).empty(), "a cloned GDI view with two real targets must not borrow the first target identity");
    targets[1].active = false;
    Require(BrightnessDdcIdentity(targets, 1) == panel && BrightnessMonitorActive(secondPanel, targets) == false &&
        !BrightnessMonitorActive(external, targets).has_value(), "explicit inactive targets differ from targets whose identity mapping is unknown");
    Require(BrightnessMonitorName(L" NE160QDM-NZL ", L"Generic PnP Monitor", panel) == L"NE160QDM-NZL" &&
        BrightnessMonitorName(L"", L"Generic PnP Monitor", external) == L"Generic PnP Monitor" &&
        BrightnessMonitorName(L"DISPLAY\\BOE0D55\\instance", L"\\\\?\\DISPLAY#bad", panel) == L"BOE0D55",
        "EDID names take precedence and even fallback labels cannot expose instance paths");
    const auto sample = [](std::string id, std::wstring identity, std::string name, bool internal, std::optional<double> level) {
        auto value = json::Object(); value.object["id"] = json::Text(std::move(id));
        value.object["name"] = json::Text(std::move(name)); value.object["kind"] = json::Text(internal ? "internal" : "ddc");
        value.object["available"] = json::Boolean(level.has_value());
        if (level) value.object["brightness"] = json::Number(*level); else value.object["error"] = json::Text("actionUnsupported");
        return BrightnessMonitorSample{std::move(identity), std::move(value)};
    };
    const std::string oldDdc = "ddc:display2:0";
    std::vector<BrightnessMonitorSample> samples{
        sample("wmi:panel", panel, "NE160QDM-NZL", true, 64),
        sample("ddc:display1:0", external, "Mi Monitor", false, 80),
        sample(oldDdc, panel, "Generic PnP Monitor", false, {})};
    auto merged = MergeBrightnessMonitors(samples);
    Require(merged.array.size() == 2 && json::String(merged.array[0], "id") == "wmi:panel" &&
        json::Numeric(merged.array[0], "brightness") == 64 && json::Numeric(merged.array[1], "brightness") == 80 && samples.size() == 3,
        "the internal WMI/DDC alias becomes one accurate row without mutating the endpoint inventory");
    samples.push_back(sample("ddc:display3:0", secondPanel, "Mi Monitor", false, 35));
    samples.push_back(sample("ddc:unknown1:0", {}, "Mi Monitor", false, {}));
    samples.push_back(sample("ddc:unknown2:0", {}, "Mi Monitor", false, 50));
    merged = MergeBrightnessMonitors(samples);
    Require(merged.array.size() == 5 && json::String(merged.array[2], "id") == "ddc:display3:0" &&
        !json::Flag(merged.array[3], "available") && !merged.array[3].Find("brightness"),
        "same-name real displays and unknown mappings remain separate, and unsupported brightness never becomes zero");
    samples[0].value.object["available"] = json::Boolean(false); samples[0].value.object.erase("brightness");
    samples[2].value.object["available"] = json::Boolean(true); samples[2].value.object["brightness"] = json::Number(72);
    samples[2].value.object.erase("error");
    Require(json::String(MergeBrightnessMonitors(samples).array.front(), "id") == oldDdc,
        "an actually available DDC endpoint remains usable when its WMI alias cannot be read");

    struct LegacyBackend final : Backend
    {
        struct Endpoint { std::string id; int physical; };
        std::vector<Endpoint> endpoints{{"ddc:display1:0", 1}, {"ddc:display2:0", 2}};
        std::vector<BrightnessMonitorSample> samples;
        Result outcome;
        int executedPhysical = 0;
        std::mutex mutex; std::condition_variable changed; bool released = false;
        std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation&) override
        {
            auto value = json::Object(); value.object["monitors"] = MergeBrightnessMonitors(samples);
            return {{"system.display.brightness", {true, std::move(value), {}, 0, 0}}};
        }
        Result Execute(const Request& request, const Cancellation&) override
        {
            return WithBrightnessEndpoint(endpoints, request.arguments.at("monitorId"), [&](const Endpoint& endpoint) {
                executedPhysical = endpoint.physical; return outcome;
            });
        }
        void Release(std::string_view) override
        { std::lock_guard guard(mutex); released = true; changed.notify_all(); }
    };
    for (const bool ok : {true, false})
    {
        auto backend = std::make_shared<LegacyBackend>();
        backend->samples = {sample("wmi:panel", panel, "NE160QDM-NZL", true, 64), sample(oldDdc, panel, "Generic PnP Monitor", false, {})};
        backend->outcome = {ok, ok ? "" : "stateMismatch", 0};
        Service service(backend); auto request = BrightnessRequest();
        request.arguments = {{"monitorId", oldDdc}, {"brightness", "64"}};
        const auto id = service.Start("legacy-widget", std::move(request));
        Require(id != 0, "an old brightness endpoint token remains a valid task argument");
        { std::unique_lock guard(backend->mutex); Require(backend->changed.wait_for(guard, 3s, [&] { return backend->released; }), "legacy brightness operation completes and releases"); }
        const auto completed = service.DrainCompletions("legacy-widget");
        Require(backend->executedPhysical == 2 && completed.size() == 1 && completed.front().id == id && completed.front().ok == ok,
            "hidden legacy DDC IDs still execute on their original endpoint and retain the real result through service readback");
        if (!ok) Require(completed.front().error == "stateMismatch", "a WMI alias value cannot manufacture success for a failed DDC operation");
        int calls = 0;
        const auto missing = WithBrightnessEndpoint(backend->endpoints, "ddc:removed:0", [&](const auto&) { ++calls; return Result{true, {}, 0}; });
        Require(!missing.ok && missing.error == "deviceGone" && calls == 0, "a removed token cannot fall through to another physical display");
        service.Shutdown();
    }
}
void BluetoothPowerAndDeviceReadFailures()
{
    Cancellation cancel{std::make_shared<std::atomic_bool>(false), std::chrono::steady_clock::now() + 3s};
    BluetoothCollection radios;
    auto radio = json::Object(); radio.object["id"] = json::Text("radio-a");
    radio.object["available"] = json::Boolean(true); radio.object["enabled"] = json::Boolean(false);
    radios.items.array.push_back(radio);
    unsigned deviceReads = 0;
    const auto peersFail = [&] { ++deviceReads; return BluetoothCollection{json::Array(), "unavailable"}; };
    auto result = SampleBluetooth([&] { return radios; }, peersFail, cancel);
    Require(result.available && result.error.empty() && deviceReads == 0 &&
        json::Flag(result.value.Find("radios")->array.front(), "available") &&
        !json::Flag(result.value.Find("radios")->array.front(), "enabled"),
        "radio off remains available without trying unavailable peer enumeration");
    radios.items.array.front().object["enabled"] = json::Boolean(true);
    result = SampleBluetooth([&] { return radios; }, peersFail, cancel);
    Require(result.available && result.error == "unavailable" && deviceReads == 1 &&
        json::String(result.value.Find("radios")->array.front(), "id") == "radio-a",
        "paired-device failures cannot discard a successfully read radio identity");
    radios.items.array.front().object["available"] = json::Boolean(false);
    result = SampleBluetooth([&] { return radios; }, peersFail, cancel);
    Require(result.available && deviceReads == 1 && !json::Flag(result.value.Find("radios")->array.front(), "available"),
        "hardware disabled radios are not advertised as controllable");
    result = SampleBluetooth([] { return BluetoothCollection{json::Array(), "accessDenied"}; }, peersFail, cancel);
    Require(!result.available && result.error == "accessDenied" && deviceReads == 1,
        "denied radio access remains unavailable instead of reusing a previous radio");
    result = SampleBluetooth([] { return BluetoothCollection{}; }, peersFail, cancel);
    Require(result.available && result.value.Find("radios")->array.empty() && deviceReads == 1,
        "device removal publishes an empty radio list");
    cancel.canceled->store(true);
    result = SampleBluetooth([&] { return radios; }, peersFail, cancel);
    Require(!result.available && result.error == "canceled" && deviceReads == 1, "canceled Bluetooth reads are never published as successful");
}
}
void TestSystemControls()
{
    {
        auto adapter = json::Object(), networks = json::Array();
        const auto network = [](const char* id, const char* name, bool connected, double signal) {
            auto value = json::Object(); value.object["id"] = json::Text(id); value.object["ssid"] = json::Text(name);
            value.object["connected"] = json::Boolean(connected); value.object["signal"] = json::Number(signal);
            value.object["connectable"] = json::Boolean(true); return value;
        };
        networks.array = {network("wifi-open", "HIT-WLAN", false, 93), network("hidden", "", false, 90),
            network("wifi-open", "HIT-WLAN", true, 85), network("wifi-secure", "HIT-WLAN", false, 80)};
        adapter.object["networks"] = networks;
        const auto visible = WifiPresentationNetworks(adapter);
        Require(visible.size() == 2 && json::Flag(visible[0], "connected") && json::Numeric(visible[0], "signal") == 93 &&
            json::String(visible[1], "id") == "wifi-secure", "Wi-Fi merges duplicate profiles, drops unnamed scans and retains distinct security identities");
        Require(adapter.Find("networks")->array.size() == 4, "presentation filtering does not alter provider data");
    }
    SharedSourceAndRelease();
    ControlQueueCancellationAndReadback();
    LastConsumerDuringExecutionAndReadback(true);
    LastConsumerDuringExecutionAndReadback(false);
    NewSubscriberDuringRelease();
    DirectTaskWithoutSubscriptionReleases();
    SlowSampleYieldsToReadyControl();
    CancellableWinRtSamplingWait();
    HostOnlyCredentialsAndConfirmation();
    WidgetControlArgumentsStayWithinTheirContract();
    WidgetControlBridgeOwnsTaskLifetimes();
    ReusedConsumerCannotReceiveDetachedWork();
    BrightnessSettlingAndStaleFeedback();
    BrightnessIdentityAndLegacyTargets();
    BluetoothPowerAndDeviceReadFailures();
    AudioPresentationKeepsRealEndpoints();
    WifiOffStillHasManageableInterface();
    ControlReadbackMustReallySettle();
    NewSliderCancelsExecutingOldTarget();
    CancellationDuringReadbackDiscardsOldState();
    QueuedCancellationDoesNotWaitForOrReleaseAnotherControl();
    WirelessNotificationsNeedActualState();
    FreshReadbackOnlyConfirmsTheRequestedTarget();
}
