#include "widget_audio_analysis_provider.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
using Microsoft::WRL::ComPtr;

std::int64_t TimestampMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string OpaqueEndpointId(IMMDevice* endpoint)
{
    if (!endpoint) return {};
    LPWSTR raw = nullptr;
    if (FAILED(endpoint->GetId(&raw)) || !raw) return {};
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    for (const wchar_t* cursor = raw; *cursor; ++cursor)
    {
        hash ^= static_cast<std::uint16_t>(*cursor);
        hash *= prime;
    }
    CoTaskMemFree(raw);
    return "audio-output-" + std::to_string(hash);
}

enum class SampleKind
{
    Float32,
    Pcm16,
    Pcm24,
    Pcm32,
    Unsupported,
};

SampleKind DetectSampleKind(const WAVEFORMATEX* format)
{
    if (!format || format->nChannels == 0 || format->nBlockAlign == 0)
        return SampleKind::Unsupported;
    WORD tag = format->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const auto* extensible =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        tag = static_cast<WORD>(extensible->SubFormat.Data1);
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32)
        return SampleKind::Float32;
    if (tag != WAVE_FORMAT_PCM) return SampleKind::Unsupported;
    if (format->wBitsPerSample == 16) return SampleKind::Pcm16;
    if (format->wBitsPerSample == 24) return SampleKind::Pcm24;
    if (format->wBitsPerSample == 32) return SampleKind::Pcm32;
    return SampleKind::Unsupported;
}

double DecodeSample(const BYTE* source, SampleKind kind)
{
    switch (kind)
    {
    case SampleKind::Float32:
    {
        float value = 0.0f;
        std::memcpy(&value, source, sizeof(value));
        return std::clamp(static_cast<double>(value), -1.0, 1.0);
    }
    case SampleKind::Pcm16:
    {
        std::int16_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return static_cast<double>(value) / 32768.0;
    }
    case SampleKind::Pcm24:
    {
        std::int32_t value = static_cast<std::int32_t>(source[0]) |
            (static_cast<std::int32_t>(source[1]) << 8) |
            (static_cast<std::int32_t>(source[2]) << 16);
        if ((value & 0x00800000) != 0) value |= ~0x00ffffff;
        return static_cast<double>(value) / 8388608.0;
    }
    case SampleKind::Pcm32:
    {
        std::int32_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return static_cast<double>(value) / 2147483648.0;
    }
    default:
        return 0.0;
    }
}

void AppendMonoSamples(std::vector<double>& samples, const BYTE* data,
    UINT32 frames, DWORD flags, const WAVEFORMATEX* format,
    SampleKind kind)
{
    const std::size_t channels = format->nChannels;
    const std::size_t bytesPerSample =
        format->nBlockAlign / format->nChannels;
    samples.reserve(samples.size() + frames);
    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double mono = 0.0;
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data)
        {
            const BYTE* frameData = data +
                static_cast<std::size_t>(frame) * format->nBlockAlign;
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                mono += DecodeSample(
                    frameData + channel * bytesPerSample, kind);
            }
            mono /= static_cast<double>(channels);
        }
        samples.push_back(std::clamp(mono, -1.0, 1.0));
    }
    constexpr std::size_t MaxBufferedSamples = 8192;
    if (samples.size() > MaxBufferedSamples)
    {
        samples.erase(samples.begin(),
            samples.end() - static_cast<std::ptrdiff_t>(MaxBufferedSamples));
    }
}

