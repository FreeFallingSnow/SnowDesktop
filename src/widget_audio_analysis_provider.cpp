#include "widget_audio_analysis_provider.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <new>
#include <numbers>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr double LowerSpectrumFrequency = 20.0;
constexpr double UpperSpectrumFrequency = 8000.0;
constexpr std::size_t MaximumSpectrumWindow = 2048;

class DefaultRenderEndpointNotification final : public IMMNotificationClient
{
public:
    explicit DefaultRenderEndpointNotification(HANDLE eventHandle) noexcept
        : eventHandle_(eventHandle)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interfaceId, void** object) noexcept override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (interfaceId == __uuidof(IUnknown) ||
            interfaceId == __uuidof(IMMNotificationClient))
        {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override
    {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        const ULONG remaining =
            references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow,
        ERole role, LPCWSTR) noexcept override
    {
        if (flow == eRender && role == eMultimedia && eventHandle_)
            SetEvent(eventHandle_);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) noexcept override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) noexcept override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
        LPCWSTR, DWORD) noexcept override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
        LPCWSTR, const PROPERTYKEY) noexcept override
    {
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{ 1 };
    HANDLE eventHandle_ = nullptr;
};

std::int64_t TimestampMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool ValidConfiguration(
    const WidgetAudioAnalysisConfiguration& configuration) noexcept
{
    return (configuration.waveform || configuration.spectrum ||
            configuration.rms || configuration.peak) &&
        (!configuration.waveform ||
            (configuration.waveformPoints >= 16 &&
                configuration.waveformPoints <=
                    WidgetAudioAnalysisProvider::MaximumWaveformPoints)) &&
        (!configuration.spectrum ||
            (configuration.spectrumBins >= 16 &&
                configuration.spectrumBins <=
                    WidgetAudioAnalysisProvider::MaximumSpectrumBins));
}

std::vector<double> Resample(
    const std::vector<double>& values, std::size_t count)
{
    if (values.empty() || count == 0) return {};
    if (values.size() == count) return values;
    std::vector<double> result(count);
    if (count == 1)
    {
        result[0] = values[values.size() / 2];
        return result;
    }
    const double scale = static_cast<double>(values.size() - 1) /
        static_cast<double>(count - 1);
    for (std::size_t index = 0; index < count; ++index)
    {
        const double sourceIndex = static_cast<double>(index) * scale;
        const std::size_t left = static_cast<std::size_t>(sourceIndex);
        const std::size_t right = std::min(left + 1, values.size() - 1);
        const double fraction = sourceIndex - static_cast<double>(left);
        result[index] = values[left] +
            (values[right] - values[left]) * fraction;
    }
    return result;
}

