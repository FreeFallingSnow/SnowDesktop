#include "widget/data/widget_audio_analysis_provider.h"
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using snowdesktop::widget_runtime::ProjectWidgetAudioAnalysisSnapshot;
using snowdesktop::widget_runtime::ComputeWidgetAudioSpectrum;
using snowdesktop::widget_runtime::WidgetAudioAnalysisConfiguration;
using snowdesktop::widget_runtime::WidgetAudioAnalysisDataSnapshot;
using snowdesktop::widget_runtime::WidgetAudioAnalysisProvider;

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
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

std::vector<double> SineWave(double frequency, std::size_t sampleCount)
{
    constexpr double SampleRate = 48000.0;
    std::vector<double> samples(sampleCount);
    for (std::size_t index = 0; index < sampleCount; ++index)
    {
        samples[index] = 0.8 * std::sin(2.0 * std::numbers::pi *
            frequency * static_cast<double>(index) / SampleRate);
    }
    return samples;
}

std::size_t PeakIndex(const std::vector<double>& values)
{
    return static_cast<std::size_t>(std::distance(values.begin(),
        std::max_element(values.begin(), values.end())));
}

void TestLogarithmicSpectrumDistribution()
{
    const auto bass = ComputeWidgetAudioSpectrum(
        SineWave(100.0, 4096), 48000, 64);
    const auto mid = ComputeWidgetAudioSpectrum(
        SineWave(1000.0, 4096), 48000, 64);
    const auto treble = ComputeWidgetAudioSpectrum(
        SineWave(4000.0, 4096), 48000, 64);
    const auto bassPeak = PeakIndex(bass);
    const auto midPeak = PeakIndex(mid);
    const auto treblePeak = PeakIndex(treble);
    Check(bass.size() == 64 && mid.size() == 64 && treble.size() == 64,
        "spectrum analysis must return the requested number of bands");
    Check(bassPeak >= 12 && bassPeak <= 24 &&
            midPeak >= 35 && midPeak <= 49 &&
            treblePeak >= 50 && treblePeak <= 62 &&
            bassPeak < midPeak && midPeak < treblePeak,
        "logarithmic bands must spread bass, midrange, and treble across the width");
    for (const auto* values : { &bass, &mid, &treble })
    {
        for (const double value : *values)
            Check(value >= 0.0 && value <= 1.0,
                "logarithmic spectrum bands must remain normalized");
    }
}

void TestOnDemandCaptureLifecycle()
{
    WidgetAudioAnalysisProvider provider;
    std::atomic<int> wakeCount{ 0 };
    provider.SetChangedCallback([&wakeCount] {
        wakeCount.fetch_add(1, std::memory_order_relaxed);
    });
    Check(provider.Start(20ms) && provider.Running(),
        "the first analysis request must start its capture worker");
    Check(WaitFor([&] {
            const auto snapshot = provider.Snapshot();
            return snapshot && snapshot->revision > 0 &&
                (!snapshot->warmingUp || !snapshot->error.empty() ||
                    provider.ResourcesActive());
        }),
        "analysis capture must publish warming, live, or unavailable state");
    Check(WaitFor([&] {
            const auto snapshot = provider.Snapshot();
            return snapshot &&
                (!snapshot->warmingUp || !snapshot->error.empty());
        }),
        "analysis capture must leave warming state or report a stable error");
    const auto snapshot = provider.Snapshot();
    Check(snapshot && snapshot->timestampMs > 0 &&
            (snapshot->available || !snapshot->error.empty()),
        "analysis snapshots must expose data or a stable error");
    if (snapshot && snapshot->available)
    {
        Check(snapshot->endpointId.starts_with("audio-output-") &&
                snapshot->waveform.size() ==
                    WidgetAudioAnalysisProvider::DefaultWaveformPoints &&
                snapshot->spectrum.size() ==
                    WidgetAudioAnalysisProvider::DefaultSpectrumBins &&
                snapshot->rms >= 0.0 && snapshot->rms <= 1.0 &&
                snapshot->peak >= 0.0 && snapshot->peak <= 1.0,
            "live analysis must expose bounded fixed-size derived data");
        for (const double value : snapshot->waveform)
            Check(value >= -1.0 && value <= 1.0,
                "waveform points must stay normalized");
        for (const double value : snapshot->spectrum)
            Check(value >= 0.0 && value <= 1.0,
                "spectrum bins must stay normalized");
    }
    Check(wakeCount.load(std::memory_order_relaxed) == 1,
        "analysis publishes must coalesce into one pending UI wake");
    Check(provider.DrainChanged() && !provider.DrainChanged(),
        "analysis changes must be coalesced and drainable");
    Check(WaitFor([&wakeCount] {
            return wakeCount.load(std::memory_order_relaxed) >= 2;
        }),
        "draining an analysis change must re-arm the next UI wake");
    Check(provider.DrainChanged(),
        "the re-armed UI wake must correspond to a drainable change");
    provider.Stop();
    Check(!provider.Running() && !provider.ResourcesActive() &&
            !provider.Snapshot(),
        "stopping the final subscription must release capture and clear data");
}

void TestSubscriptionProjection()
{
    WidgetAudioAnalysisDataSnapshot snapshot;
    snapshot.available = true;
    snapshot.waveform = { -1.0, -0.5, 0.0, 0.5, 1.0 };
    snapshot.spectrum = { 0.0, 0.25, 0.5, 0.75, 1.0 };
    snapshot.rms = 0.4;
    snapshot.peak = 0.9;
    WidgetAudioAnalysisConfiguration configuration;
    configuration.waveformPoints = 16;
    configuration.spectrum = false;
    configuration.rms = true;
    configuration.peak = false;
    const auto projected = ProjectWidgetAudioAnalysisSnapshot(
        snapshot, configuration);
    Check(projected.hasWaveform && projected.waveform.size() == 16 &&
            projected.waveform.front() == -1.0 &&
            projected.waveform.back() == 1.0 &&
            !projected.hasSpectrum && projected.spectrum.empty() &&
            projected.hasRms && projected.rms == 0.4 &&
            !projected.hasPeak && projected.peak == 0.0,
        "each subscription must receive only its selected derived features");
}

