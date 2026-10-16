#pragma once

#include "widget_data_semantic_debounce.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct IMMDeviceEnumerator;

namespace snowdesktop::widget_runtime
{
struct WidgetAudioAnalysisDataSnapshot
{
    bool available = false;
    bool warmingUp = true;
    bool silent = true;
    bool deviceChanged = false;
    std::string endpointId;
    std::vector<double> waveform;
    std::vector<double> spectrum;
    bool hasWaveform = true;
    bool hasSpectrum = true;
    bool hasRms = true;
    bool hasPeak = true;
    double rms = 0.0;
    double peak = 0.0;
    unsigned int sampleRate = 0;
    unsigned int channels = 0;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};

struct WidgetAudioAnalysisConfiguration
{
    bool waveform = true;
    bool spectrum = true;
    bool rms = true;
    bool peak = true;
    std::size_t waveformPoints = 128;
    std::size_t spectrumBins = 64;
    bool operator==(const WidgetAudioAnalysisConfiguration&) const = default;
};

bool IsRepeatedSilentWidgetAudioSnapshot(
    const WidgetAudioAnalysisDataSnapshot& previous,
    const WidgetAudioAnalysisDataSnapshot& next) noexcept;

// The capture worker and deterministic regression fixture use the same
// publication state. The provider serializes it with its snapshot mutex.
class WidgetAudioAnalysisPublication
{
public:
    bool Publish(WidgetAudioAnalysisDataSnapshot snapshot);
    bool DrainChanged() noexcept { return std::exchange(changed_, false); }
    const std::optional<WidgetAudioAnalysisDataSnapshot>& Latest() const noexcept { return snapshot_; }
    void Reset() noexcept;

private:
    std::optional<WidgetAudioAnalysisDataSnapshot> snapshot_;
    WidgetDataSemanticDebouncer semanticDebouncer_;
    bool changed_ = false;
};

WidgetAudioAnalysisDataSnapshot ProjectWidgetAudioAnalysisSnapshot(
    const WidgetAudioAnalysisDataSnapshot& snapshot,
    const WidgetAudioAnalysisConfiguration& configuration);

std::vector<double> ComputeWidgetAudioSpectrum(
    const std::vector<double>& samples, unsigned int sampleRate,
    std::size_t spectrumBins);

class WidgetAudioAnalysisProvider
{
public:
    using Clock = std::chrono::steady_clock;
    using ChangedCallback = std::function<void()>;

    static constexpr std::size_t DefaultWaveformPoints = 128;
    static constexpr std::size_t MaximumWaveformPoints = 256;
    static constexpr std::size_t DefaultSpectrumBins = 64;
    static constexpr std::size_t MaximumSpectrumBins = 128;
    static constexpr std::chrono::milliseconds MinimumInterval{ 16 };
    static constexpr std::chrono::milliseconds MaximumInterval{ 1000 };

    struct Initialization
    {
        std::function<long(IMMDeviceEnumerator**)> createEnumerator;
        std::function<void*()> createEndpointEvent;
    };
    explicit WidgetAudioAnalysisProvider(Initialization initialization = {});
    ~WidgetAudioAnalysisProvider();

    WidgetAudioAnalysisProvider(const WidgetAudioAnalysisProvider&) = delete;
    WidgetAudioAnalysisProvider& operator=(
        const WidgetAudioAnalysisProvider&) = delete;

    bool Start(std::chrono::milliseconds interval,
        WidgetAudioAnalysisConfiguration configuration = {});
    void SetChangedCallback(ChangedCallback callback);
    void Stop();
    std::optional<WidgetAudioAnalysisDataSnapshot> Snapshot() const;
    bool DrainChanged();
    bool Running() const noexcept;
    bool ResourcesActive() const noexcept;

private:
    void WorkerMain(std::stop_token stopToken);
    void Publish(WidgetAudioAnalysisDataSnapshot snapshot);
    WidgetAudioAnalysisConfiguration Configuration() const;

    mutable std::mutex mutex_;
    WidgetAudioAnalysisPublication publication_;
    WidgetAudioAnalysisConfiguration configuration_;
    ChangedCallback changedCallback_;
    std::jthread worker_;
    std::atomic<std::int64_t> intervalMs_{ 33 };
    std::atomic<bool> resourcesActive_{ false };
    void* stopEvent_ = nullptr;
    Initialization initialization_;
};
}
