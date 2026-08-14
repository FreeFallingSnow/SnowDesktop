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
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

void TestTopicLifecycleAndSampling()
{
    WidgetSystemDataProvider provider;
    Check(!provider.StartTopic("system.display.topology", 20ms) &&
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
            (memory->available || !memory->error.empty()),
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

    Check(provider.StopTopic("system.memory") && provider.Running() &&
            provider.ActiveTopicCount() == 1,
        "stopping one topic must preserve another active topic");
    Check(provider.StopTopic("system.cpu") && !provider.Running() &&
            provider.ActiveTopicCount() == 0,
        "stopping the final topic must join the provider worker");
    Check(!provider.StopTopic("system.cpu"),
        "stopping an inactive topic must report no change");
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
    TestTopicLifecycleAndSampling();
    TestStopAll();
    std::cout << "widget system data provider tests passed\n";
    return 0;
}
