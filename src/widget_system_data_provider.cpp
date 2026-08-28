#include "widget_system_data_provider.h"
#include "widget_media_contract.h"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <dxgi1_6.h>
#include <endpointvolume.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <propvarutil.h>
#include <shellscalingapi.h>
#include <wrl/client.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <unordered_map>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::string_view CpuTopic = "system.cpu";
constexpr std::string_view MemoryTopic = "system.memory";
constexpr std::string_view ProcessSummaryTopic = "process.summary";
constexpr std::string_view PowerTopic = "system.power";
constexpr std::string_view NetworkStatusTopic = "system.network.status";
constexpr std::string_view NetworkTrafficTopic = "system.network.traffic";
constexpr std::string_view GpuTopic = "system.gpu";
constexpr std::string_view StorageVolumesTopic = "system.storage.volumes";
constexpr std::string_view StorageIoTopic = "system.storage.io";
constexpr std::string_view DisplayTopologyTopic = "system.display.topology";
constexpr std::string_view DisplayCurrentTopic = "system.display.current";
constexpr std::string_view AudioOutputDefaultTopic = "audio.output.default";
constexpr std::string_view AudioOutputVolumeTopic = "audio.output.volume";
constexpr std::string_view MediaSessionsTopic = "media.sessions";
constexpr std::string_view MediaCurrentTopic = "media.current";
constexpr std::string_view MediaTimelineTopic = "media.timeline";
constexpr std::string_view MediaArtworkTopic = "media.artwork";
constexpr std::size_t MaximumMediaStringBytes = 4096;
constexpr std::uint64_t MaximumArtworkEncodedBytes = 4ull * 1024 * 1024;
constexpr UINT MaximumArtworkSourceDimension = 16384;
constexpr UINT MaximumArtworkOutputDimension = 512;

std::uint64_t FileTimeValue(const FILETIME& value)
{
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

std::int64_t TimestampMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string CpuName()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    wchar_t value[256]{};
    DWORD size = sizeof(value);
    const LONG result = RegQueryValueExW(key,
        L"ProcessorNameString", nullptr, nullptr,
        reinterpret_cast<LPBYTE>(value), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string name(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1,
        name.data(), length, nullptr, nullptr);
    name.resize(static_cast<std::size_t>(length - 1));
    return name;
}

std::string ConnectivityName(
    winrt::Windows::Networking::Connectivity::NetworkConnectivityLevel level)
{
    using Level = winrt::Windows::Networking::Connectivity::
        NetworkConnectivityLevel;
    switch (level)
    {
    case Level::InternetAccess:
        return "internet";
    case Level::LocalAccess:
    case Level::ConstrainedInternetAccess:
        return "local";
    default:
        return "none";
    }
}

int ConnectivityRank(
    winrt::Windows::Networking::Connectivity::NetworkConnectivityLevel level)
{
    const std::string name = ConnectivityName(level);
    if (name == "internet") return 2;
    if (name == "local") return 1;
    return 0;
}

std::string TransportName(std::uint32_t ianaType)
{
    switch (ianaType)
    {
    case IF_TYPE_ETHERNET_CSMACD:
        return "ethernet";
    case IF_TYPE_IEEE80211:
        return "wifi";
    case 243:
    case 244:
        return "cellular";
    case 0:
        return "none";
    default:
        return "other";
    }
}

std::string WideToUtf8(const wchar_t* value)
{
    if (!value || !*value) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1,
        result.data(), length, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

std::string OpaqueProcessId(DWORD processId,
    std::uint64_t creationTime)
{
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](std::uint64_t value) {
        for (unsigned int shift = 0; shift < 64; shift += 8)
        {
            hash ^= static_cast<std::uint8_t>(value >> shift);
            hash *= 1099511628211ull;
        }
    };
    mix(processId);
    mix(creationTime);
    return "process-" + std::to_string(hash);
}

std::uint64_t LuidKey(const LUID& luid)
{
    return (static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(luid.HighPart)) << 32) |
        static_cast<std::uint32_t>(luid.LowPart);
}

std::optional<std::uint64_t> ParseGpuLuid(const wchar_t* instance)
{
    if (!instance) return std::nullopt;
    const wchar_t* marker = wcsstr(instance, L"luid_0x");
    if (!marker) return std::nullopt;
    unsigned long high = 0;
    unsigned long low = 0;
    if (swscanf_s(marker, L"luid_0x%lx_0x%lx", &high, &low) != 2)
        return std::nullopt;
    return (static_cast<std::uint64_t>(high) << 32) |
        static_cast<std::uint32_t>(low);
}

std::string OpaqueVolumeId(const wchar_t* root, bool resolveVolumeName)
{
    wchar_t volumeName[MAX_PATH + 1]{};
    const wchar_t* identity = root;
    if (resolveVolumeName && GetVolumeNameForVolumeMountPointW(
            root, volumeName, static_cast<DWORD>(std::size(volumeName))))
    {
        identity = volumeName;
    }
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    for (const wchar_t* cursor = identity; cursor && *cursor; ++cursor)
    {
        const wchar_t normalized = static_cast<wchar_t>(
            towlower(static_cast<wint_t>(*cursor)));
        hash ^= static_cast<std::uint16_t>(normalized);
        hash *= prime;
    }
    return "volume-" + std::to_string(hash);
}

std::string DriveKind(UINT type)
{
    switch (type)
    {
    case DRIVE_REMOVABLE: return "removable";
    case DRIVE_FIXED: return "fixed";
    case DRIVE_REMOTE: return "network";
    case DRIVE_CDROM: return "optical";
    case DRIVE_RAMDISK: return "ramdisk";
    default: return "unknown";
    }
}

bool IsMappedNetworkRoot(const wchar_t* root)
{
    if (!root || !root[0] || root[1] != L':') return false;
    wchar_t driveName[3]{ root[0], L':', L'\0' };
    wchar_t targets[2048]{};
    const DWORD length = QueryDosDeviceW(
        driveName, targets, static_cast<DWORD>(std::size(targets)));
    if (length == 0) return false;
    for (const wchar_t* target = targets; *target;
        target += wcslen(target) + 1)
    {
        const std::wstring_view path(target);
        if (path.starts_with(L"\\Device\\Mup") ||
            path.find(L"Redirector") != std::wstring_view::npos)
            return true;
    }
    return false;
}

std::wstring LowerWide(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(
                towlower(static_cast<wint_t>(character)));
        });
    return value;
}

std::string OpaqueDisplayId(std::wstring_view deviceName)
{
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    for (const wchar_t character : deviceName)
    {
        hash ^= static_cast<std::uint16_t>(
            towlower(static_cast<wint_t>(character)));
        hash *= prime;
    }
    return "display-" + std::to_string(hash);
}

std::string OpaqueAudioEndpointId(std::wstring_view endpointId)
{
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    for (const wchar_t character : endpointId)
    {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= prime;
    }
    return "audio-output-" + std::to_string(hash);
}

std::string BoundedMediaString(std::wstring_view value)
{
    const std::wstring copy(value);
    std::string result = WideToUtf8(copy.c_str());
    if (result.size() <= MaximumMediaStringBytes) return result;
    std::size_t end = MaximumMediaStringBytes;
    while (end > 0 &&
        (static_cast<unsigned char>(result[end]) & 0xc0) == 0x80)
    {
        --end;
    }
    result.resize(end);
    return result;
}

std::uint64_t MediaArtworkIdentity(std::string_view sessionId,
    std::string_view title, std::string_view artist, std::string_view album)
{
    constexpr std::uint64_t offset = 1469598103934665603ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    const auto mix = [&](std::string_view value) {
        for (const unsigned char character : value)
        {
            hash ^= character;
            hash *= prime;
        }
        hash ^= 0xffu;
        hash *= prime;
    };
    mix(sessionId);
    mix(title);
    mix(artist);
    mix(album);
    return hash;
}