WidgetAudioAnalysisDataSnapshot Analyze(
    const std::vector<double>& samples, std::string endpointId,
    unsigned int sampleRate, unsigned int channels,
    bool deviceChanged)
{
    WidgetAudioAnalysisDataSnapshot snapshot;
    snapshot.available = true;
    snapshot.warmingUp = false;
    snapshot.endpointId = std::move(endpointId);
    snapshot.sampleRate = sampleRate;
    snapshot.channels = channels;
    snapshot.timestampMs = TimestampMilliseconds();
    snapshot.deviceChanged = deviceChanged;
    snapshot.waveform.assign(
        WidgetAudioAnalysisProvider::WaveformPoints, 0.0);
    snapshot.spectrum.assign(
        WidgetAudioAnalysisProvider::SpectrumBins, 0.0);
    if (samples.empty()) return snapshot;

    const std::size_t windowSize = std::min<std::size_t>(
        samples.size(), 2048);
    const std::size_t windowStart = samples.size() - windowSize;
    double sumSquares = 0.0;
    for (std::size_t index = windowStart; index < samples.size(); ++index)
    {
        const double value = samples[index];
        sumSquares += value * value;
        snapshot.peak = std::max(snapshot.peak, std::abs(value));
    }
    snapshot.rms = std::sqrt(sumSquares / static_cast<double>(windowSize));
    snapshot.rms = std::clamp(snapshot.rms, 0.0, 1.0);
    snapshot.peak = std::clamp(snapshot.peak, 0.0, 1.0);
    snapshot.silent = snapshot.peak < 0.0005;

    for (std::size_t point = 0;
        point < WidgetAudioAnalysisProvider::WaveformPoints; ++point)
    {
        const std::size_t begin = windowStart +
            point * windowSize /
                WidgetAudioAnalysisProvider::WaveformPoints;
        const std::size_t end = windowStart +
            (point + 1) * windowSize /
                WidgetAudioAnalysisProvider::WaveformPoints;
        if (end <= begin) continue;
        double total = 0.0;
        for (std::size_t index = begin; index < end; ++index)
            total += samples[index];
        snapshot.waveform[point] = std::clamp(
            total / static_cast<double>(end - begin), -1.0, 1.0);
    }

    const std::size_t fftSize = std::min<std::size_t>(samples.size(), 256);
    const std::size_t fftStart = samples.size() - fftSize;
    if (fftSize > 1)
    {
        for (std::size_t bin = 0;
            bin < WidgetAudioAnalysisProvider::SpectrumBins; ++bin)
        {
            double real = 0.0;
            double imaginary = 0.0;
            for (std::size_t index = 0; index < fftSize; ++index)
            {
                const double window = 0.5 - 0.5 * std::cos(
                    2.0 * std::numbers::pi * static_cast<double>(index) /
                    static_cast<double>(fftSize - 1));
                const double angle = -2.0 * std::numbers::pi *
                    static_cast<double>(bin * index) /
                    static_cast<double>(fftSize);
                const double value = samples[fftStart + index] * window;
                real += value * std::cos(angle);
                imaginary += value * std::sin(angle);
            }
            const double magnitude =
                std::sqrt(real * real + imaginary * imaginary) *
                4.0 / static_cast<double>(fftSize);
            snapshot.spectrum[bin] = std::clamp(magnitude, 0.0, 1.0);
        }
    }
    return snapshot;
}

bool WaitInterruptible(std::stop_token stopToken,
    std::chrono::milliseconds duration)
{
    const auto deadline = WidgetAudioAnalysisProvider::Clock::now() + duration;
    while (!stopToken.stop_requested() &&
        WidgetAudioAnalysisProvider::Clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return stopToken.stop_requested();
}
}

WidgetAudioAnalysisProvider::~WidgetAudioAnalysisProvider()
{
    Stop();
}

bool WidgetAudioAnalysisProvider::Start(std::chrono::milliseconds interval)
{
    interval = std::clamp(interval, MinimumInterval, MaximumInterval);
    intervalMs_.store(interval.count());
    if (worker_.joinable()) return true;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return false;
    worker_ = std::jthread([this](std::stop_token token) {
        WorkerMain(token);
    });
    return true;
}

void WidgetAudioAnalysisProvider::Stop()
{
    if (worker_.joinable())
    {
        worker_.request_stop();
        if (stopEvent_) SetEvent(static_cast<HANDLE>(stopEvent_));
        worker_.join();
    }
    if (stopEvent_)
    {
        CloseHandle(static_cast<HANDLE>(stopEvent_));
        stopEvent_ = nullptr;
    }
    resourcesActive_.store(false);
    std::scoped_lock lock(mutex_);
    snapshot_.reset();
    changed_ = false;
}

std::optional<WidgetAudioAnalysisDataSnapshot>
WidgetAudioAnalysisProvider::Snapshot() const
{
    std::scoped_lock lock(mutex_);
    return snapshot_;
}

bool WidgetAudioAnalysisProvider::DrainChanged()
{
    std::scoped_lock lock(mutex_);
    return std::exchange(changed_, false);
}

bool WidgetAudioAnalysisProvider::Running() const noexcept
{
    return worker_.joinable();
}

bool WidgetAudioAnalysisProvider::ResourcesActive() const noexcept
{
    return resourcesActive_.load();
}

void WidgetAudioAnalysisProvider::Publish(
    WidgetAudioAnalysisDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    snapshot.revision = snapshot_ ? snapshot_->revision + 1 : 1;
    snapshot_ = std::move(snapshot);
    changed_ = true;
}

