#include "widget_system_data_provider.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using snowdesktop::widget_runtime::WidgetSystemDataProvider;
using snowdesktop::widget_runtime::WidgetNetworkStatusDataSnapshot;
using snowdesktop::widget_runtime::WidgetNetworkStatusDebouncer;
using snowdesktop::widget_runtime::WidgetDisplayDataSnapshot;
using snowdesktop::widget_runtime::WidgetDisplayTopologyDataSnapshot;
using snowdesktop::widget_runtime::WidgetDisplayPixelRectDataSnapshot;
using snowdesktop::widget_runtime::MatchDisplayByPixelBounds;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

template<typename Predicate>
bool WaitFor(Predicate predicate)
{
    for (int attempt = 0; attempt < 400; ++attempt)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

void TestTopicLifecycleAndSampling()
{
    WidgetSystemDataProvider provider;
    Check(!provider.StartTopic("media.unknown", 20ms) &&
            !provider.StartTopic("system.memory", 1ms),
        "unsupported topics and invalid intervals must be rejected");
    Check(provider.StartTopic("system.memory", 20ms) &&
            provider.Running() && provider.ActiveTopicCount() == 1,
        "the first topic must start the provider worker on demand");
    Check(WaitFor([&] {
            const auto snapshot = provider.Memory();
            return snapshot && snapshot->revision > 0;
        }),
        "memory sampling must publish an immutable revision");
    const auto memory = provider.Memory();
    Check(memory && memory->timestampMs > 0 &&
            (memory->available || !memory->error.empty()) &&
            (!memory->available ||
                (memory->commitLimitBytes >= memory->commitUsedBytes &&
                    memory->commitAvailableBytes ==
                        memory->commitLimitBytes -
                            memory->commitUsedBytes)),
        "memory snapshots must include availability or a stable error");
    const auto changed = provider.DrainChangedTopics();
    Check(changed.size() == 1 && changed[0] == "system.memory",
        "changed topics must be drainable without unrelated sources");

    Check(provider.StartTopic("system.memory", 50ms) &&
            provider.ActiveTopicCount() == 1,
        "reconfiguration must not duplicate an active topic");
    Check(provider.StartTopic("system.cpu", 20ms) &&
            provider.ActiveTopicCount() == 2,
        "CPU and memory must be independently active on one worker");
    Check(WaitFor([&] {
            const auto snapshot = provider.Cpu();
            return snapshot && snapshot->revision >= 2;
        }),
        "CPU sampling must publish warming and differential revisions");
    Check(provider.StartTopic("system.power", 20ms) &&
            provider.ActiveTopicCount() == 3,
        "power sampling must start independently on the shared worker");
    Check(WaitFor([&] {
            const auto snapshot = provider.Power();
            return snapshot && snapshot->revision > 0;
        }),
        "power sampling must publish an immutable revision");
    const auto power = provider.Power();
    Check(power && power->timestampMs > 0 &&
            (power->available || !power->error.empty()),
        "power snapshots must distinguish battery data from unavailability");
    Check(provider.StopTopic("system.power") && provider.Running() &&
            provider.ActiveTopicCount() == 2,
        "stopping power sampling must preserve CPU and memory topics");
    Check(provider.StartTopic("system.network.status", 20ms) &&
            provider.StartTopic("system.network.traffic", 20ms) &&
            provider.ActiveTopicCount() == 4,
        "network status and traffic must start as independent topics");
    Check(WaitFor([&] {
            const auto status = provider.NetworkStatus();
            const auto traffic = provider.NetworkTraffic();
            return status && status->revision > 0 &&
                traffic && traffic->revision >= 2;
        }),
        "network topics must publish status and differential traffic revisions");
    const auto networkStatus = provider.NetworkStatus();
    const auto networkTraffic = provider.NetworkTraffic();
    Check(networkStatus && networkStatus->timestampMs > 0 &&
            (networkStatus->available || !networkStatus->error.empty()),
        "network status must publish availability or a stable error");
    Check(networkTraffic && networkTraffic->timestampMs > 0 &&
            (networkTraffic->available || !networkTraffic->error.empty()),
        "network traffic must publish availability or a stable error");
    Check(provider.StopTopic("system.network.status") &&
            provider.ActiveTopicCount() == 3 &&
            provider.StopTopic("system.network.traffic") &&
            provider.ActiveTopicCount() == 2,
        "network topics must stop independently without stopping CPU or memory");
    Check(provider.StartTopic("system.gpu", 20ms) &&
            provider.ActiveTopicCount() == 3,
        "GPU sampling must start as its own topic");
    Check(WaitFor([&] {
            const auto snapshot = provider.Gpu();
            return snapshot && snapshot->revision >= 2;
        }),
        "GPU sampling must publish warming and measured revisions");
    const auto gpu = provider.Gpu();
    Check(gpu && gpu->timestampMs > 0 &&
            (gpu->available || !gpu->error.empty()) &&
            (!gpu->available || !gpu->adapters.empty()),
        "GPU snapshots must expose all discovered adapters or a stable error");
    Check(provider.StopTopic("system.gpu") &&
            provider.ActiveTopicCount() == 2 &&
            WaitFor([&] { return !provider.GpuResourcesActive(); }),
        "the final GPU subscription must close PDH resources while the worker remains active");
    Check(provider.StartTopic("system.storage.volumes", 20ms) &&
            provider.ActiveTopicCount() == 3,
        "volume sampling must start as an independent topic");
    Check(WaitFor([&] {
            const auto snapshot = provider.StorageVolumes();
            return snapshot && snapshot->revision > 0;
        }),
        "volume sampling must publish an immutable revision");
    const auto volumes = provider.StorageVolumes();
    Check(volumes && volumes->timestampMs > 0 &&
            (volumes->available || !volumes->error.empty()) &&
            (!volumes->available || !volumes->volumes.empty()),
        "volume snapshots must expose mounted volumes or a stable error");
    if (volumes && volumes->available && !volumes->volumes.empty())
    {
        const auto& volume = volumes->volumes.front();
        Check(volume.id.starts_with("volume-") &&
                !volume.displayName.empty() &&
                !volume.mountPoint.empty() &&
                !volume.kind.empty() &&
                (!volume.capacityAvailable ||
                    (volume.capacityBytes > 0 &&
                        volume.freeBytes <= volume.capacityBytes)),
            "volume entries must use opaque IDs and bounded capacity fields");
    }
    Check(provider.StopTopic("system.storage.volumes") &&
            provider.ActiveTopicCount() == 2,
        "stopping volume sampling must preserve CPU and memory topics");
    Check(provider.StartTopic("system.storage.io", 20ms) &&
            provider.ActiveTopicCount() == 3,
        "storage I/O sampling must start as an independent topic");
    Check(WaitFor([&] {
            const auto snapshot = provider.StorageIo();
            return snapshot && snapshot->revision >= 2;
        }),
        "storage I/O sampling must publish warming and measured revisions");
    const auto storageIo = provider.StorageIo();
    Check(storageIo && storageIo->timestampMs > 0 &&
            (storageIo->available || !storageIo->error.empty()) &&
            (!storageIo->available || storageIo->busyPercent <= 100.0),
        "storage I/O snapshots must expose aggregate rates or a stable error");
    Check(provider.StopTopic("system.storage.io") &&
            provider.ActiveTopicCount() == 2 &&
            WaitFor([&] { return !provider.StorageIoResourcesActive(); }),
        "the final storage I/O subscription must close PDH resources while the worker remains active");
    Check(provider.StartTopic("system.display.topology", 20ms) &&
            provider.ActiveTopicCount() == 3,
        "display topology sampling must start as an independent topic");
    Check(WaitFor([&] {
            const auto snapshot = provider.DisplayTopology();
            return snapshot && snapshot->revision > 0;
        }),
        "display topology sampling must publish an immutable revision");
    const auto displays = provider.DisplayTopology();
    Check(displays && displays->timestampMs > 0 &&
            (displays->available || !displays->error.empty()) &&
            (!displays->available || !displays->displays.empty()),
        "display topology must expose active displays or a stable error");
    if (displays && displays->available && !displays->displays.empty())
    {
        const auto& display = displays->displays.front();
        Check(display.id.starts_with("display-") &&
                !display.name.empty() && display.dpiX > 0 &&
                display.dpiY > 0 && display.scale > 0.0 &&
                display.pixelBounds.width > 0 &&
                display.pixelBounds.height > 0 &&
                display.bounds.width > 0.0 &&
                display.bounds.height > 0.0 &&
                !display.orientation.empty(),
            "display entries must use opaque IDs and complete geometry metadata");
    }
    Check(provider.StopTopic("system.display.topology") &&
            provider.ActiveTopicCount() == 2,
        "stopping topology sampling must preserve CPU and memory topics");
    Check(provider.StartTopic("system.display.current", 20ms) &&
            provider.ActiveTopicCount() == 3,
        "current-display source sampling must start independently");
    Check(WaitFor([&] {
            const auto snapshot = provider.DisplayCurrent();
            return snapshot && snapshot->revision > 0;
        }),
        "current-display source must publish active monitor metadata");
    const auto currentDisplay = provider.DisplayCurrent();
    Check(currentDisplay && currentDisplay->timestampMs > 0 &&
            (currentDisplay->available || !currentDisplay->error.empty()) &&
            (!currentDisplay->available ||
                !currentDisplay->displays.empty()),
        "current-display source must expose matchable topology or a stable error");
    Check(provider.StopTopic("system.display.current") &&
            provider.ActiveTopicCount() == 2,
        "stopping current-display sampling must preserve other topics");
    Check(provider.StartTopic("audio.output.default", 20ms) &&
            provider.StartTopic("audio.output.volume", 20ms) &&
            provider.ActiveTopicCount() == 4,
        "default audio endpoint and volume must start as independent topics");
    Check(WaitFor([&] {
            const auto endpoint = provider.AudioOutputDefault();
            const auto volume = provider.AudioOutputVolume();
            return endpoint && endpoint->revision > 0 &&
                volume && volume->revision > 0;
        }),
        "audio output topics must publish endpoint and volume revisions");
    const auto audioEndpoint = provider.AudioOutputDefault();
    const auto audioVolume = provider.AudioOutputVolume();
    Check(audioEndpoint && audioEndpoint->timestampMs > 0 &&
            (audioEndpoint->available || !audioEndpoint->error.empty()) &&
            (!audioEndpoint->available ||
                (audioEndpoint->id.starts_with("audio-output-") &&
                    !audioEndpoint->state.empty())),
        "default audio output must expose an opaque endpoint or stable error");
    Check(audioVolume && audioVolume->timestampMs > 0 &&
            (audioVolume->available || !audioVolume->error.empty()) &&
            (!audioVolume->available ||
                (audioVolume->endpointId.starts_with("audio-output-") &&
                    audioVolume->volume >= 0.0 &&
                    audioVolume->volume <= 1.0)),
        "audio volume must expose a bounded scalar or stable error");
    Check(provider.StopTopic("audio.output.default") &&
            provider.ActiveTopicCount() == 3 &&
            provider.StopTopic("audio.output.volume") &&
            provider.ActiveTopicCount() == 2,
        "audio output topics must stop independently from other sources");
    Check(provider.StartTopic("media.sessions", 20ms) &&
            provider.StartTopic("media.current", 20ms) &&
            provider.StartTopic("media.timeline", 20ms) &&
            provider.StartTopic("media.artwork", 20ms) &&
            provider.ActiveTopicCount() == 6,
        "media list, current session, timeline, and artwork must start independently");
    Check(WaitFor([&] {
            const auto sessions = provider.MediaSessions();
            const auto current = provider.MediaCurrent();
            const auto timeline = provider.MediaTimeline();
            const auto artwork = provider.MediaArtwork();
            return sessions && sessions->revision > 0 &&
                current && current->revision > 0 &&
                timeline && timeline->revision > 0 &&
                artwork && artwork->revision > 0;
        }),
        "media topics must publish coalesced revisions");
    const auto mediaSessions = provider.MediaSessions();
    const auto mediaCurrent = provider.MediaCurrent();
    const auto mediaTimeline = provider.MediaTimeline();
    const auto mediaArtwork = provider.MediaArtwork();
    Check(mediaSessions && mediaSessions->timestampMs > 0 &&
            (mediaSessions->available || !mediaSessions->error.empty()) &&
            mediaSessions->sessions.size() <= 32,
        "media session enumeration must be bounded or report a stable error");
    if (mediaSessions && mediaSessions->available)
    {
        for (const auto& session : mediaSessions->sessions)
        {
            Check(session.id.starts_with("media-session-") &&
                    session.title.size() <= 4096 &&
                    session.artist.size() <= 4096 &&
                    session.album.size() <= 4096 &&
                    session.timeline.sessionId == session.id &&
                    session.timeline.positionMs >= 0 &&
                    session.timeline.durationMs >= 0 &&
                    session.timeline.positionMs <=
                        session.timeline.durationMs,
                "media entries must use opaque IDs and bounded metadata and timeline values");
        }
    }
    Check(mediaCurrent && mediaCurrent->timestampMs > 0 &&
            (mediaCurrent->available || !mediaCurrent->error.empty()),
        "current media must expose one session or notPresent");
    Check(mediaTimeline && mediaTimeline->timestampMs > 0 &&
            (mediaTimeline->available || !mediaTimeline->error.empty()),
        "media timeline must expose bounded values or notPresent");
    Check(mediaArtwork && mediaArtwork->timestampMs > 0 &&
            (mediaArtwork->available || !mediaArtwork->error.empty()) &&
            (!mediaArtwork->available ||
                (mediaArtwork->resourceToken.starts_with("@media:") &&
                    mediaArtwork->pixels &&
                    mediaArtwork->pixels->width <= 512 &&
                    mediaArtwork->pixels->height <= 512 &&
                    mediaArtwork->pixels->bgraPremultiplied.size() ==
                        static_cast<std::size_t>(
                            mediaArtwork->pixels->stride) *
                            mediaArtwork->pixels->height)),
        "media artwork must expose bounded decoded pixels or a stable error");
    Check(provider.StopTopic("media.sessions") &&
            provider.ActiveTopicCount() == 5 &&
            provider.StopTopic("media.current") &&
            provider.ActiveTopicCount() == 4 &&
            provider.StopTopic("media.timeline") &&
            provider.ActiveTopicCount() == 3 &&
            provider.StopTopic("media.artwork") &&
            provider.ActiveTopicCount() == 2,
        "media topics must stop independently from other sources");

    Check(provider.StartTopic("process.summary", 20ms) &&
            provider.ActiveTopicCount() == 3,
        "process summary sampling must start only when subscribed");
    Check(WaitFor([&] {
            const auto snapshot = provider.ProcessSummary();
            return snapshot && snapshot->revision >= 2 &&
                !snapshot->warmingUp;
        }),
        "process summary must publish a shared differential sample");
    const auto processSummary = provider.ProcessSummary();
    Check(processSummary && processSummary->available &&
            processSummary->timestampMs > 0 &&
            !processSummary->processes.empty() &&
            processSummary->processes.size() <=
                WidgetSystemDataProvider::MaximumProcessSummaryEntries &&
            processSummary->observedCount >=
                processSummary->processes.size() &&
            processSummary->truncated ==
                (processSummary->observedCount >
                    processSummary->processes.size()),
        "process summary must expose a bounded top-N view");
    if (processSummary && processSummary->available)
    {
        for (const auto& process : processSummary->processes)
        {
            Check(process.id.starts_with("process-") &&
                    !process.name.empty() &&
                    process.name.find('\\') == std::string::npos &&
                    process.name.find('/') == std::string::npos &&
                    process.cpuPercent >= 0.0 &&
                    process.cpuPercent <= 100.0,
                "process entries must be opaque, path-free, and bounded");
        }
    }
    Check(provider.StopTopic("process.summary") &&
            provider.ActiveTopicCount() == 2 &&
            !provider.ProcessSummary(),
        "stopping process summary must clear it while preserving CPU and memory topics");

    Check(provider.StopTopic("system.memory") && provider.Running() &&
            provider.ActiveTopicCount() == 1,
        "stopping one topic must preserve another active topic");
    Check(provider.StopTopic("system.cpu") && !provider.Running() &&
            provider.ActiveTopicCount() == 0,
        "stopping the final topic must join the provider worker");
    Check(!provider.StopTopic("system.cpu"),
        "stopping an inactive topic must report no change");
}

void TestNetworkStatusDebounce()
{
    const auto sample = [](const std::string& connectivity,
                            std::int64_t timestamp) {
        WidgetNetworkStatusDataSnapshot result;
        result.available = true;
        result.connectivity = connectivity;
        result.transport = connectivity == "none" ? "none" : "ethernet";
        result.timestampMs = timestamp;
        return result;
    };

    WidgetNetworkStatusDebouncer debouncer;
    const auto initial = debouncer.Push(sample("internet", 100));
    Check(initial.connectivity == "internet" && initial.timestampMs == 100,
        "the first network status sample must publish immediately");

    const auto transient = debouncer.Push(sample("none", 200));
    Check(transient.connectivity == "internet" && transient.timestampMs == 200,
        "one contradictory sample must retain the stable network semantics");

    const auto recovered = debouncer.Push(sample("internet", 300));
    Check(recovered.connectivity == "internet" &&
            recovered.timestampMs == 300,
        "a stable sample must clear an unconfirmed candidate");

    const auto firstDisconnect = debouncer.Push(sample("none", 400));
    const auto confirmedDisconnect = debouncer.Push(sample("none", 500));
    Check(firstDisconnect.connectivity == "internet" &&
            confirmedDisconnect.connectivity == "none",
        "a semantic network change must require two consecutive samples");

    debouncer.Reset();
    const auto afterReset = debouncer.Push(sample("local", 600));
    Check(afterReset.connectivity == "local",
        "reset must let the next network state publish immediately");
}

void TestCurrentDisplayMatching()
{
    WidgetDisplayTopologyDataSnapshot topology;
    WidgetDisplayDataSnapshot primary;
    primary.id = "display-primary";
    primary.primary = true;
    primary.pixelBounds = { 0, 0, 1920, 1080 };
    WidgetDisplayDataSnapshot secondary;
    secondary.id = "display-secondary";
    secondary.pixelBounds = { 1920, 0, 2560, 1440 };
    topology.displays = { primary, secondary };

    const auto match = MatchDisplayByPixelBounds(
        topology, WidgetDisplayPixelRectDataSnapshot{
            1920, 0, 2560, 1440 });
    Check(match && match->id == "display-secondary",
        "current-display matching must select exact surface monitor bounds");
    Check(!MatchDisplayByPixelBounds(topology,
            WidgetDisplayPixelRectDataSnapshot{ 10, 10, 100, 100 }),
        "current-display matching must not silently fall back to primary");
}

void TestStopAll()
{
    WidgetSystemDataProvider provider;
    Check(provider.StartTopic("system.cpu", 20ms),
        "CPU topic must start for shutdown testing");
    provider.StopAll();
    Check(!provider.Running() && provider.ActiveTopicCount() == 0 &&
            provider.DrainChangedTopics().empty(),
        "StopAll must synchronously release the worker and pending changes");
}
}

int main()
{
    TestCurrentDisplayMatching();
    TestNetworkStatusDebounce();
    TestTopicLifecycleAndSampling();
    TestStopAll();
    std::cout << "widget system data provider tests passed\n";
    return 0;
}
