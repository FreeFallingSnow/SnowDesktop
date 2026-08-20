#pragma once

#include "widget_data_semantic_debounce.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

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
};

WidgetAudioAnalysisDataSnapshot ProjectWidgetAudioAnalysisSnapshot(
    const WidgetAudioAnalysisDataSnapshot& snapshot,
    const WidgetAudioAnalysisConfiguration& configuration);

class WidgetAudioAnalysisProvider
{
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::size_t DefaultWaveformPoints = 128;
    static constexpr std::size_t MaximumWaveformPoints = 256;
    static constexpr std::size_t DefaultSpectrumBins = 64;
    static constexpr std::size_t MaximumSpectrumBins = 128;
    static constexpr std::chrono::milliseconds MinimumInterval{ 16 };
    static constexpr std::chrono::milliseconds MaximumInterval{ 1000 };

    WidgetAudioAnalysisProvider() = default;
    ~WidgetAudioAnalysisProvider();

    WidgetAudioAnalysisProvider(const WidgetAudioAnalysisProvider&) = delete;
    WidgetAudioAnalysisProvider& operator=(
        const WidgetAudioAnalysisProvider&) = delete;

    bool Start(std::chrono::milliseconds interval,
        WidgetAudioAnalysisConfiguration configuration = {});
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
    std::optional<WidgetAudioAnalysisDataSnapshot> snapshot_;
    WidgetDataSemanticDebouncer semanticDebouncer_;
    WidgetAudioAnalysisConfiguration configuration_;
    bool changed_ = false;
    std::jthread worker_;
    std::atomic<std::int64_t> intervalMs_{ 33 };
    std::atomic<bool> resourcesActive_{ false };
    void* stopEvent_ = nullptr;
};
}
