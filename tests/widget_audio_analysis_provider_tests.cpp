#include "widget_audio_analysis_provider.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace
{
using namespace std::chrono_literals;
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

void TestOnDemandCaptureLifecycle()
{
    WidgetAudioAnalysisProvider provider;
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
                    WidgetAudioAnalysisProvider::WaveformPoints &&
                snapshot->spectrum.size() ==
                    WidgetAudioAnalysisProvider::SpectrumBins &&
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
    Check(provider.DrainChanged() && !provider.DrainChanged(),
        "analysis changes must be coalesced and drainable");
    provider.Stop();
    Check(!provider.Running() && !provider.ResourcesActive() &&
            !provider.Snapshot(),
        "stopping the final subscription must release capture and clear data");
}
}

int main()
{
    TestOnDemandCaptureLifecycle();
    std::cout << "widget audio analysis provider tests passed\n";
    return 0;
}