WidgetMediaArtworkDataSnapshot DecodeMediaArtwork(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference&
        reference,
    std::string sessionId, std::int64_t timestampMs)
{
    using namespace winrt::Windows::Storage::Streams;
    WidgetMediaArtworkDataSnapshot snapshot;
    snapshot.sessionId = std::move(sessionId);
    snapshot.timestampMs = timestampMs;
    if (!reference)
    {
        snapshot.error = "notPresent";
        return snapshot;
    }
    try
    {
        const auto stream = reference.OpenReadAsync().get();
        if (!stream)
        {
            snapshot.error = "artworkReadFailed";
            return snapshot;
        }
        const std::uint64_t encodedSize = stream.Size();
        if (encodedSize == 0)
        {
            snapshot.error = "notPresent";
            return snapshot;
        }
        if (encodedSize > MaximumArtworkEncodedBytes ||
            encodedSize > (std::numeric_limits<std::uint32_t>::max)())
        {
            snapshot.error = "artworkTooLarge";
            return snapshot;
        }
        DataReader reader(stream.GetInputStreamAt(0));
        const std::uint32_t expected =
            static_cast<std::uint32_t>(encodedSize);
        if (reader.LoadAsync(expected).get() != expected)
        {
            snapshot.error = "artworkReadFailed";
            return snapshot;
        }
        std::vector<std::uint8_t> encoded(expected);
        reader.ReadBytes(encoded);
        reader.Close();

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory)
        {
            snapshot.error = "artworkDecodeFailed";
            return snapshot;
        }
        Microsoft::WRL::ComPtr<IWICStream> wicStream;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(factory->CreateStream(&wicStream)) || !wicStream ||
            FAILED(wicStream->InitializeFromMemory(encoded.data(), expected)) ||
            FAILED(factory->CreateDecoderFromStream(wicStream.Get(), nullptr,
                WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder ||
            FAILED(decoder->GetFrame(0, &frame)) || !frame)
        {
            snapshot.error = "artworkDecodeFailed";
            return snapshot;
        }
        UINT sourceWidth = 0;
        UINT sourceHeight = 0;
        if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
            sourceWidth == 0 || sourceHeight == 0 ||
            sourceWidth > MaximumArtworkSourceDimension ||
            sourceHeight > MaximumArtworkSourceDimension)
        {
            snapshot.error = "artworkDimensionsInvalid";
            return snapshot;
        }

        const double scale = std::min(1.0,
            static_cast<double>(MaximumArtworkOutputDimension) /
                static_cast<double>(std::max(sourceWidth, sourceHeight)));
        const UINT width = std::max<UINT>(1,
            static_cast<UINT>(std::lround(sourceWidth * scale)));
        const UINT height = std::max<UINT>(1,
            static_cast<UINT>(std::lround(sourceHeight * scale)));
        Microsoft::WRL::ComPtr<IWICBitmapSource> source;
        if (width != sourceWidth || height != sourceHeight)
        {
            Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(factory->CreateBitmapScaler(&scaler)) || !scaler ||
                FAILED(scaler->Initialize(frame.Get(), width, height,
                    WICBitmapInterpolationModeFant)) ||
                FAILED(scaler.As(&source)))
            {
                snapshot.error = "artworkDecodeFailed";
                return snapshot;
            }
        }
        else if (FAILED(frame.As(&source)))
        {
            snapshot.error = "artworkDecodeFailed";
            return snapshot;
        }

        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)) || !converter ||
            FAILED(converter->Initialize(source.Get(),
                GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        {
            snapshot.error = "artworkDecodeFailed";
            return snapshot;
        }
        auto pixels = std::make_shared<WidgetRuntimeImagePixels>();
        pixels->width = width;
        pixels->height = height;
        pixels->stride = width * 4;
        pixels->bgraPremultiplied.resize(
            static_cast<std::size_t>(pixels->stride) * height);
        if (FAILED(converter->CopyPixels(nullptr, pixels->stride,
                static_cast<UINT>(pixels->bgraPremultiplied.size()),
                pixels->bgraPremultiplied.data())))
        {
            snapshot.error = "artworkDecodeFailed";
            return snapshot;
        }
        snapshot.resourceToken = MakeWidgetRuntimeImageToken(
            "media", *pixels);
        snapshot.pixels = std::move(pixels);
        snapshot.available = true;
    }
    catch (...)
    {
        snapshot.error = "artworkReadFailed";
    }
    return snapshot;
}

std::string MediaPlaybackStatus(
    winrt::Windows::Media::Control::
        GlobalSystemMediaTransportControlsSessionPlaybackStatus status)
{
    using Status = winrt::Windows::Media::Control::
        GlobalSystemMediaTransportControlsSessionPlaybackStatus;
    switch (status)
    {
    case Status::Opened: return "open";
    case Status::Changing: return "changing";
    case Status::Stopped: return "stopped";
    case Status::Playing: return "playing";
    case Status::Paused: return "paused";
    default: return "closed";
    }
}

std::int64_t TimeSpanMilliseconds(winrt::Windows::Foundation::TimeSpan value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
}

std::string AudioDeviceState(DWORD state)
{
    if ((state & DEVICE_STATE_ACTIVE) != 0) return "active";
    if ((state & DEVICE_STATE_DISABLED) != 0) return "disabled";
    if ((state & DEVICE_STATE_UNPLUGGED) != 0) return "unplugged";
    if ((state & DEVICE_STATE_NOTPRESENT) != 0) return "notPresent";
    return "unknown";
}

Microsoft::WRL::ComPtr<IMMDevice> DefaultRenderEndpoint(
    std::string& error)
{
    error.clear();
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))))
    {
        error = "audioEnumeratorUnavailable";
        return {};
    }
    Microsoft::WRL::ComPtr<IMMDevice> endpoint;
    const HRESULT status = enumerator->GetDefaultAudioEndpoint(
        eRender, eMultimedia, &endpoint);
    if (FAILED(status) || !endpoint)
    {
        error = status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
            ? "notPresent" : "audioEndpointUnavailable";
        return {};
    }
    return endpoint;
}

std::string OpaqueEndpointId(IMMDevice* endpoint)
{
    if (!endpoint) return {};
    LPWSTR rawId = nullptr;
    if (FAILED(endpoint->GetId(&rawId)) || !rawId) return {};
    const std::string result = OpaqueAudioEndpointId(rawId);
    CoTaskMemFree(rawId);
    return result;
}

struct DisplayTargetMetadata
{
    std::wstring friendlyName;
    bool hdrKnown = false;
    bool hdrSupported = false;
    bool hdrEnabled = false;
};

std::unordered_map<std::wstring, DisplayTargetMetadata>
LoadDisplayTargetMetadata()
{
    std::unordered_map<std::wstring, DisplayTargetMetadata> result;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,
                &pathCount, &modeCount) != ERROR_SUCCESS)
            return result;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const LONG query = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
            &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (query == ERROR_INSUFFICIENT_BUFFER) continue;
        if (query != ERROR_SUCCESS) return result;
        paths.resize(pathCount);
        for (const auto& path : paths)
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type =
                DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
                continue;

            DisplayTargetMetadata metadata;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type =
                DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
                metadata.friendlyName = target.monitorFriendlyDeviceName;

            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 color2{};
            color2.header.type =
                DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
            color2.header.size = sizeof(color2);
            color2.header.adapterId = path.targetInfo.adapterId;
            color2.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&color2.header) == ERROR_SUCCESS)
            {
                metadata.hdrKnown = true;
                metadata.hdrSupported =
                    color2.highDynamicRangeSupported != 0;
                metadata.hdrEnabled =
                    color2.highDynamicRangeUserEnabled != 0;
            }
            else
            {
                DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
                color.header.type =
                    DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
                color.header.size = sizeof(color);
                color.header.adapterId = path.targetInfo.adapterId;
                color.header.id = path.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(
                        &color.header) == ERROR_SUCCESS)
                {
                    metadata.hdrKnown = true;
                    metadata.hdrSupported =
                        color.advancedColorSupported != 0;
                    metadata.hdrEnabled =
                        color.advancedColorEnabled != 0;
                }
            }
            result[LowerWide(source.viewGdiDeviceName)] =
                std::move(metadata);
        }
        return result;
    }
    return result;
}

std::string DisplayOrientation(DWORD orientation)
{
    switch (orientation)
    {
    case DMDO_DEFAULT: return "landscape";
    case DMDO_90: return "portrait";
    case DMDO_180: return "landscapeFlipped";
    case DMDO_270: return "portraitFlipped";
    default: return "unknown";
    }
}
}

std::optional<WidgetDisplayDataSnapshot> MatchDisplayByPixelBounds(
    const WidgetDisplayTopologyDataSnapshot& topology,
    const WidgetDisplayPixelRectDataSnapshot& bounds)
{
    const auto display = std::find_if(
        topology.displays.begin(), topology.displays.end(),
        [&](const auto& candidate) {
            return candidate.pixelBounds.x == bounds.x &&
                candidate.pixelBounds.y == bounds.y &&
                candidate.pixelBounds.width == bounds.width &&
                candidate.pixelBounds.height == bounds.height;
        });
    return display == topology.displays.end()
        ? std::nullopt
        : std::optional<WidgetDisplayDataSnapshot>(*display);
}

bool WidgetNetworkStatusDebouncer::SemanticallyEqual(
    const WidgetNetworkStatusDataSnapshot& left,
    const WidgetNetworkStatusDataSnapshot& right) noexcept
{
    return left.available == right.available &&
        left.connectivity == right.connectivity &&
        left.transport == right.transport &&
        left.costKnown == right.costKnown &&
        left.metered == right.metered &&
        left.roaming == right.roaming &&
        left.overLimit == right.overLimit &&
        left.error == right.error;
}

WidgetNetworkStatusDataSnapshot WidgetNetworkStatusDebouncer::Push(
    WidgetNetworkStatusDataSnapshot snapshot)
{
    if (!stable_)
    {
        stable_ = snapshot;
        pending_.reset();
        pendingConfirmations_ = 0;
        return snapshot;
    }

    if (SemanticallyEqual(*stable_, snapshot))
    {
        stable_ = snapshot;
        pending_.reset();
        pendingConfirmations_ = 0;
        return snapshot;
    }

    if (pending_ && SemanticallyEqual(*pending_, snapshot))
    {
        ++pendingConfirmations_;
    }
    else
    {
        pending_ = snapshot;
        pendingConfirmations_ = 1;
    }

    if (pendingConfirmations_ >= RequiredConfirmations)
    {
        stable_ = snapshot;
        pending_.reset();
        pendingConfirmations_ = 0;
        return snapshot;
    }

    WidgetNetworkStatusDataSnapshot held = *stable_;
    held.timestampMs = snapshot.timestampMs;
    held.revision = snapshot.revision;
    return held;
}

void WidgetNetworkStatusDebouncer::Reset() noexcept
{
    stable_.reset();
    pending_.reset();
    pendingConfirmations_ = 0;
}

WidgetSystemDataProvider::~WidgetSystemDataProvider()
{
    StopAll();
}

bool WidgetSystemDataProvider::SupportsTopic(
    std::string_view topic) noexcept
{
    return topic == CpuTopic || topic == MemoryTopic ||
        topic == ProcessSummaryTopic ||
        topic == PowerTopic || topic == NetworkStatusTopic ||
        topic == NetworkTrafficTopic || topic == GpuTopic ||
        topic == StorageVolumesTopic || topic == StorageIoTopic ||
        topic == DisplayTopologyTopic || topic == DisplayCurrentTopic ||
        topic == AudioOutputDefaultTopic || topic == AudioOutputVolumeTopic ||
        topic == MediaSessionsTopic || topic == MediaCurrentTopic ||
        topic == MediaTimelineTopic || topic == MediaArtworkTopic;
}

