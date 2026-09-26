#include "system_controls.h"
#include "system_control_feedback.h"
#include "system_control_bluetooth_sampling.h"
#include "system_control_wifi_presentation.h"
#include "system_control_windows.h"
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
    std::map<std::string, Snapshot> Sample(std::string_view source, const Cancellation& cancel) override
    {
        std::unique_lock guard(mutex); ++samples; sampleEntered = true; changed.notify_all();
        while (blockSample && !cancel.Stop()) changed.wait_for(guard, 10ms);
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
        changed.notify_all(); return {false, "deviceGone", 1167};
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
        --users[source]; changed.notify_all(); return {};
    }
    Result Execute(const Request& request, const Cancellation&) override
    {
        const std::string source(Source(request.name)); std::unique_lock guard(mutex); Enter(source);
        resources.insert(source); ++executions[source]; changed.notify_all();
        if (source == "brightness") Await(guard, [&] { return !holdExecute; }, "execution gate was not released");
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
    BrightnessSettlingAndStaleFeedback();
    BluetoothPowerAndDeviceReadFailures();
}