void TestInitializationFailures()
{
    struct Enumerator final : Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMMDeviceEnumerator>
    {
        std::atomic<unsigned>* queries = nullptr;
        HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow, ERole, IMMDevice**) override { ++*queries; return E_FAIL; }
        HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR, IMMDevice**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient*) override { return E_ACCESSDENIED; }
        HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient*) override { return S_OK; }
    };
    for (const int stage : {0, 1, 2})
    {
        std::atomic<unsigned> queries{0};
        WidgetAudioAnalysisProvider::Initialization initialization;
        initialization.createEnumerator = [&](IMMDeviceEnumerator** result) -> long {
            if (stage == 0) { *result = nullptr; return E_ACCESSDENIED; }
            auto instance = Microsoft::WRL::Make<Enumerator>();
            instance->queries = &queries;
            *result = instance.Detach();
            return S_OK;
        };
        if (stage == 1) initialization.createEndpointEvent = []() -> void* { return nullptr; };
        WidgetAudioAnalysisProvider provider(std::move(initialization));
        Check(provider.Start(33ms), "failure injection starts the actual audio worker");
        Check(WaitFor([&] { const auto snapshot = provider.Snapshot(); return snapshot && !snapshot->warmingUp; }),
            "initialization failure publishes a terminal sample without dereferencing an absent endpoint");
        const auto snapshot = provider.Snapshot();
        Check(snapshot && !snapshot->available && snapshot->error == (stage == 0
            ? "audioEnumeratorUnavailable" : "audioEndpointNotificationUnavailable") && queries.load() == 0,
            "an earlier initialization error prevents later endpoint operations and preserves its diagnostic");
        const auto started = std::chrono::steady_clock::now();
        provider.Stop();
        Check(std::chrono::steady_clock::now() - started < 500ms && !provider.ResourcesActive(),
            "failed initialization releases resources and its retry wait is canceled on shutdown");
    }
}
}

int main(int argc, char** argv)
{
    {
        using snowdesktop::widget_runtime::IsRepeatedSilentWidgetAudioSnapshot;
        WidgetAudioAnalysisDataSnapshot quiet;
        quiet.available = true; quiet.warmingUp = false;
        quiet.endpointId = "device-a"; quiet.sampleRate = 48000; quiet.channels = 2;
        quiet.waveform.assign(16, 0.0); quiet.spectrum.assign(16, 0.0);
        auto next = quiet;
        next.timestampMs = 2000; next.revision = 42;
        snowdesktop::widget_runtime::WidgetAudioAnalysisPublication publication;
        Check(publication.Publish(quiet), "first silent state wakes the actual publication path");
        Check(!publication.Publish(next) && publication.DrainChanged(),
            "duplicate silence retains an already queued delivery without posting another wake");
        Check(!publication.Publish(next) && !publication.DrainChanged() &&
            publication.Latest()->timestampMs == 2000 && publication.Latest()->revision == 3,
            "quiet publication keeps fresh readable snapshots without idle redraws");
        auto audible = quiet; audible.silent = false;
        Check(publication.Publish(audible) && publication.DrainChanged(), "resume wakes a quiet subscriber");
        Check(publication.Publish(audible) && publication.DrainChanged(), "audible duplicate samples keep animation delivery");
        Check(publication.Publish(quiet) && publication.DrainChanged(), "first quiet frame clears the last audible frame");
        publication.Reset();
        Check(!publication.Latest() && !publication.DrainChanged(), "stop releases publication and pending state");
        Check(IsRepeatedSilentWidgetAudioSnapshot(quiet, next),
            "fresh timestamps and revisions alone do not redraw identical silence");
        next.silent = false;
        Check(!IsRepeatedSilentWidgetAudioSnapshot(quiet, next), "playback resumption must redraw immediately");
        next = quiet; next.deviceChanged = true;
        Check(!IsRepeatedSilentWidgetAudioSnapshot(quiet, next), "device changes must be delivered even in silence");
        next = quiet; next.endpointId = "device-b";
        Check(!IsRepeatedSilentWidgetAudioSnapshot(quiet, next), "new endpoints must not reuse a silent presentation");
        next = quiet; next.warmingUp = true;
        Check(!IsRepeatedSilentWidgetAudioSnapshot(quiet, next), "reconnection warming state remains observable");
        next = quiet; next.error = "unavailable";
        Check(!IsRepeatedSilentWidgetAudioSnapshot(quiet, next), "errors cannot be hidden by silent deduplication");
        next = quiet; next.hasWaveform = false; next.waveform.clear();
        Check(!IsRepeatedSilentWidgetAudioSnapshot(quiet, next), "subscription feature changes must be delivered");
        next = quiet; next.spectrum[0] = 0.0001;
        Check(!IsRepeatedSilentWidgetAudioSnapshot(quiet, next), "sub-threshold audio values still remain observable");
        next = quiet; quiet.silent = false; next.silent = false;
        Check(!IsRepeatedSilentWidgetAudioSnapshot(quiet, next), "audible repeated windows retain animation deliveries");
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    TestInitializationFailures();
    TestLogarithmicSpectrumDistribution();
    TestSubscriptionProjection();
    if (argc == 2 && std::string_view(argv[1]) == "--initialization-only") return 0;
    TestOnDemandCaptureLifecycle();
    std::cout << "widget audio analysis provider tests passed\n";
    return 0;
}