bool WidgetSystemDataProvider::StartTopic(
    std::string_view topic, std::chrono::milliseconds interval)
{
    if (!SupportsTopic(topic) || interval < MinimumInterval ||
        interval > MaximumInterval)
        return false;

    bool startWorker = false;
    {
        std::scoped_lock lock(mutex_);
        const auto now = Clock::now();
        const std::string key(topic);
        auto existing = schedules_.find(key);
        if (existing == schedules_.end())
        {
            schedules_.emplace(key, TopicSchedule{ interval, now });
            if (topic == CpuTopic) resetCpuBaseline_.store(true);
            if (topic == ProcessSummaryTopic)
                resetProcessBaseline_.store(true);
            if (topic == NetworkTrafficTopic)
                resetNetworkBaseline_.store(true);
            if (topic == GpuTopic)
            {
                resetGpuBaseline_.store(true);
                closeGpuRequested_.store(false);
            }
            if (topic == StorageIoTopic)
            {
                resetStorageIoBaseline_.store(true);
                closeStorageIoRequested_.store(false);
            }
        }
        else
        {
            existing->second.interval = interval;
            existing->second.due = std::min(
                existing->second.due, now + interval);
        }
        ++configurationGeneration_;
        startWorker = !worker_.joinable();
    }
    if (startWorker)
    {
        worker_ = std::jthread([this](std::stop_token token) {
            WorkerMain(token);
        });
    }
    condition_.notify_all();
    return true;
}

bool WidgetSystemDataProvider::StopTopic(std::string_view topic)
{
    bool removed = false;
    bool stopWorker = false;
    {
        std::scoped_lock lock(mutex_);
        removed = schedules_.erase(std::string(topic)) > 0;
        if (!removed) return false;
        semanticDebouncers_.erase(std::string(topic));
        if (topic == MediaArtworkTopic) mediaArtwork_.reset();
        if (topic == ProcessSummaryTopic) processSummary_.reset();
        if (topic == CpuTopic) resetCpuBaseline_.store(true);
        if (topic == ProcessSummaryTopic)
            resetProcessBaseline_.store(true);
        if (topic == NetworkTrafficTopic)
            resetNetworkBaseline_.store(true);
        if (topic == NetworkStatusTopic)
            networkStatusDebouncer_.Reset();
        if (topic == GpuTopic)
            closeGpuRequested_.store(true);
        if (topic == StorageIoTopic)
            closeStorageIoRequested_.store(true);
        ++configurationGeneration_;
        stopWorker = schedules_.empty();
    }
    condition_.notify_all();
    if (stopWorker && worker_.joinable())
    {
        worker_.request_stop();
        condition_.notify_all();
        worker_.join();
    }
    return true;
}

void WidgetSystemDataProvider::StopAll()
{
    {
        std::scoped_lock lock(mutex_);
        schedules_.clear();
        changedTopics_.clear();
        semanticDebouncers_.clear();
        networkStatusDebouncer_.Reset();
        mediaArtwork_.reset();
        processSummary_.reset();
        ++configurationGeneration_;
    }
    resetCpuBaseline_.store(true);
    resetProcessBaseline_.store(true);
    resetNetworkBaseline_.store(true);
    resetGpuBaseline_.store(true);
    closeGpuRequested_.store(true);
    resetStorageIoBaseline_.store(true);
    closeStorageIoRequested_.store(true);
    if (worker_.joinable())
    {
        worker_.request_stop();
        condition_.notify_all();
        worker_.join();
    }
    previousIdle_ = 0;
    previousKernel_ = 0;
    previousUser_ = 0;
    previousProcessCpuTimes_.clear();
    previousProcessSample_ = {};
    previousReceived_ = 0;
    previousSent_ = 0;
    previousNetworkSample_ = {};
    CloseGpuQuery();
    CloseStorageIoQuery();
}

std::optional<WidgetCpuDataSnapshot>
WidgetSystemDataProvider::Cpu() const
{
    std::scoped_lock lock(mutex_);
    return cpu_;
}

std::optional<WidgetMemoryDataSnapshot>
WidgetSystemDataProvider::Memory() const
{
    std::scoped_lock lock(mutex_);
    return memory_;
}

std::optional<WidgetProcessSummaryDataSnapshot>
WidgetSystemDataProvider::ProcessSummary() const
{
    std::scoped_lock lock(mutex_);
    return processSummary_;
}

std::optional<WidgetPowerDataSnapshot>
WidgetSystemDataProvider::Power() const
{
    std::scoped_lock lock(mutex_);
    return power_;
}

std::optional<WidgetNetworkStatusDataSnapshot>
WidgetSystemDataProvider::NetworkStatus() const
{
    std::scoped_lock lock(mutex_);
    return networkStatus_;
}

std::optional<WidgetNetworkTrafficDataSnapshot>
WidgetSystemDataProvider::NetworkTraffic() const
{
    std::scoped_lock lock(mutex_);
    return networkTraffic_;
}

std::optional<WidgetGpuDataSnapshot>
WidgetSystemDataProvider::Gpu() const
{
    std::scoped_lock lock(mutex_);
    return gpu_;
}

std::optional<WidgetStorageVolumesDataSnapshot>
WidgetSystemDataProvider::StorageVolumes() const
{
    std::scoped_lock lock(mutex_);
    return storageVolumes_;
}

std::optional<WidgetStorageIoDataSnapshot>
WidgetSystemDataProvider::StorageIo() const
{
    std::scoped_lock lock(mutex_);
    return storageIo_;
}

std::optional<WidgetDisplayTopologyDataSnapshot>
WidgetSystemDataProvider::DisplayTopology() const
{
    std::scoped_lock lock(mutex_);
    return displayTopology_;
}

std::optional<WidgetDisplayTopologyDataSnapshot>
WidgetSystemDataProvider::DisplayCurrent() const
{
    std::scoped_lock lock(mutex_);
    return displayCurrent_;
}

std::optional<WidgetAudioOutputDefaultDataSnapshot>
WidgetSystemDataProvider::AudioOutputDefault() const
{
    std::scoped_lock lock(mutex_);
    return audioOutputDefault_;
}

std::optional<WidgetAudioOutputVolumeDataSnapshot>
WidgetSystemDataProvider::AudioOutputVolume() const
{
    std::scoped_lock lock(mutex_);
    return audioOutputVolume_;
}

std::optional<WidgetMediaSessionsDataSnapshot>
WidgetSystemDataProvider::MediaSessions() const
{
    std::scoped_lock lock(mutex_);
    return mediaSessions_;
}

std::optional<WidgetMediaCurrentDataSnapshot>
WidgetSystemDataProvider::MediaCurrent() const
{
    std::scoped_lock lock(mutex_);
    return mediaCurrent_;
}

std::optional<WidgetMediaTimelineDataSnapshot>
WidgetSystemDataProvider::MediaTimeline() const
{
    std::scoped_lock lock(mutex_);
    return mediaTimeline_;
}

std::optional<WidgetMediaArtworkDataSnapshot>
WidgetSystemDataProvider::MediaArtwork() const
{
    std::scoped_lock lock(mutex_);
    return mediaArtwork_;
}

std::vector<std::string>
WidgetSystemDataProvider::DrainChangedTopics()
{
    std::scoped_lock lock(mutex_);
    std::vector<std::string> result(
        changedTopics_.begin(), changedTopics_.end());
    changedTopics_.clear();
    std::sort(result.begin(), result.end());
    return result;
}

bool WidgetSystemDataProvider::Running() const noexcept
{
    return worker_.joinable();
}

bool WidgetSystemDataProvider::GpuResourcesActive() const noexcept
{
    return gpuResourcesActive_.load();
}

bool WidgetSystemDataProvider::StorageIoResourcesActive() const noexcept
{
    return storageIoResourcesActive_.load();
}

std::size_t WidgetSystemDataProvider::ActiveTopicCount() const
{
    std::scoped_lock lock(mutex_);
    return schedules_.size();
}

void WidgetSystemDataProvider::WorkerMain(std::stop_token stopToken)
{
    bool apartmentInitialized = false;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;
    }
    catch (...)
    {
    }
    while (!stopToken.stop_requested())
    {
        if (closeGpuRequested_.exchange(false))
            CloseGpuQuery();
        if (closeStorageIoRequested_.exchange(false))
            CloseStorageIoQuery();
        std::vector<std::string> dueTopics;
        {
            std::unique_lock lock(mutex_);
            while (schedules_.empty() && !stopToken.stop_requested())
            {
                const auto generation = configurationGeneration_;
                condition_.wait(lock, [&] {
                    return stopToken.stop_requested() ||
                        configurationGeneration_ != generation;
                });
            }
            if (stopToken.stop_requested()) break;

            const auto now = Clock::now();
            auto nextDue = Clock::time_point::max();
            for (auto& [topic, schedule] : schedules_)
            {
                if (now >= schedule.due)
                {
                    dueTopics.push_back(topic);
                    schedule.due = now + schedule.interval;
                }
                nextDue = std::min(nextDue, schedule.due);
            }
            if (dueTopics.empty())
            {
                const auto generation = configurationGeneration_;
                condition_.wait_until(lock, nextDue, [&] {
                    return stopToken.stop_requested() ||
                        configurationGeneration_ != generation;
                });
                continue;
            }
        }

        const bool mediaDue = std::any_of(
            dueTopics.begin(), dueTopics.end(), [](const auto& topic) {
                return topic == MediaSessionsTopic ||
                    topic == MediaCurrentTopic ||
                    topic == MediaTimelineTopic ||
                    topic == MediaArtworkTopic;
            });
        for (const std::string& topic : dueTopics)
        {
            if (stopToken.stop_requested()) break;
            if (topic == CpuTopic)
                PublishCpu(SampleCpu());
            else if (topic == MemoryTopic)
                PublishMemory(SampleMemory());
            else if (topic == ProcessSummaryTopic)
                PublishProcessSummary(SampleProcessSummary());
            else if (topic == PowerTopic)
                PublishPower(SamplePower());
            else if (topic == NetworkStatusTopic)
                PublishNetworkStatus(SampleNetworkStatus());
            else if (topic == NetworkTrafficTopic)
                PublishNetworkTraffic(SampleNetworkTraffic());
            else if (topic == GpuTopic)
                PublishGpu(SampleGpu());
            else if (topic == StorageVolumesTopic)
                PublishStorageVolumes(SampleStorageVolumes());
            else if (topic == StorageIoTopic)
                PublishStorageIo(SampleStorageIo());
            else if (topic == DisplayTopologyTopic)
                PublishDisplayTopology(SampleDisplayTopology());
            else if (topic == DisplayCurrentTopic)
                PublishDisplayCurrent(SampleDisplayTopology());
            else if (topic == AudioOutputDefaultTopic)
                PublishAudioOutputDefault(SampleAudioOutputDefault());
            else if (topic == AudioOutputVolumeTopic)
                PublishAudioOutputVolume(SampleAudioOutputVolume());
        }
        if (mediaDue && !stopToken.stop_requested())
        {
            const bool artworkDue = std::find(
                dueTopics.begin(), dueTopics.end(),
                MediaArtworkTopic) != dueTopics.end();
            const auto snapshot = SampleMediaSessions(artworkDue);
            if (std::find(dueTopics.begin(), dueTopics.end(),
                    MediaSessionsTopic) != dueTopics.end())
                PublishMediaSessions(snapshot);
            if (std::find(dueTopics.begin(), dueTopics.end(),
                    MediaCurrentTopic) != dueTopics.end())
                PublishMediaCurrent(snapshot);
            if (std::find(dueTopics.begin(), dueTopics.end(),
                    MediaTimelineTopic) != dueTopics.end())
                PublishMediaTimeline(snapshot);
            if (artworkDue)
                PublishMediaArtwork(snapshot);
        }
    }
    CloseGpuQuery();
    CloseStorageIoQuery();
    if (apartmentInitialized)
        winrt::uninit_apartment();
}