void WidgetAudioAnalysisProvider::WorkerMain(std::stop_token stopToken)
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(apartment);
    std::string previousEndpointId;
    bool hadEndpoint = false;
    while (!stopToken.stop_requested())
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> endpoint;
        ComPtr<IAudioClient> audioClient;
        ComPtr<IAudioCaptureClient> captureClient;
        WAVEFORMATEX* format = nullptr;
        HANDLE eventHandle = nullptr;
        std::string error;

        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))))
            error = "audioEnumeratorUnavailable";
        else if (FAILED(enumerator->GetDefaultAudioEndpoint(
                     eRender, eMultimedia, &endpoint)) || !endpoint)
            error = "notPresent";
        else if (FAILED(endpoint->Activate(__uuidof(IAudioClient),
                     CLSCTX_INPROC_SERVER, nullptr,
                     reinterpret_cast<void**>(audioClient.GetAddressOf()))) ||
                 !audioClient)
            error = "audioCaptureUnavailable";
        else if (FAILED(audioClient->GetMixFormat(&format)) || !format)
            error = "audioFormatUnavailable";

        const SampleKind kind = error.empty()
            ? DetectSampleKind(format) : SampleKind::Unsupported;
        if (error.empty() && kind == SampleKind::Unsupported)
            error = "audioFormatUnsupported";
        if (error.empty())
        {
            eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!eventHandle)
                error = "audioCaptureEventUnavailable";
        }
        if (error.empty() && FAILED(audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK |
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                0, 0, format, nullptr)))
        {
            error = "audioCaptureInitializationFailed";
        }
        if (error.empty() &&
            FAILED(audioClient->SetEventHandle(eventHandle)))
            error = "audioCaptureEventUnavailable";
        if (error.empty() && FAILED(audioClient->GetService(
                IID_PPV_ARGS(&captureClient))))
            error = "audioCaptureUnavailable";
        if (error.empty() && FAILED(audioClient->Start()))
            error = "audioCaptureStartFailed";

        if (!error.empty())
        {
            WidgetAudioAnalysisDataSnapshot failed;
            failed.warmingUp = false;
            failed.timestampMs = TimestampMilliseconds();
            failed.error = std::move(error);
            Publish(std::move(failed));
            if (eventHandle) CloseHandle(eventHandle);
            if (format) CoTaskMemFree(format);
            if (WaitInterruptible(stopToken, std::chrono::seconds(1))) break;
            continue;
        }

        resourcesActive_.store(true);
        const std::string endpointId = OpaqueEndpointId(endpoint.Get());
        const bool deviceChanged = hadEndpoint &&
            endpointId != previousEndpointId;
        bool deviceChangedPending = deviceChanged;
        previousEndpointId = endpointId;
        hadEndpoint = true;
        WidgetAudioAnalysisDataSnapshot warming;
        warming.available = true;
        warming.warmingUp = true;
        warming.endpointId = endpointId;
        warming.sampleRate = format->nSamplesPerSec;
        warming.channels = format->nChannels;
        warming.timestampMs = TimestampMilliseconds();
        warming.deviceChanged = deviceChanged;
        Publish(std::move(warming));

        std::vector<double> samples;
        auto lastPublish = Clock::now();
        bool reconnect = false;
        while (!stopToken.stop_requested() && !reconnect)
        {
            const HANDLE events[] = {
                eventHandle,
                static_cast<HANDLE>(stopEvent_),
            };
            const DWORD waitResult = WaitForMultipleObjects(
                static_cast<DWORD>(std::size(events)),
                events, FALSE, 100);
            if (waitResult == WAIT_OBJECT_0 + 1)
                break;
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT)
            {
                reconnect = true;
                break;
            }
            UINT32 packetFrames = 0;
            HRESULT packetStatus = captureClient->GetNextPacketSize(
                &packetFrames);
            while (SUCCEEDED(packetStatus) && packetFrames > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                packetStatus = captureClient->GetBuffer(
                    &data, &frames, &flags, nullptr, nullptr);
                if (FAILED(packetStatus)) break;
                AppendMonoSamples(samples, data, frames, flags, format, kind);
                captureClient->ReleaseBuffer(frames);
                packetStatus = captureClient->GetNextPacketSize(
                    &packetFrames);
            }
            if (FAILED(packetStatus))
            {
                reconnect = true;
                break;
            }
            const auto now = Clock::now();
            const auto interval = std::chrono::milliseconds(
                std::clamp<std::int64_t>(intervalMs_.load(),
                    MinimumInterval.count(), MaximumInterval.count()));
            if (now - lastPublish >= interval)
            {
                Publish(Analyze(samples, endpointId,
                    format->nSamplesPerSec, format->nChannels,
                    deviceChangedPending));
                deviceChangedPending = false;
                lastPublish = now;
            }
        }

        audioClient->Stop();
        resourcesActive_.store(false);
        CloseHandle(eventHandle);
        CoTaskMemFree(format);
        if (!stopToken.stop_requested())
        {
            WidgetAudioAnalysisDataSnapshot reconnecting;
            reconnecting.warmingUp = true;
            reconnecting.timestampMs = TimestampMilliseconds();
            reconnecting.error = "audioDeviceChanged";
            reconnecting.deviceChanged = true;
            Publish(std::move(reconnecting));
            if (WaitInterruptible(
                    stopToken, std::chrono::milliseconds(250)))
                break;
        }
    }
    resourcesActive_.store(false);
    if (uninitialize) CoUninitialize();
}
}
