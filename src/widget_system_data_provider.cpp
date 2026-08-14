#include "widget_system_data_provider.h"

#include <windows.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>

#include <algorithm>
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
        topic == NetworkTrafficTopic;
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
        }
    }
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
}