WidgetCpuDataSnapshot WidgetSystemDataProvider::SampleCpu()
{
    WidgetCpuDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    snapshot.logicalProcessors = systemInfo.dwNumberOfProcessors;
    static const std::string cpuName = CpuName();
    snapshot.name = cpuName;

    if (resetCpuBaseline_.exchange(false))
    {
        previousIdle_ = 0;
        previousKernel_ = 0;
        previousUser_ = 0;
    }
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user))
    {
        snapshot.error = "CPU sampling failed";
        return snapshot;
    }
    const auto idleValue = FileTimeValue(idle);
    const auto kernelValue = FileTimeValue(kernel);
    const auto userValue = FileTimeValue(user);
    if (previousKernel_ != 0)
    {
        const auto totalDelta =
            (kernelValue - previousKernel_) +
            (userValue - previousUser_);
        const auto idleDelta = idleValue - previousIdle_;
        if (totalDelta > 0 && totalDelta >= idleDelta)
        {
            snapshot.available = true;
            snapshot.warmingUp = false;
            snapshot.usagePercent = std::clamp(
                100.0 * static_cast<double>(totalDelta - idleDelta) /
                    static_cast<double>(totalDelta),
                0.0, 100.0);
        }
    }
    previousIdle_ = idleValue;
    previousKernel_ = kernelValue;
    previousUser_ = userValue;
    return snapshot;
}

WidgetMemoryDataSnapshot WidgetSystemDataProvider::SampleMemory()
{
    WidgetMemoryDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    MEMORYSTATUSEX status{ sizeof(status) };
    if (!GlobalMemoryStatusEx(&status))
    {
        snapshot.error = "Memory sampling failed";
        return snapshot;
    }
    snapshot.totalBytes = status.ullTotalPhys;
    snapshot.freeBytes = status.ullAvailPhys;
    snapshot.usedBytes = snapshot.totalBytes - snapshot.freeBytes;
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (!K32GetPerformanceInfo(&performance, sizeof(performance)))
    {
        snapshot.error = "Memory commit sampling failed";
        return snapshot;
    }
    const std::uint64_t pageSize = static_cast<std::uint64_t>(
        performance.PageSize);
    snapshot.commitLimitBytes = static_cast<std::uint64_t>(
        performance.CommitLimit) * pageSize;
    snapshot.commitUsedBytes = static_cast<std::uint64_t>(
        performance.CommitTotal) * pageSize;
    snapshot.commitAvailableBytes = snapshot.commitLimitBytes >=
            snapshot.commitUsedBytes
        ? snapshot.commitLimitBytes - snapshot.commitUsedBytes : 0;
    snapshot.usagePercent = static_cast<double>(status.dwMemoryLoad);
    snapshot.available = true;
    return snapshot;
}

WidgetProcessSummaryDataSnapshot
WidgetSystemDataProvider::SampleProcessSummary()
{
    WidgetProcessSummaryDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    const auto sampleTime = Clock::now();
    if (resetProcessBaseline_.exchange(false))
    {
        previousProcessCpuTimes_.clear();
        previousProcessSample_ = {};
    }

    const HANDLE processSnapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS, 0);
    if (processSnapshot == INVALID_HANDLE_VALUE)
    {
        snapshot.warmingUp = false;
        snapshot.error = "processEnumerationFailed";
        return snapshot;
    }

    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    const double logicalProcessors = static_cast<double>(
        std::max<DWORD>(1, systemInfo.dwNumberOfProcessors));
    const bool hasBaseline =
        previousProcessSample_.time_since_epoch().count() != 0 &&
        sampleTime > previousProcessSample_;
    const double elapsedHundredNanoseconds = hasBaseline
        ? std::chrono::duration<double>(
            sampleTime - previousProcessSample_).count() *
            10000000.0 * logicalProcessors
        : 0.0;
    std::unordered_map<std::string, std::uint64_t> nextCpuTimes;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(processSnapshot, &entry))
    {
        CloseHandle(processSnapshot);
        snapshot.warmingUp = false;
        snapshot.error = "processEnumerationFailed";
        return snapshot;
    }
    do
    {
        const HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE, entry.th32ProcessID);
        if (!process) continue;

        FILETIME creation{}, exit{}, kernel{}, user{};
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        const bool timesAvailable = GetProcessTimes(process, &creation,
            &exit, &kernel, &user) != FALSE;
        const bool memoryAvailable = K32GetProcessMemoryInfo(process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory)) != FALSE;
        CloseHandle(process);
        if (!timesAvailable || !memoryAvailable) continue;

        const std::string name = WideToUtf8(entry.szExeFile);
        if (name.empty()) continue;
        const std::uint64_t creationValue = FileTimeValue(creation);
        const std::string id = OpaqueProcessId(
            entry.th32ProcessID, creationValue);
        const std::uint64_t cpuTime =
            FileTimeValue(kernel) + FileTimeValue(user);
        double cpuPercent = 0.0;
        if (hasBaseline && elapsedHundredNanoseconds > 0.0)
        {
            const auto previous = previousProcessCpuTimes_.find(id);
            if (previous != previousProcessCpuTimes_.end() &&
                cpuTime >= previous->second)
            {
                cpuPercent = std::clamp(
                    100.0 * static_cast<double>(
                        cpuTime - previous->second) /
                        elapsedHundredNanoseconds,
                    0.0, 100.0);
            }
        }
        nextCpuTimes.emplace(id, cpuTime);
        snapshot.processes.push_back({ id, name, cpuPercent,
            static_cast<std::uint64_t>(memory.WorkingSetSize),
            static_cast<std::uint64_t>(memory.PrivateUsage) });
    } while (Process32NextW(processSnapshot, &entry));
    CloseHandle(processSnapshot);

    previousProcessCpuTimes_ = std::move(nextCpuTimes);
    previousProcessSample_ = sampleTime;
    snapshot.observedCount = snapshot.processes.size();
    std::sort(snapshot.processes.begin(), snapshot.processes.end(),
        [](const auto& left, const auto& right) {
            if (left.cpuPercent != right.cpuPercent)
                return left.cpuPercent > right.cpuPercent;
            if (left.privateBytes != right.privateBytes)
                return left.privateBytes > right.privateBytes;
            if (left.workingSetBytes != right.workingSetBytes)
                return left.workingSetBytes > right.workingSetBytes;
            return left.id < right.id;
        });
    if (snapshot.processes.size() > MaximumProcessSummaryEntries)
    {
        snapshot.processes.resize(MaximumProcessSummaryEntries);
        snapshot.truncated = true;
    }
    snapshot.available = true;
    snapshot.warmingUp = !hasBaseline;
    return snapshot;
}

WidgetPowerDataSnapshot WidgetSystemDataProvider::SamplePower()
{
    WidgetPowerDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status))
    {
        snapshot.error = "Power sampling failed";
        return snapshot;
    }
    snapshot.acPower = status.ACLineStatus == 1;
    snapshot.charging = (status.BatteryFlag & 8) != 0;
    snapshot.saver = status.SystemStatusFlag != 0;
    if (status.BatteryFlag == 128)
    {
        snapshot.error = "notPresent";
        return snapshot;
    }
    if (status.BatteryLifePercent == 255)
    {
        snapshot.error = "temporarilyUnavailable";
        return snapshot;
    }
    snapshot.available = true;
    snapshot.batteryPercent = std::clamp(
        static_cast<double>(status.BatteryLifePercent), 0.0, 100.0);
    if (status.BatteryLifeTime != static_cast<DWORD>(-1))
    {
        snapshot.estimatedRemainingSeconds =
            static_cast<std::int64_t>(status.BatteryLifeTime);
    }
    return snapshot;
}