void TransformSpectrum(std::vector<std::complex<double>>& values)
{
    const std::size_t count = values.size();
    for (std::size_t index = 1, reversed = 0; index < count; ++index)
    {
        std::size_t bit = count >> 1;
        for (; reversed & bit; bit >>= 1) reversed ^= bit;
        reversed ^= bit;
        if (index < reversed) std::swap(values[index], values[reversed]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1)
    {
        const double angle = -2.0 * std::numbers::pi /
            static_cast<double>(length);
        const std::complex<double> step(
            std::cos(angle), std::sin(angle));
        for (std::size_t offset = 0; offset < count; offset += length)
        {
            std::complex<double> factor(1.0, 0.0);
            for (std::size_t index = 0; index < length / 2; ++index)
            {
                const auto even = values[offset + index];
                const auto odd = values[offset + index + length / 2] *
                    factor;
                values[offset + index] = even + odd;
                values[offset + index + length / 2] = even - odd;
                factor *= step;
            }
        }
    }
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
    bool deviceChanged,
    const WidgetAudioAnalysisConfiguration& configuration)
{
    WidgetAudioAnalysisDataSnapshot snapshot;
    snapshot.available = true;
    snapshot.warmingUp = false;
    snapshot.endpointId = std::move(endpointId);
    snapshot.sampleRate = sampleRate;
    snapshot.channels = channels;
    snapshot.timestampMs = TimestampMilliseconds();
    snapshot.deviceChanged = deviceChanged;
    snapshot.hasWaveform = configuration.waveform;
    snapshot.hasSpectrum = configuration.spectrum;
    snapshot.hasRms = configuration.rms;
    snapshot.hasPeak = configuration.peak;
    if (configuration.waveform)
        snapshot.waveform.assign(configuration.waveformPoints, 0.0);
    if (configuration.spectrum)
        snapshot.spectrum.assign(configuration.spectrumBins, 0.0);
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
        point < snapshot.waveform.size(); ++point)
    {
        const std::size_t begin = windowStart +
            point * windowSize / snapshot.waveform.size();
        const std::size_t end = windowStart +
            (point + 1) * windowSize / snapshot.waveform.size();
        if (end <= begin) continue;
        double total = 0.0;
        for (std::size_t index = begin; index < end; ++index)
            total += samples[index];
        snapshot.waveform[point] = std::clamp(
            total / static_cast<double>(end - begin), -1.0, 1.0);
    }

    if (configuration.spectrum)
        snapshot.spectrum = ComputeWidgetAudioSpectrum(samples,
            sampleRate, configuration.spectrumBins);
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

std::vector<double> ComputeWidgetAudioSpectrum(
    const std::vector<double>& samples, unsigned int sampleRate,
    std::size_t spectrumBins)
{
    std::vector<double> result(spectrumBins, 0.0);
    if (samples.size() < 2 || sampleRate == 0 || spectrumBins == 0)
        return result;

    const std::size_t available = std::min(
        samples.size(), MaximumSpectrumWindow);
    std::size_t fftSize = 1;
    while ((fftSize << 1) <= available) fftSize <<= 1;
    if (fftSize < 2) return result;

    const std::size_t start = samples.size() - fftSize;
    std::vector<std::complex<double>> transformed(fftSize);
    double windowSum = 0.0;
    for (std::size_t index = 0; index < fftSize; ++index)
    {
        const double window = fftSize > 1
            ? 0.5 - 0.5 * std::cos(
                2.0 * std::numbers::pi * static_cast<double>(index) /
                static_cast<double>(fftSize - 1))
            : 1.0;
        transformed[index] = std::clamp(
            samples[start + index], -1.0, 1.0) * window;
        windowSum += window;
    }
    TransformSpectrum(transformed);

    const std::size_t lastFftBin = fftSize / 2;
    std::vector<double> magnitudes(lastFftBin + 1, 0.0);
    const double scale = windowSum > 0.0 ? 2.0 / windowSum : 0.0;
    for (std::size_t index = 1; index <= lastFftBin; ++index)
    {
        magnitudes[index] = std::clamp(
            std::abs(transformed[index]) * scale, 0.0, 1.0);
    }

    const double frequencyStep = static_cast<double>(sampleRate) /
        static_cast<double>(fftSize);
    const double lower = std::max(
        LowerSpectrumFrequency, frequencyStep);
    const double upper = std::min(
        UpperSpectrumFrequency, static_cast<double>(sampleRate) * 0.5);
    if (upper <= lower) return result;
    const double ratio = upper / lower;

    for (std::size_t band = 0; band < spectrumBins; ++band)
    {
        const double lowerEdge = lower * std::pow(ratio,
            static_cast<double>(band) /
                static_cast<double>(spectrumBins));
        const double upperEdge = lower * std::pow(ratio,
            static_cast<double>(band + 1) /
                static_cast<double>(spectrumBins));
        const std::size_t first = std::max<std::size_t>(1,
            static_cast<std::size_t>(std::ceil(lowerEdge / frequencyStep)));
        const std::size_t last = std::min(lastFftBin,
            static_cast<std::size_t>(std::floor(upperEdge / frequencyStep)));
        if (first <= last)
        {
            double peak = 0.0;
            double total = 0.0;
            for (std::size_t index = first; index <= last; ++index)
            {
                peak = std::max(peak, magnitudes[index]);
                total += magnitudes[index];
            }
            const double average = total /
                static_cast<double>(last - first + 1);
            result[band] = std::clamp(
                peak * 0.7 + average * 0.3, 0.0, 1.0);
            continue;
        }

        const double center = std::sqrt(lowerEdge * upperEdge) /
            frequencyStep;
        const std::size_t left = std::clamp<std::size_t>(
            static_cast<std::size_t>(center), 1, lastFftBin);
        const std::size_t right = std::min(left + 1, lastFftBin);
        const double fraction = std::clamp(
            center - static_cast<double>(left), 0.0, 1.0);
        result[band] = magnitudes[left] +
            (magnitudes[right] - magnitudes[left]) * fraction;
    }
    return result;
}

WidgetAudioAnalysisDataSnapshot ProjectWidgetAudioAnalysisSnapshot(
    const WidgetAudioAnalysisDataSnapshot& snapshot,
    const WidgetAudioAnalysisConfiguration& configuration)
{
    WidgetAudioAnalysisDataSnapshot result = snapshot;
    result.hasWaveform = configuration.waveform;
    result.hasSpectrum = configuration.spectrum;
    result.hasRms = configuration.rms;
    result.hasPeak = configuration.peak;
    result.waveform = configuration.waveform
        ? Resample(snapshot.waveform, configuration.waveformPoints)
        : std::vector<double>{};
    result.spectrum = configuration.spectrum
        ? Resample(snapshot.spectrum, configuration.spectrumBins)
        : std::vector<double>{};
    if (!configuration.rms) result.rms = 0.0;
    if (!configuration.peak) result.peak = 0.0;
    return result;
}

WidgetAudioAnalysisProvider::~WidgetAudioAnalysisProvider()
{
    Stop();
}

bool WidgetAudioAnalysisProvider::Start(
    std::chrono::milliseconds interval,
    WidgetAudioAnalysisConfiguration configuration)
{
    if (!ValidConfiguration(configuration)) return false;
    interval = std::clamp(interval, MinimumInterval, MaximumInterval);
    intervalMs_.store(interval.count());
    {
        std::scoped_lock lock(mutex_);
        configuration_ = configuration;
    }
    if (worker_.joinable()) return true;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return false;
    worker_ = std::jthread([this](std::stop_token token) {
        WorkerMain(token);
    });
    return true;
}

void WidgetAudioAnalysisProvider::SetChangedCallback(
    ChangedCallback callback)
{
    std::scoped_lock lock(mutex_);
    changedCallback_ = std::move(callback);
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
    semanticDebouncer_.Reset();
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

WidgetAudioAnalysisConfiguration
WidgetAudioAnalysisProvider::Configuration() const
{
    std::scoped_lock lock(mutex_);
    return configuration_;
}

void WidgetAudioAnalysisProvider::Publish(
    WidgetAudioAnalysisDataSnapshot snapshot)
{
    ChangedCallback changedCallback;
    {
        std::scoped_lock lock(mutex_);
        snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot), snapshot_,
            semanticDebouncer_);
        snapshot.revision = snapshot_ ? snapshot_->revision + 1 : 1;
        snapshot_ = std::move(snapshot);
        if (!changed_)
            changedCallback = changedCallback_;
        changed_ = true;
    }
    if (changedCallback)
        changedCallback();
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
        ComPtr<IMMNotificationClient> endpointNotification;
        ComPtr<IMMDevice> endpoint;
        ComPtr<IAudioClient> audioClient;
        ComPtr<IAudioCaptureClient> captureClient;
        WAVEFORMATEX* format = nullptr;
        HANDLE eventHandle = nullptr;
        HANDLE endpointChangeEvent = nullptr;
        bool endpointNotificationRegistered = false;
        std::string error;

        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))))
            error = "audioEnumeratorUnavailable";
        if (error.empty())
        {
            endpointChangeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            auto* notification = endpointChangeEvent
                ? new (std::nothrow) DefaultRenderEndpointNotification(
                    endpointChangeEvent)
                : nullptr;
            if (!notification)
                error = "audioEndpointNotificationUnavailable";
            else
            {
                endpointNotification.Attach(notification);
                if (FAILED(enumerator->RegisterEndpointNotificationCallback(
                        endpointNotification.Get())))
                    error = "audioEndpointNotificationUnavailable";
                else
                    endpointNotificationRegistered = true;
            }
        }
        if (error.empty() &&
            (FAILED(enumerator->GetDefaultAudioEndpoint(
                 eRender, eMultimedia, &endpoint)) || !endpoint))
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
            if (endpointNotificationRegistered)
                enumerator->UnregisterEndpointNotificationCallback(
                    endpointNotification.Get());
            if (eventHandle) CloseHandle(eventHandle);
            if (endpointChangeEvent) CloseHandle(endpointChangeEvent);
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
                endpointChangeEvent,
            };
            const DWORD waitResult = WaitForMultipleObjects(
                static_cast<DWORD>(std::size(events)),
                events, FALSE, 100);
            if (waitResult == WAIT_OBJECT_0 + 1)
                break;
            if (waitResult == WAIT_OBJECT_0 + 2)
            {
                reconnect = true;
                break;
            }
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
                    deviceChangedPending, Configuration()));
                deviceChangedPending = false;
                lastPublish = now;
            }
        }

        audioClient->Stop();
        resourcesActive_.store(false);
        if (endpointNotificationRegistered)
            enumerator->UnregisterEndpointNotificationCallback(
                endpointNotification.Get());
        CloseHandle(eventHandle);
        CloseHandle(endpointChangeEvent);
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
