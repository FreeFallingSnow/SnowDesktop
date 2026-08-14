#include "widget_system_data_provider.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::string_view CpuTopic = "system.cpu";
constexpr std::string_view MemoryTopic = "system.memory";
constexpr std::string_view PowerTopic = "system.power";
constexpr std::string_view NetworkStatusTopic = "system.network.status";
constexpr std::string_view NetworkTrafficTopic = "system.network.traffic";
constexpr std::string_view GpuTopic = "system.gpu";
constexpr std::string_view StorageVolumesTopic = "system.storage.volumes";
constexpr std::string_view StorageIoTopic = "system.storage.io";

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
}

WidgetSystemDataProvider::~WidgetSystemDataProvider()
{
    StopAll();
}

bool WidgetSystemDataProvider::SupportsTopic(
    std::string_view topic) noexcept
{
    return topic == CpuTopic || topic == MemoryTopic ||
        topic == PowerTopic || topic == NetworkStatusTopic ||
        topic == NetworkTrafficTopic || topic == GpuTopic ||
        topic == StorageVolumesTopic || topic == StorageIoTopic;
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
        if (topic == CpuTopic) resetCpuBaseline_.store(true);
        if (topic == NetworkTrafficTopic)
            resetNetworkBaseline_.store(true);
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
        ++configurationGeneration_;
    }
    resetCpuBaseline_.store(true);
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

        for (const std::string& topic : dueTopics)
        {
            if (stopToken.stop_requested()) break;
            if (topic == CpuTopic)
                PublishCpu(SampleCpu());
            else if (topic == MemoryTopic)
                PublishMemory(SampleMemory());
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
    snapshot.available = true;
    snapshot.totalBytes = status.ullTotalPhys;
    snapshot.freeBytes = status.ullAvailPhys;
    snapshot.usedBytes = snapshot.totalBytes - snapshot.freeBytes;
    snapshot.usagePercent = static_cast<double>(status.dwMemoryLoad);
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
    if (PdhAddEnglishCounterW(query,
            L"\\GPU Engine(*)\\Utilization Percentage",
            0, &counter) != ERROR_SUCCESS)
    {
        PdhCloseQuery(query);
        return false;
    }
    if (PdhCollectQueryData(query) != ERROR_SUCCESS)
    {
        PdhRemoveCounter(counter);
        PdhCloseQuery(query);
        return false;
    }
    gpuQuery_ = query;
    gpuUtilizationCounter_ = counter;
    gpuResourcesActive_.store(true);
    return true;
}

void WidgetSystemDataProvider::CloseGpuQuery()
{
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
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(adapter.As(&adapter3)))
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                    0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
            {
                entry.snapshot.dedicatedUsedBytes = local.CurrentUsage;
            }
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                    0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal)))
            {
                entry.snapshot.sharedUsedBytes = nonLocal.CurrentUsage;
            }
        }
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
        !gpuQuery_ || !gpuUtilizationCounter_;
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
                snapshot.warmingUp = false;
            }
        }
        if (snapshot.warmingUp)
            snapshot.error = "GPU utilization sampling unavailable";
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

void WidgetSystemDataProvider::PublishCpu(
    WidgetCpuDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(CpuTopic))) return;
    snapshot.revision = cpu_ ? cpu_->revision + 1 : 1;
    cpu_ = std::move(snapshot);
    changedTopics_.insert(std::string(CpuTopic));
}

void WidgetSystemDataProvider::PublishMemory(
    WidgetMemoryDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(MemoryTopic))) return;
    snapshot.revision = memory_ ? memory_->revision + 1 : 1;
    memory_ = std::move(snapshot);
    changedTopics_.insert(std::string(MemoryTopic));
}

void WidgetSystemDataProvider::PublishPower(
    WidgetPowerDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(PowerTopic))) return;
    snapshot.revision = power_ ? power_->revision + 1 : 1;
    power_ = std::move(snapshot);
    changedTopics_.insert(std::string(PowerTopic));
}

void WidgetSystemDataProvider::PublishNetworkStatus(
    WidgetNetworkStatusDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(NetworkStatusTopic))) return;
    snapshot.revision = networkStatus_ ? networkStatus_->revision + 1 : 1;
    networkStatus_ = std::move(snapshot);
    changedTopics_.insert(std::string(NetworkStatusTopic));
}

void WidgetSystemDataProvider::PublishNetworkTraffic(
    WidgetNetworkTrafficDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(NetworkTrafficTopic))) return;
    snapshot.revision = networkTraffic_ ? networkTraffic_->revision + 1 : 1;
    networkTraffic_ = std::move(snapshot);
    changedTopics_.insert(std::string(NetworkTrafficTopic));
}

void WidgetSystemDataProvider::PublishGpu(
    WidgetGpuDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(GpuTopic))) return;
    snapshot.revision = gpu_ ? gpu_->revision + 1 : 1;
    gpu_ = std::move(snapshot);
    changedTopics_.insert(std::string(GpuTopic));
}

void WidgetSystemDataProvider::PublishStorageVolumes(
    WidgetStorageVolumesDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(StorageVolumesTopic))) return;
    snapshot.revision = storageVolumes_ ? storageVolumes_->revision + 1 : 1;
    storageVolumes_ = std::move(snapshot);
    changedTopics_.insert(std::string(StorageVolumesTopic));
}

void WidgetSystemDataProvider::PublishStorageIo(
    WidgetStorageIoDataSnapshot snapshot)
{
    std::scoped_lock lock(mutex_);
    if (!schedules_.contains(std::string(StorageIoTopic))) return;
    snapshot.revision = storageIo_ ? storageIo_->revision + 1 : 1;
    storageIo_ = std::move(snapshot);
    changedTopics_.insert(std::string(StorageIoTopic));
}
}