WidgetNetworkStatusDataSnapshot
WidgetSystemDataProvider::SampleNetworkStatus()
{
    using namespace winrt::Windows::Networking::Connectivity;
    WidgetNetworkStatusDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    try
    {
        ConnectionProfile selected{ nullptr };
        int selectedRank = -1;
        for (const auto& profile : NetworkInformation::GetConnectionProfiles())
        {
            const int rank = ConnectivityRank(
                profile.GetNetworkConnectivityLevel());
            if (rank > selectedRank)
            {
                selected = profile;
                selectedRank = rank;
            }
        }
        snapshot.available = true;
        if (!selected)
            return snapshot;

        snapshot.connectivity = ConnectivityName(
            selected.GetNetworkConnectivityLevel());
        if (const auto adapter = selected.NetworkAdapter())
            snapshot.transport = TransportName(adapter.IanaInterfaceType());
        const auto cost = selected.GetConnectionCost();
        if (cost)
        {
            const auto type = cost.NetworkCostType();
            snapshot.costKnown = type != NetworkCostType::Unknown;
            snapshot.metered = type == NetworkCostType::Fixed ||
                type == NetworkCostType::Variable;
            snapshot.roaming = cost.Roaming();
            snapshot.overLimit = cost.OverDataLimit();
        }
    }
    catch (const winrt::hresult_error& error)
    {
        snapshot.error = "Network status sampling failed: " +
            winrt::to_string(error.message());
    }
    catch (...)
    {
        snapshot.error = "Network status sampling failed";
    }
    return snapshot;
}

WidgetNetworkTrafficDataSnapshot
WidgetSystemDataProvider::SampleNetworkTraffic()
{
    WidgetNetworkTrafficDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    const auto sampleTime = Clock::now();
    if (resetNetworkBaseline_.exchange(false))
    {
        previousReceived_ = 0;
        previousSent_ = 0;
        previousNetworkSample_ = {};
    }

    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table)
    {
        snapshot.error = "Network traffic sampling failed";
        return snapshot;
    }
    snapshot.available = true;
    for (ULONG index = 0; index < table->NumEntries; ++index)
    {
        const auto& row = table->Table[index];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK ||
            row.OperStatus != IfOperStatusUp ||
            row.MediaConnectState != MediaConnectStateConnected)
            continue;
        snapshot.connected = true;
        snapshot.receivedBytes += row.InOctets;
        snapshot.sentBytes += row.OutOctets;
    }
    FreeMibTable(table);

    if (previousNetworkSample_.time_since_epoch().count() != 0)
    {
        const double seconds = std::chrono::duration<double>(
            sampleTime - previousNetworkSample_).count();
        if (seconds > 0.0 &&
            snapshot.receivedBytes >= previousReceived_ &&
            snapshot.sentBytes >= previousSent_)
        {
            snapshot.warmingUp = false;
            snapshot.downloadBytesPerSecond =
                static_cast<std::uint64_t>(
                    (snapshot.receivedBytes - previousReceived_) / seconds);
            snapshot.uploadBytesPerSecond =
                static_cast<std::uint64_t>(
                    (snapshot.sentBytes - previousSent_) / seconds);
        }
    }
    previousReceived_ = snapshot.receivedBytes;
    previousSent_ = snapshot.sentBytes;
    previousNetworkSample_ = sampleTime;
    return snapshot;
}

bool WidgetSystemDataProvider::InitializeGpuQuery()
{
    CloseGpuQuery();
    HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS)
        return false;
    HCOUNTER counter = nullptr;
    HCOUNTER dedicatedUsageCounter = nullptr;
    HCOUNTER sharedUsageCounter = nullptr;
    if (PdhAddEnglishCounterW(query,
            L"\\GPU Engine(*)\\Utilization Percentage",
            0, &counter) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(query,
            L"\\GPU Adapter Memory(*)\\Dedicated Usage",
            0, &dedicatedUsageCounter) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(query,
            L"\\GPU Adapter Memory(*)\\Shared Usage",
            0, &sharedUsageCounter) != ERROR_SUCCESS)
    {
        if (sharedUsageCounter) PdhRemoveCounter(sharedUsageCounter);
        if (dedicatedUsageCounter) PdhRemoveCounter(dedicatedUsageCounter);
        if (counter) PdhRemoveCounter(counter);
        PdhCloseQuery(query);
        return false;
    }
    if (PdhCollectQueryData(query) != ERROR_SUCCESS)
    {
        PdhRemoveCounter(sharedUsageCounter);
        PdhRemoveCounter(dedicatedUsageCounter);
        PdhRemoveCounter(counter);
        PdhCloseQuery(query);
        return false;
    }
    gpuQuery_ = query;
    gpuUtilizationCounter_ = counter;
    gpuDedicatedUsageCounter_ = dedicatedUsageCounter;
    gpuSharedUsageCounter_ = sharedUsageCounter;
    gpuResourcesActive_.store(true);
    return true;
}

void WidgetSystemDataProvider::CloseGpuQuery()
{
    if (gpuSharedUsageCounter_)
    {
        PdhRemoveCounter(
            reinterpret_cast<HCOUNTER>(gpuSharedUsageCounter_));
        gpuSharedUsageCounter_ = nullptr;
    }
    if (gpuDedicatedUsageCounter_)
    {
        PdhRemoveCounter(
            reinterpret_cast<HCOUNTER>(gpuDedicatedUsageCounter_));
        gpuDedicatedUsageCounter_ = nullptr;
    }
    if (gpuUtilizationCounter_)
    {
        PdhRemoveCounter(
            reinterpret_cast<HCOUNTER>(gpuUtilizationCounter_));
        gpuUtilizationCounter_ = nullptr;
    }
    if (gpuQuery_)
    {
        PdhCloseQuery(reinterpret_cast<HQUERY>(gpuQuery_));
        gpuQuery_ = nullptr;
    }
    gpuResourcesActive_.store(false);
}

WidgetGpuDataSnapshot WidgetSystemDataProvider::SampleGpu()
{
    struct AdapterEntry
    {
        std::uint64_t luid = 0;
        WidgetGpuAdapterDataSnapshot snapshot;
    };

    WidgetGpuDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    std::vector<AdapterEntry> adapters;
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        snapshot.error = "GPU adapter enumeration failed";
        snapshot.warmingUp = false;
        return snapshot;
    }
    for (UINT index = 0; ; ++index)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        if (!adapter) continue;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            continue;

        AdapterEntry entry;
        entry.luid = LuidKey(description.AdapterLuid);
        entry.snapshot.id = "adapter-" +
            std::to_string(adapters.size() + 1);
        entry.snapshot.name = WideToUtf8(description.Description);
        entry.snapshot.dedicatedMemoryBytes =
            description.DedicatedVideoMemory;
        entry.snapshot.sharedMemoryBytes =
            description.SharedSystemMemory;
        adapters.push_back(std::move(entry));
    }
    if (adapters.empty())
    {
        snapshot.error = "notPresent";
        snapshot.warmingUp = false;
        return snapshot;
    }
    snapshot.available = true;

    const bool initializeQuery = resetGpuBaseline_.exchange(false) ||
        !gpuQuery_ || !gpuUtilizationCounter_ ||
        !gpuDedicatedUsageCounter_ || !gpuSharedUsageCounter_;
    if (initializeQuery)
    {
        if (!InitializeGpuQuery())
        {
            snapshot.error = "GPU utilization sampling unavailable";
            snapshot.warmingUp = false;
        }
    }
    else if (PdhCollectQueryData(
                 reinterpret_cast<HQUERY>(gpuQuery_)) == ERROR_SUCCESS)
    {
        bool utilizationAvailable = false;
        bool memoryUsageAvailable = false;
        DWORD bufferBytes = 0;
        DWORD itemCount = 0;
        PDH_STATUS status = PdhGetFormattedCounterArrayW(
            reinterpret_cast<HCOUNTER>(gpuUtilizationCounter_),
            PDH_FMT_DOUBLE, &bufferBytes, &itemCount, nullptr);
        if (status == PDH_MORE_DATA && bufferBytes > 0)
        {
            std::vector<std::byte> buffer(bufferBytes);
            auto* items = reinterpret_cast<
                PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
            status = PdhGetFormattedCounterArrayW(
                reinterpret_cast<HCOUNTER>(gpuUtilizationCounter_),
                PDH_FMT_DOUBLE, &bufferBytes, &itemCount, items);
            if (status == ERROR_SUCCESS)
            {
                std::unordered_map<std::uint64_t, double> usageByLuid;
                for (DWORD index = 0; index < itemCount; ++index)
                {
                    if (items[index].FmtValue.CStatus !=
                            PDH_CSTATUS_VALID_DATA &&
                        items[index].FmtValue.CStatus !=
                            PDH_CSTATUS_NEW_DATA)
                        continue;
                    const auto luid = ParseGpuLuid(items[index].szName);
                    if (!luid) continue;
                    usageByLuid[*luid] +=
                        items[index].FmtValue.doubleValue;
                }
                for (auto& entry : adapters)
                {
                    entry.snapshot.usagePercent = std::clamp(
                        usageByLuid[entry.luid], 0.0, 100.0);
                }
                utilizationAvailable = true;
            }
        }

        const auto readMemoryUsage = [](void* counter,
            std::unordered_map<std::uint64_t, std::uint64_t>& byLuid) {
            DWORD bytes = 0;
            DWORD count = 0;
            PDH_STATUS status = PdhGetFormattedCounterArrayW(
                reinterpret_cast<HCOUNTER>(counter), PDH_FMT_LARGE,
                &bytes, &count, nullptr);
            if (status != PDH_MORE_DATA || bytes == 0)
                return false;
            std::vector<std::byte> buffer(bytes);
            auto* values = reinterpret_cast<
                PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
            status = PdhGetFormattedCounterArrayW(
                reinterpret_cast<HCOUNTER>(counter), PDH_FMT_LARGE,
                &bytes, &count, values);
            if (status != ERROR_SUCCESS)
                return false;
            for (DWORD index = 0; index < count; ++index)
            {
                if (values[index].FmtValue.CStatus !=
                        PDH_CSTATUS_VALID_DATA &&
                    values[index].FmtValue.CStatus !=
                        PDH_CSTATUS_NEW_DATA)
                    continue;
                const auto luid = ParseGpuLuid(values[index].szName);
                if (!luid || values[index].FmtValue.largeValue <= 0)
                    continue;
                byLuid[*luid] += static_cast<std::uint64_t>(
                    values[index].FmtValue.largeValue);
            }
            return true;
        };
        std::unordered_map<std::uint64_t, std::uint64_t>
            dedicatedUsageByLuid;
        std::unordered_map<std::uint64_t, std::uint64_t>
            sharedUsageByLuid;
        memoryUsageAvailable = readMemoryUsage(
                gpuDedicatedUsageCounter_, dedicatedUsageByLuid) &&
            readMemoryUsage(
                gpuSharedUsageCounter_, sharedUsageByLuid);
        if (memoryUsageAvailable)
        {
            for (auto& entry : adapters)
            {
                entry.snapshot.dedicatedUsedBytes =
                    dedicatedUsageByLuid[entry.luid];
                entry.snapshot.sharedUsedBytes =
                    sharedUsageByLuid[entry.luid];
            }
        }

        snapshot.warmingUp = !utilizationAvailable;
        if (!utilizationAvailable)
            snapshot.error = "GPU utilization sampling unavailable";
        else if (!memoryUsageAvailable)
            snapshot.error = "GPU memory sampling unavailable";
    }
    else
    {
        snapshot.error = "GPU utilization sampling failed";
        snapshot.warmingUp = false;
    }

    snapshot.adapters.reserve(adapters.size());
    for (auto& entry : adapters)
        snapshot.adapters.push_back(std::move(entry.snapshot));
    return snapshot;
}

