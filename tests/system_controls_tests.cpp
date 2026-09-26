#include "system_controls.h"
#include "system_control_feedback.h"
#include "system_control_bluetooth_sampling.h"
#include "system_control_wifi_presentation.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>

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
    HostOnlyCredentialsAndConfirmation();
    BrightnessSettlingAndStaleFeedback();
    BluetoothPowerAndDeviceReadFailures();
}
