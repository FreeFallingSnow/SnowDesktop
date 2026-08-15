#pragma once

#include "widget_runtime_image.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetCpuDataSnapshot
{
    bool available = false;
    bool warmingUp = true;
    double usagePercent = 0.0;
    unsigned int logicalProcessors = 0;
    std::string name;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetMemoryDataSnapshot
{
    bool available = false;
    std::uint64_t totalBytes = 0;
    std::uint64_t usedBytes = 0;
    std::uint64_t freeBytes = 0;
    double usagePercent = 0.0;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetPowerDataSnapshot
{
    bool available = false;
    bool acPower = false;
    bool charging = false;
    bool saver = false;
    double batteryPercent = 0.0;
    std::int64_t estimatedRemainingSeconds = -1;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetNetworkStatusDataSnapshot
{
    bool available = false;
    std::string connectivity = "none";
    std::string transport = "none";
    bool costKnown = false;
    bool metered = false;
    bool roaming = false;
    bool overLimit = false;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetNetworkTrafficDataSnapshot
{
    bool available = false;
    bool connected = false;
    bool warmingUp = true;
    std::uint64_t receivedBytes = 0;
    std::uint64_t sentBytes = 0;
    std::uint64_t downloadBytesPerSecond = 0;
    std::uint64_t uploadBytesPerSecond = 0;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetGpuAdapterDataSnapshot
{
    std::string id;
    std::string name;
    double usagePercent = 0.0;
    std::uint64_t dedicatedMemoryBytes = 0;
    std::uint64_t dedicatedUsedBytes = 0;
    std::uint64_t sharedMemoryBytes = 0;
    std::uint64_t sharedUsedBytes = 0;
};

struct WidgetGpuDataSnapshot
{
    bool available = false;
    bool warmingUp = true;
    std::vector<WidgetGpuAdapterDataSnapshot> adapters;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetStorageVolumeDataSnapshot
{
    std::string id;
    std::string displayName;
    std::string mountPoint;
    std::string kind;
    std::uint64_t capacityBytes = 0;
    std::uint64_t freeBytes = 0;
    bool capacityAvailable = false;
    bool removable = false;
    bool readOnly = false;
};

struct WidgetStorageVolumesDataSnapshot
{
    bool available = false;
    std::vector<WidgetStorageVolumeDataSnapshot> volumes;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetStorageIoDataSnapshot
{
    bool available = false;
    bool warmingUp = true;
    std::uint64_t readBytesPerSecond = 0;
    std::uint64_t writeBytesPerSecond = 0;
    double busyPercent = 0.0;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetDisplayRectDataSnapshot
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct WidgetDisplayPixelRectDataSnapshot
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct WidgetDisplayDataSnapshot
{
    std::string id;
    std::string name;
    bool primary = false;
    WidgetDisplayRectDataSnapshot bounds;
    WidgetDisplayRectDataSnapshot workArea;
    WidgetDisplayPixelRectDataSnapshot pixelBounds;
    WidgetDisplayPixelRectDataSnapshot pixelWorkArea;
    unsigned int dpiX = 96;
    unsigned int dpiY = 96;
    double scale = 1.0;
    double refreshHz = 0.0;
    std::string orientation;
    bool hdrKnown = false;
    bool hdrSupported = false;
    bool hdrEnabled = false;
};

struct WidgetDisplayTopologyDataSnapshot
{
    bool available = false;
    std::vector<WidgetDisplayDataSnapshot> displays;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetAudioOutputDefaultDataSnapshot
{
    bool available = false;
    std::string id;
    std::string name;
    std::string state;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetAudioOutputVolumeDataSnapshot
{
    bool available = false;
    std::string endpointId;
    double volume = 0.0;
    bool muted = false;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetMediaControlsDataSnapshot
{
    bool canPlay = false;
    bool canPause = false;
    bool canPlayPause = false;
    bool canStop = false;
    bool canNext = false;
    bool canPrevious = false;
    bool canSeek = false;
    bool canChangePlaybackRate = false;
    bool canToggleShuffle = false;
    bool canChangeRepeatMode = false;
};

struct WidgetMediaTimelineDataSnapshot
{
    bool available = false;
    std::string sessionId;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    std::int64_t minimumSeekMs = 0;
    std::int64_t maximumSeekMs = 0;
    std::int64_t updatedAtMs = 0;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetMediaSessionDataSnapshot
{
    std::string id;
    std::string sourceName;
    std::string title;
    std::string artist;
    std::string album;
    std::string playbackStatus = "closed";
    bool current = false;
    WidgetMediaControlsDataSnapshot controls;
    WidgetMediaTimelineDataSnapshot timeline;
};

struct WidgetMediaArtworkDataSnapshot
{
    bool available = false;
    std::string sessionId;
    std::string resourceToken;
    std::shared_ptr<const WidgetRuntimeImagePixels> pixels;
    std::uint64_t mediaIdentity = 0;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetMediaSessionsDataSnapshot
{
    bool available = false;
    std::string currentSessionId;
    std::vector<WidgetMediaSessionDataSnapshot> sessions;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
    WidgetMediaArtworkDataSnapshot artwork;
};

struct WidgetMediaCurrentDataSnapshot
{
    bool available = false;
    WidgetMediaSessionDataSnapshot session;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

std::optional<WidgetDisplayDataSnapshot> MatchDisplayByPixelBounds(
    const WidgetDisplayTopologyDataSnapshot& topology,
    const WidgetDisplayPixelRectDataSnapshot& bounds);

class WidgetSystemDataProvider
{
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::milliseconds MinimumInterval{ 10 };
    static constexpr std::chrono::milliseconds MaximumInterval{
        24 * 60 * 60 * 1000 };

    WidgetSystemDataProvider() = default;
    ~WidgetSystemDataProvider();

    WidgetSystemDataProvider(const WidgetSystemDataProvider&) = delete;
    WidgetSystemDataProvider& operator=(
        const WidgetSystemDataProvider&) = delete;

    bool StartTopic(std::string_view topic,
        std::chrono::milliseconds interval);
    bool StopTopic(std::string_view topic);
    void StopAll();

    std::optional<WidgetCpuDataSnapshot> Cpu() const;
    std::optional<WidgetMemoryDataSnapshot> Memory() const;
    std::optional<WidgetPowerDataSnapshot> Power() const;
    std::optional<WidgetNetworkStatusDataSnapshot> NetworkStatus() const;
    std::optional<WidgetNetworkTrafficDataSnapshot> NetworkTraffic() const;
    std::optional<WidgetGpuDataSnapshot> Gpu() const;
    std::optional<WidgetStorageVolumesDataSnapshot> StorageVolumes() const;
    std::optional<WidgetStorageIoDataSnapshot> StorageIo() const;
    std::optional<WidgetDisplayTopologyDataSnapshot> DisplayTopology() const;
    std::optional<WidgetDisplayTopologyDataSnapshot> DisplayCurrent() const;
    std::optional<WidgetAudioOutputDefaultDataSnapshot>
        AudioOutputDefault() const;
    std::optional<WidgetAudioOutputVolumeDataSnapshot>
        AudioOutputVolume() const;
    std::optional<WidgetMediaSessionsDataSnapshot> MediaSessions() const;
    std::optional<WidgetMediaCurrentDataSnapshot> MediaCurrent() const;
    std::optional<WidgetMediaTimelineDataSnapshot> MediaTimeline() const;
    std::optional<WidgetMediaArtworkDataSnapshot> MediaArtwork() const;
    std::vector<std::string> DrainChangedTopics();

    bool Running() const noexcept;
    bool GpuResourcesActive() const noexcept;
    bool StorageIoResourcesActive() const noexcept;
    std::size_t ActiveTopicCount() const;
    static bool SupportsTopic(std::string_view topic) noexcept;

private:
    struct TopicSchedule
    {
        std::chrono::milliseconds interval{ 1000 };
        Clock::time_point due{};
    };

    void WorkerMain(std::stop_token stopToken);
    WidgetCpuDataSnapshot SampleCpu();
    WidgetMemoryDataSnapshot SampleMemory();
    WidgetPowerDataSnapshot SamplePower();
    WidgetNetworkStatusDataSnapshot SampleNetworkStatus();
    WidgetNetworkTrafficDataSnapshot SampleNetworkTraffic();
    WidgetGpuDataSnapshot SampleGpu();
    WidgetStorageVolumesDataSnapshot SampleStorageVolumes();
    WidgetStorageIoDataSnapshot SampleStorageIo();
    WidgetDisplayTopologyDataSnapshot SampleDisplayTopology();
    WidgetAudioOutputDefaultDataSnapshot SampleAudioOutputDefault();
    WidgetAudioOutputVolumeDataSnapshot SampleAudioOutputVolume();
    WidgetMediaSessionsDataSnapshot SampleMediaSessions(
        bool includeArtwork = false);
    void PublishCpu(WidgetCpuDataSnapshot snapshot);
    void PublishMemory(WidgetMemoryDataSnapshot snapshot);
    void PublishPower(WidgetPowerDataSnapshot snapshot);
    void PublishNetworkStatus(WidgetNetworkStatusDataSnapshot snapshot);
    void PublishNetworkTraffic(WidgetNetworkTrafficDataSnapshot snapshot);
    void PublishGpu(WidgetGpuDataSnapshot snapshot);
    void PublishStorageVolumes(WidgetStorageVolumesDataSnapshot snapshot);
    void PublishStorageIo(WidgetStorageIoDataSnapshot snapshot);
    void PublishDisplayTopology(WidgetDisplayTopologyDataSnapshot snapshot);
    void PublishDisplayCurrent(WidgetDisplayTopologyDataSnapshot snapshot);
    void PublishAudioOutputDefault(
        WidgetAudioOutputDefaultDataSnapshot snapshot);
    void PublishAudioOutputVolume(
        WidgetAudioOutputVolumeDataSnapshot snapshot);
    void PublishMediaSessions(WidgetMediaSessionsDataSnapshot snapshot);
    void PublishMediaCurrent(const WidgetMediaSessionsDataSnapshot& snapshot);
    void PublishMediaTimeline(const WidgetMediaSessionsDataSnapshot& snapshot);
    void PublishMediaArtwork(const WidgetMediaSessionsDataSnapshot& snapshot);
    bool InitializeGpuQuery();
    void CloseGpuQuery();
    bool InitializeStorageIoQuery();
    void CloseStorageIoQuery();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<std::string, TopicSchedule> schedules_;
    std::unordered_set<std::string> changedTopics_;
    std::optional<WidgetCpuDataSnapshot> cpu_;
    std::optional<WidgetMemoryDataSnapshot> memory_;
    std::optional<WidgetPowerDataSnapshot> power_;
    std::optional<WidgetNetworkStatusDataSnapshot> networkStatus_;
    std::optional<WidgetNetworkTrafficDataSnapshot> networkTraffic_;
    std::optional<WidgetGpuDataSnapshot> gpu_;
    std::optional<WidgetStorageVolumesDataSnapshot> storageVolumes_;
    std::optional<WidgetStorageIoDataSnapshot> storageIo_;
    std::optional<WidgetDisplayTopologyDataSnapshot> displayTopology_;
    std::optional<WidgetDisplayTopologyDataSnapshot> displayCurrent_;
    std::optional<WidgetAudioOutputDefaultDataSnapshot> audioOutputDefault_;
    std::optional<WidgetAudioOutputVolumeDataSnapshot> audioOutputVolume_;
    std::optional<WidgetMediaSessionsDataSnapshot> mediaSessions_;
    std::optional<WidgetMediaCurrentDataSnapshot> mediaCurrent_;
    std::optional<WidgetMediaTimelineDataSnapshot> mediaTimeline_;
    std::optional<WidgetMediaArtworkDataSnapshot> mediaArtwork_;
    std::uint64_t configurationGeneration_ = 0;
    std::jthread worker_;
    std::atomic<bool> resetCpuBaseline_{ true };
    std::atomic<bool> resetNetworkBaseline_{ true };
    std::atomic<bool> resetGpuBaseline_{ true };
    std::atomic<bool> closeGpuRequested_{ false };
    std::atomic<bool> gpuResourcesActive_{ false };
    std::atomic<bool> resetStorageIoBaseline_{ true };
    std::atomic<bool> closeStorageIoRequested_{ false };
    std::atomic<bool> storageIoResourcesActive_{ false };

    std::uint64_t previousIdle_ = 0;
    std::uint64_t previousKernel_ = 0;
    std::uint64_t previousUser_ = 0;
    std::uint64_t previousReceived_ = 0;
    std::uint64_t previousSent_ = 0;
    Clock::time_point previousNetworkSample_{};
    void* gpuQuery_ = nullptr;
    void* gpuUtilizationCounter_ = nullptr;
    void* gpuDedicatedUsageCounter_ = nullptr;
    void* gpuSharedUsageCounter_ = nullptr;
    void* storageIoQuery_ = nullptr;
    void* storageReadCounter_ = nullptr;
    void* storageWriteCounter_ = nullptr;
    void* storageBusyCounter_ = nullptr;
};
}