WidgetStorageVolumesDataSnapshot
WidgetSystemDataProvider::SampleStorageVolumes()
{
    WidgetStorageVolumesDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (required == 0)
    {
        snapshot.error = "volume enumeration failed";
        return snapshot;
    }
    std::vector<wchar_t> roots(static_cast<std::size_t>(required) + 1, L'\0');
    const DWORD copied = GetLogicalDriveStringsW(
        static_cast<DWORD>(roots.size()), roots.data());
    if (copied == 0 || copied >= roots.size())
    {
        snapshot.error = "volume enumeration failed";
        return snapshot;
    }

    for (const wchar_t* root = roots.data(); *root;
        root += wcslen(root) + 1)
    {
        // GetDriveTypeW can synchronously wait for a disconnected mapped
        // network drive. QueryDosDevice is local-only, so identify redirector
        // mappings before asking the filesystem for any drive metadata.
        const UINT driveType = IsMappedNetworkRoot(root)
            ? DRIVE_REMOTE : GetDriveTypeW(root);
        if (driveType == DRIVE_NO_ROOT_DIR)
            continue;
        ULARGE_INTEGER available{};
        ULARGE_INTEGER capacity{};
        ULARGE_INTEGER free{};
        const bool queryCapacity = driveType != DRIVE_REMOTE &&
            driveType != DRIVE_CDROM && driveType != DRIVE_UNKNOWN;
        const bool capacityAvailable = queryCapacity &&
            GetDiskFreeSpaceExW(root, &available, &capacity, &free) != FALSE;

        wchar_t label[MAX_PATH + 1]{};
        DWORD fileSystemFlags = 0;
        const bool volumeInfoAvailable = capacityAvailable &&
            GetVolumeInformationW(
            root, label, static_cast<DWORD>(std::size(label)), nullptr,
            nullptr, &fileSystemFlags, nullptr, 0) != FALSE;
        std::wstring displayName = label;
        if (displayName.empty())
        {
            displayName = root;
            while (!displayName.empty() &&
                (displayName.back() == L'\\' || displayName.back() == L'/'))
            {
                displayName.pop_back();
            }
        }
        WidgetStorageVolumeDataSnapshot volume;
        volume.id = OpaqueVolumeId(root, capacityAvailable);
        volume.displayName = WideToUtf8(displayName.c_str());
        volume.mountPoint = WideToUtf8(root);
        volume.kind = DriveKind(driveType);
        volume.capacityBytes = capacity.QuadPart;
        volume.freeBytes = available.QuadPart;
        volume.capacityAvailable = capacityAvailable;
        volume.removable = driveType == DRIVE_REMOVABLE;
        volume.readOnly = driveType == DRIVE_CDROM ||
            (volumeInfoAvailable &&
                (fileSystemFlags & FILE_READ_ONLY_VOLUME) != 0);
        snapshot.volumes.push_back(std::move(volume));
    }
    snapshot.available = true;
    return snapshot;
}

bool WidgetSystemDataProvider::InitializeStorageIoQuery()
{
    CloseStorageIoQuery();
    HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS)
        return false;
    HCOUNTER readCounter = nullptr;
    HCOUNTER writeCounter = nullptr;
    HCOUNTER busyCounter = nullptr;
    const bool countersAdded =
        PdhAddEnglishCounterW(query,
            L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
            0, &readCounter) == ERROR_SUCCESS &&
        PdhAddEnglishCounterW(query,
            L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
            0, &writeCounter) == ERROR_SUCCESS &&
        PdhAddEnglishCounterW(query,
            L"\\PhysicalDisk(_Total)\\% Disk Time",
            0, &busyCounter) == ERROR_SUCCESS;
    if (!countersAdded || PdhCollectQueryData(query) != ERROR_SUCCESS)
    {
        if (busyCounter) PdhRemoveCounter(busyCounter);
        if (writeCounter) PdhRemoveCounter(writeCounter);
        if (readCounter) PdhRemoveCounter(readCounter);
        PdhCloseQuery(query);
        return false;
    }
    storageIoQuery_ = query;
    storageReadCounter_ = readCounter;
    storageWriteCounter_ = writeCounter;
    storageBusyCounter_ = busyCounter;
    storageIoResourcesActive_.store(true);
    return true;
}

void WidgetSystemDataProvider::CloseStorageIoQuery()
{
    if (storageBusyCounter_)
    {
        PdhRemoveCounter(reinterpret_cast<HCOUNTER>(storageBusyCounter_));
        storageBusyCounter_ = nullptr;
    }
    if (storageWriteCounter_)
    {
        PdhRemoveCounter(reinterpret_cast<HCOUNTER>(storageWriteCounter_));
        storageWriteCounter_ = nullptr;
    }
    if (storageReadCounter_)
    {
        PdhRemoveCounter(reinterpret_cast<HCOUNTER>(storageReadCounter_));
        storageReadCounter_ = nullptr;
    }
    if (storageIoQuery_)
    {
        PdhCloseQuery(reinterpret_cast<HQUERY>(storageIoQuery_));
        storageIoQuery_ = nullptr;
    }
    storageIoResourcesActive_.store(false);
}

WidgetStorageIoDataSnapshot WidgetSystemDataProvider::SampleStorageIo()
{
    WidgetStorageIoDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    snapshot.available = true;
    const bool initializeQuery = resetStorageIoBaseline_.exchange(false) ||
        !storageIoQuery_ || !storageReadCounter_ ||
        !storageWriteCounter_ || !storageBusyCounter_;
    if (initializeQuery)
    {
        if (!InitializeStorageIoQuery())
        {
            snapshot.available = false;
            snapshot.warmingUp = false;
            snapshot.error = "storage I/O sampling unavailable";
        }
        return snapshot;
    }
    if (PdhCollectQueryData(
            reinterpret_cast<HQUERY>(storageIoQuery_)) != ERROR_SUCCESS)
    {
        snapshot.available = false;
        snapshot.warmingUp = false;
        snapshot.error = "storage I/O sampling failed";
        return snapshot;
    }

    const auto readCounter = [](void* handle, double& value) {
        PDH_FMT_COUNTERVALUE formatted{};
        DWORD type = 0;
        if (PdhGetFormattedCounterValue(reinterpret_cast<HCOUNTER>(handle),
                PDH_FMT_DOUBLE, &type, &formatted) != ERROR_SUCCESS ||
            (formatted.CStatus != PDH_CSTATUS_VALID_DATA &&
                formatted.CStatus != PDH_CSTATUS_NEW_DATA))
            return false;
        value = formatted.doubleValue;
        return true;
    };
    double readBytes = 0.0;
    double writeBytes = 0.0;
    double busyPercent = 0.0;
    if (!readCounter(storageReadCounter_, readBytes) ||
        !readCounter(storageWriteCounter_, writeBytes) ||
        !readCounter(storageBusyCounter_, busyPercent))
    {
        snapshot.available = false;
        snapshot.warmingUp = false;
        snapshot.error = "storage I/O counters unavailable";
        return snapshot;
    }
    snapshot.readBytesPerSecond = static_cast<std::uint64_t>(
        std::max(0.0, readBytes));
    snapshot.writeBytesPerSecond = static_cast<std::uint64_t>(
        std::max(0.0, writeBytes));
    snapshot.busyPercent = std::clamp(busyPercent, 0.0, 100.0);
    snapshot.warmingUp = false;
    return snapshot;
}

WidgetDisplayTopologyDataSnapshot
WidgetSystemDataProvider::SampleDisplayTopology()
{
    struct EnumContext
    {
        WidgetDisplayTopologyDataSnapshot* snapshot = nullptr;
        const std::unordered_map<std::wstring,
            DisplayTargetMetadata>* metadata = nullptr;
    };

    WidgetDisplayTopologyDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    const auto metadata = LoadDisplayTargetMetadata();
    EnumContext context{ &snapshot, &metadata };
    const auto callback = [](HMONITOR monitor, HDC, LPRECT,
                              LPARAM parameter) -> BOOL {
        auto* context = reinterpret_cast<EnumContext*>(parameter);
        if (!context || !context->snapshot) return FALSE;
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) return TRUE;

        WidgetDisplayDataSnapshot display;
        display.id = OpaqueDisplayId(info.szDevice);
        display.name = WideToUtf8(info.szDevice);
        display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        display.pixelBounds = { info.rcMonitor.left, info.rcMonitor.top,
            info.rcMonitor.right - info.rcMonitor.left,
            info.rcMonitor.bottom - info.rcMonitor.top };
        display.pixelWorkArea = { info.rcWork.left, info.rcWork.top,
            info.rcWork.right - info.rcWork.left,
            info.rcWork.bottom - info.rcWork.top };
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (FAILED(GetDpiForMonitor(
                monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        {
            dpiX = dpiY = 96;
        }
        display.dpiX = dpiX;
        display.dpiY = dpiY;
        display.scale = static_cast<double>(dpiX) / 96.0;
        const double xScale = std::max(
            0.01, static_cast<double>(dpiX) / 96.0);
        const double yScale = std::max(
            0.01, static_cast<double>(dpiY) / 96.0);
        display.bounds = {
            static_cast<double>(info.rcMonitor.left) / xScale,
            static_cast<double>(info.rcMonitor.top) / yScale,
            static_cast<double>(display.pixelBounds.width) / xScale,
            static_cast<double>(display.pixelBounds.height) / yScale };
        display.workArea = {
            static_cast<double>(info.rcWork.left) / xScale,
            static_cast<double>(info.rcWork.top) / yScale,
            static_cast<double>(display.pixelWorkArea.width) / xScale,
            static_cast<double>(display.pixelWorkArea.height) / yScale };

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsExW(
                info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0))
        {
            display.refreshHz = mode.dmDisplayFrequency > 1
                ? static_cast<double>(mode.dmDisplayFrequency) : 0.0;
            display.orientation = DisplayOrientation(
                mode.dmDisplayOrientation);
        }
        else
        {
            display.orientation = "unknown";
        }

        const auto found = context->metadata->find(
            LowerWide(info.szDevice));
        if (found != context->metadata->end())
        {
            if (!found->second.friendlyName.empty())
                display.name = WideToUtf8(
                    found->second.friendlyName.c_str());
            display.hdrKnown = found->second.hdrKnown;
            display.hdrSupported = found->second.hdrSupported;
            display.hdrEnabled = found->second.hdrEnabled;
        }
        context->snapshot->displays.push_back(std::move(display));
        return TRUE;
    };
    if (!EnumDisplayMonitors(nullptr, nullptr, callback,
            reinterpret_cast<LPARAM>(&context)))
    {
        snapshot.error = "display enumeration failed";
        return snapshot;
    }
    if (snapshot.displays.empty())
    {
        snapshot.error = "notPresent";
        return snapshot;
    }
    std::sort(snapshot.displays.begin(), snapshot.displays.end(),
        [](const auto& left, const auto& right) {
            if (left.primary != right.primary) return left.primary;
            if (left.pixelBounds.x != right.pixelBounds.x)
                return left.pixelBounds.x < right.pixelBounds.x;
            if (left.pixelBounds.y != right.pixelBounds.y)
                return left.pixelBounds.y < right.pixelBounds.y;
            return left.id < right.id;
        });
    snapshot.available = true;
    return snapshot;
}

WidgetAudioOutputDefaultDataSnapshot
WidgetSystemDataProvider::SampleAudioOutputDefault()
{
    WidgetAudioOutputDefaultDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    std::string error;
    auto endpoint = DefaultRenderEndpoint(error);
    if (!endpoint)
    {
        snapshot.error = std::move(error);
        return snapshot;
    }
    snapshot.id = OpaqueEndpointId(endpoint.Get());
    DWORD state = 0;
    if (FAILED(endpoint->GetState(&state)))
    {
        snapshot.error = "audioEndpointStateUnavailable";
        return snapshot;
    }
    snapshot.state = AudioDeviceState(state);

    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &properties)) &&
        properties)
    {
        PROPVARIANT value{};
        PropVariantInit(&value);
        if (SUCCEEDED(properties->GetValue(
                PKEY_Device_FriendlyName, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal)
        {
            snapshot.name = WideToUtf8(value.pwszVal);
        }
        PropVariantClear(&value);
    }
    snapshot.available = !snapshot.id.empty();
    if (!snapshot.available)
        snapshot.error = "audioEndpointIdentityUnavailable";
    return snapshot;
}

WidgetAudioOutputVolumeDataSnapshot
WidgetSystemDataProvider::SampleAudioOutputVolume()
{
    WidgetAudioOutputVolumeDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    std::string error;
    auto endpoint = DefaultRenderEndpoint(error);
    if (!endpoint)
    {
        snapshot.error = std::move(error);
        return snapshot;
    }
    snapshot.endpointId = OpaqueEndpointId(endpoint.Get());
    Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(endpoint->Activate(__uuidof(IAudioEndpointVolume),
            CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void**>(volume.GetAddressOf()))) || !volume)
    {
        snapshot.error = "audioVolumeUnavailable";
        return snapshot;
    }
    float scalar = 0.0f;
    BOOL muted = FALSE;
    if (FAILED(volume->GetMasterVolumeLevelScalar(&scalar)) ||
        FAILED(volume->GetMute(&muted)))
    {
        snapshot.error = "audioVolumeUnavailable";
        return snapshot;
    }
    snapshot.available = !snapshot.endpointId.empty();
    snapshot.volume = std::clamp(
        static_cast<double>(scalar), 0.0, 1.0);
    snapshot.muted = muted != FALSE;
    if (!snapshot.available)
        snapshot.error = "audioEndpointIdentityUnavailable";
    return snapshot;
}

WidgetMediaSessionsDataSnapshot
WidgetSystemDataProvider::SampleMediaSessions(bool includeArtwork)
{
    using namespace winrt::Windows::Media::Control;
    WidgetMediaSessionsDataSnapshot snapshot;
    snapshot.timestampMs = TimestampMilliseconds();
    snapshot.artwork.timestampMs = snapshot.timestampMs;
    try
    {
        thread_local GlobalSystemMediaTransportControlsSessionManager manager{
            nullptr };
        if (!manager)
        {
            manager = GlobalSystemMediaTransportControlsSessionManager::
                RequestAsync().get();
        }
        if (!manager)
        {
            snapshot.error = "mediaSessionManagerUnavailable";
            return snapshot;
        }

        const auto currentSession = manager.GetCurrentSession();
        const auto sessions = manager.GetSessions();
        std::unordered_map<std::wstring, std::size_t> sourceOccurrences;
        const auto appendSession = [&](const auto& session, bool current) {
            if (!session || snapshot.sessions.size() >=
                    MaximumExposedMediaSessions)
                return;
            WidgetMediaSessionDataSnapshot value;
            const std::wstring sourceId =
                session.SourceAppUserModelId().c_str();
            const std::size_t occurrence = sourceOccurrences[sourceId]++;
            value.id = OpaqueMediaSessionId(sourceId, occurrence);
            value.sourceName = BoundedMediaString(sourceId);
            value.current = current;

            const auto playback = session.GetPlaybackInfo();
            value.playbackStatus = MediaPlaybackStatus(
                playback.PlaybackStatus());
            const auto controls = playback.Controls();
            value.controls.canPlay = controls.IsPlayEnabled();
            value.controls.canPause = controls.IsPauseEnabled();
            value.controls.canPlayPause =
                controls.IsPlayPauseToggleEnabled();
            value.controls.canStop = controls.IsStopEnabled();
            value.controls.canNext = controls.IsNextEnabled();
            value.controls.canPrevious = controls.IsPreviousEnabled();
            value.controls.canSeek = controls.IsPlaybackPositionEnabled();
            value.controls.canChangePlaybackRate =
                controls.IsPlaybackRateEnabled();
            value.controls.canToggleShuffle = controls.IsShuffleEnabled();
            value.controls.canChangeRepeatMode = controls.IsRepeatEnabled();

            const auto timeline = session.GetTimelineProperties();
            const std::int64_t start = TimeSpanMilliseconds(
                timeline.StartTime());
            const std::int64_t end = TimeSpanMilliseconds(
                timeline.EndTime());
            const std::int64_t position = TimeSpanMilliseconds(
                timeline.Position());
            value.timeline.available = true;
            value.timeline.sessionId = value.id;
            value.timeline.durationMs = std::max<std::int64_t>(0, end - start);
            value.timeline.positionMs = std::clamp<std::int64_t>(
                position - start, 0, value.timeline.durationMs);
            value.timeline.minimumSeekMs = std::max<std::int64_t>(0,
                TimeSpanMilliseconds(timeline.MinSeekTime()) - start);
            value.timeline.maximumSeekMs = std::max<std::int64_t>(
                value.timeline.minimumSeekMs,
                TimeSpanMilliseconds(timeline.MaxSeekTime()) - start);
            value.timeline.updatedAtMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    winrt::clock::to_sys(timeline.LastUpdatedTime())
                        .time_since_epoch()).count();
            value.timeline.timestampMs = snapshot.timestampMs;

            try
            {
                const auto properties =
                    session.TryGetMediaPropertiesAsync().get();
                value.title = BoundedMediaString(properties.Title());
                value.artist = BoundedMediaString(properties.Artist());
                value.album = BoundedMediaString(properties.AlbumTitle());
                if (current && includeArtwork)
                {
                    const std::uint64_t mediaIdentity = MediaArtworkIdentity(
                        value.id, value.title, value.artist, value.album);
                    const auto previous = MediaArtwork();
                    if (previous && previous->available &&
                        previous->mediaIdentity == mediaIdentity)
                    {
                        snapshot.artwork = *previous;
                        snapshot.artwork.timestampMs = snapshot.timestampMs;
                    }
                    else
                    {
                        snapshot.artwork = DecodeMediaArtwork(
                            properties.Thumbnail(), value.id,
                            snapshot.timestampMs);
                        snapshot.artwork.mediaIdentity = mediaIdentity;
                    }
                }
            }
            catch (...)
            {
                // One source may withhold metadata without invalidating the
                // session list, playback state, controls, or timeline.
                if (current && includeArtwork)
                {
                    snapshot.artwork.sessionId = value.id;
                    snapshot.artwork.timestampMs = snapshot.timestampMs;
                    snapshot.artwork.error = "artworkQueryFailed";
                }
            }
            if (current) snapshot.currentSessionId = value.id;
            snapshot.sessions.push_back(std::move(value));
        };

        if (currentSession) appendSession(currentSession, true);
        for (const auto& session : sessions)
        {
            if (snapshot.sessions.size() >=
                MaximumExposedMediaSessions) break;
            if (currentSession && session == currentSession) continue;
            appendSession(session, false);
        }
        snapshot.available = true;
        if (includeArtwork && snapshot.artwork.error.empty() &&
            !snapshot.artwork.available)
            snapshot.artwork.error = "notPresent";
    }
    catch (...)
    {
        snapshot.error = "mediaSessionQueryFailed";
        if (includeArtwork)
            snapshot.artwork.error = "mediaSessionQueryFailed";
    }
    return snapshot;
}

void WidgetSystemDataProvider::PublishCpu(
    WidgetCpuDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(CpuTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot), cpu_,
        semanticDebouncers_[std::string(CpuTopic)]);
    snapshot.revision = cpu_ ? cpu_->revision + 1 : 1;
    cpu_ = std::move(snapshot);
    changedTopics_.insert(std::string(CpuTopic));
}

void WidgetSystemDataProvider::PublishMemory(
    WidgetMemoryDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(MemoryTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot), memory_,
        semanticDebouncers_[std::string(MemoryTopic)]);
    snapshot.revision = memory_ ? memory_->revision + 1 : 1;
    memory_ = std::move(snapshot);
    changedTopics_.insert(std::string(MemoryTopic));
}

void WidgetSystemDataProvider::PublishProcessSummary(
    WidgetProcessSummaryDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(ProcessSummaryTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot),
        processSummary_,
        semanticDebouncers_[std::string(ProcessSummaryTopic)]);
    snapshot.revision = processSummary_
        ? processSummary_->revision + 1 : 1;
    processSummary_ = std::move(snapshot);
    changedTopics_.insert(std::string(ProcessSummaryTopic));
}

void WidgetSystemDataProvider::PublishPower(
    WidgetPowerDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(PowerTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot), power_,
        semanticDebouncers_[std::string(PowerTopic)]);
    snapshot.revision = power_ ? power_->revision + 1 : 1;
    power_ = std::move(snapshot);
    changedTopics_.insert(std::string(PowerTopic));
}

void WidgetSystemDataProvider::PublishNetworkStatus(
    WidgetNetworkStatusDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(NetworkStatusTopic))) return;
    snapshot = networkStatusDebouncer_.Push(std::move(snapshot));
    snapshot.revision = networkStatus_ ? networkStatus_->revision + 1 : 1;
    networkStatus_ = std::move(snapshot);
    changedTopics_.insert(std::string(NetworkStatusTopic));
}

void WidgetSystemDataProvider::PublishNetworkTraffic(
    WidgetNetworkTrafficDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(NetworkTrafficTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot),
        networkTraffic_,
        semanticDebouncers_[std::string(NetworkTrafficTopic)]);
    snapshot.revision = networkTraffic_ ? networkTraffic_->revision + 1 : 1;
    networkTraffic_ = std::move(snapshot);
    changedTopics_.insert(std::string(NetworkTrafficTopic));
}

void WidgetSystemDataProvider::PublishGpu(
    WidgetGpuDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(GpuTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot), gpu_,
        semanticDebouncers_[std::string(GpuTopic)]);
    snapshot.revision = gpu_ ? gpu_->revision + 1 : 1;
    gpu_ = std::move(snapshot);
    changedTopics_.insert(std::string(GpuTopic));
}

void WidgetSystemDataProvider::PublishStorageVolumes(
    WidgetStorageVolumesDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(StorageVolumesTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot),
        storageVolumes_,
        semanticDebouncers_[std::string(StorageVolumesTopic)]);
    snapshot.revision = storageVolumes_ ? storageVolumes_->revision + 1 : 1;
    storageVolumes_ = std::move(snapshot);
    changedTopics_.insert(std::string(StorageVolumesTopic));
}

void WidgetSystemDataProvider::PublishStorageIo(
    WidgetStorageIoDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(StorageIoTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot), storageIo_,
        semanticDebouncers_[std::string(StorageIoTopic)]);
    snapshot.revision = storageIo_ ? storageIo_->revision + 1 : 1;
    storageIo_ = std::move(snapshot);
    changedTopics_.insert(std::string(StorageIoTopic));
}

void WidgetSystemDataProvider::PublishDisplayTopology(
    WidgetDisplayTopologyDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(DisplayTopologyTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot),
        displayTopology_,
        semanticDebouncers_[std::string(DisplayTopologyTopic)]);
    snapshot.revision = displayTopology_
        ? displayTopology_->revision + 1 : 1;
    displayTopology_ = std::move(snapshot);
    changedTopics_.insert(std::string(DisplayTopologyTopic));
}

void WidgetSystemDataProvider::PublishDisplayCurrent(
    WidgetDisplayTopologyDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(DisplayCurrentTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot),
        displayCurrent_,
        semanticDebouncers_[std::string(DisplayCurrentTopic)]);
    snapshot.revision = displayCurrent_
        ? displayCurrent_->revision + 1 : 1;
    displayCurrent_ = std::move(snapshot);
    changedTopics_.insert(std::string(DisplayCurrentTopic));
}

void WidgetSystemDataProvider::PublishAudioOutputDefault(
    WidgetAudioOutputDefaultDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(AudioOutputDefaultTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot),
        audioOutputDefault_,
        semanticDebouncers_[std::string(AudioOutputDefaultTopic)]);
    snapshot.revision = audioOutputDefault_
        ? audioOutputDefault_->revision + 1 : 1;
    audioOutputDefault_ = std::move(snapshot);
    changedTopics_.insert(std::string(AudioOutputDefaultTopic));
}

void WidgetSystemDataProvider::PublishAudioOutputVolume(
    WidgetAudioOutputVolumeDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(AudioOutputVolumeTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot),
        audioOutputVolume_,
        semanticDebouncers_[std::string(AudioOutputVolumeTopic)]);
    snapshot.revision = audioOutputVolume_
        ? audioOutputVolume_->revision + 1 : 1;
    audioOutputVolume_ = std::move(snapshot);
    changedTopics_.insert(std::string(AudioOutputVolumeTopic));
}

void WidgetSystemDataProvider::PublishMediaSessions(
    WidgetMediaSessionsDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(MediaSessionsTopic))) return;
    snapshot = StabilizeWidgetDataEnvelope(std::move(snapshot),
        mediaSessions_,
        semanticDebouncers_[std::string(MediaSessionsTopic)]);
    snapshot.revision = mediaSessions_ ? mediaSessions_->revision + 1 : 1;
    mediaSessions_ = std::move(snapshot);
    changedTopics_.insert(std::string(MediaSessionsTopic));
}

void WidgetSystemDataProvider::PublishMediaCurrent(
    const WidgetMediaSessionsDataSnapshot& snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(MediaCurrentTopic))) return;
    WidgetMediaCurrentDataSnapshot current;
    current.timestampMs = snapshot.timestampMs;
    if (!snapshot.available)
    {
        current.error = snapshot.error.empty()
            ? "mediaSessionQueryFailed" : snapshot.error;
    }
    else
    {
        const auto match = std::find_if(snapshot.sessions.begin(),
            snapshot.sessions.end(), [](const auto& session) {
                return session.current;
            });
        if (match == snapshot.sessions.end())
            current.error = "notPresent";
        else
        {
            current.available = true;
            current.session = *match;
        }
    }
    current = StabilizeWidgetDataEnvelope(std::move(current), mediaCurrent_,
        semanticDebouncers_[std::string(MediaCurrentTopic)]);
    current.revision = mediaCurrent_ ? mediaCurrent_->revision + 1 : 1;
    mediaCurrent_ = std::move(current);
    changedTopics_.insert(std::string(MediaCurrentTopic));
}

void WidgetSystemDataProvider::PublishMediaTimeline(
    const WidgetMediaSessionsDataSnapshot& snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(MediaTimelineTopic))) return;
    WidgetMediaTimelineDataSnapshot timeline;
    timeline.timestampMs = snapshot.timestampMs;
    if (!snapshot.available)
    {
        timeline.error = snapshot.error.empty()
            ? "mediaSessionQueryFailed" : snapshot.error;
    }
    else
    {
        const auto match = std::find_if(snapshot.sessions.begin(),
            snapshot.sessions.end(), [](const auto& session) {
                return session.current;
            });
        if (match == snapshot.sessions.end() ||
            !match->timeline.available)
        {
            timeline.error = "notPresent";
        }
        else
        {
            timeline = match->timeline;
            timeline.timestampMs = snapshot.timestampMs;
        }
    }
    timeline = StabilizeWidgetDataEnvelope(std::move(timeline),
        mediaTimeline_,
        semanticDebouncers_[std::string(MediaTimelineTopic)]);
    timeline.revision = mediaTimeline_ ? mediaTimeline_->revision + 1 : 1;
    mediaTimeline_ = std::move(timeline);
    changedTopics_.insert(std::string(MediaTimelineTopic));
}

void WidgetSystemDataProvider::PublishMediaArtwork(
    const WidgetMediaSessionsDataSnapshot& snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(MediaArtworkTopic))) return;
    WidgetMediaArtworkDataSnapshot artwork = snapshot.artwork;
    artwork.timestampMs = snapshot.timestampMs;
    if (!snapshot.available && artwork.error.empty())
        artwork.error = snapshot.error.empty()
            ? "mediaSessionQueryFailed" : snapshot.error;
    artwork = StabilizeWidgetDataEnvelope(std::move(artwork), mediaArtwork_,
        semanticDebouncers_[std::string(MediaArtworkTopic)]);
    artwork.revision = mediaArtwork_ ? mediaArtwork_->revision + 1 : 1;
    mediaArtwork_ = std::move(artwork);
    changedTopics_.insert(std::string(MediaArtworkTopic));
}
}
