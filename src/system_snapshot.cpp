#include "system_snapshot.h"

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
#include <winrt/Windows.Media.Control.h>

#include <algorithm>
#include <chrono>

namespace
{
std::uint64_t FileTimeValue(const FILETIME& value)
{
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

bool MediaEqual(const MediaSnapshot& a, const MediaSnapshot& b)
{
    return a.available == b.available && a.title == b.title && a.artist == b.artist &&
        a.album == b.album && a.sourceApp == b.sourceApp &&
        a.playbackStatus == b.playbackStatus && a.canPlayPause == b.canPlayPause &&
        a.canNext == b.canNext && a.canPrevious == b.canPrevious;
}

bool CpuEqual(const CpuSnapshot& a, const CpuSnapshot& b)
{
    return a.available == b.available && a.usagePercent == b.usagePercent &&
        a.logicalProcessors == b.logicalProcessors && a.name == b.name;
}

bool MemoryEqual(const MemorySnapshot& a, const MemorySnapshot& b)
{
    return a.available == b.available && a.totalBytes == b.totalBytes &&
        a.usedBytes == b.usedBytes && a.freeBytes == b.freeBytes &&
        a.usagePercent == b.usagePercent;
}

bool GpuEqual(const GpuSnapshot& a, const GpuSnapshot& b)
{
    return a.available == b.available && a.name == b.name &&
        a.usagePercent == b.usagePercent &&
        a.vramTotalBytes == b.vramTotalBytes &&
        a.vramUsedBytes == b.vramUsedBytes;
}

bool BatteryEqual(const BatterySnapshot& a, const BatterySnapshot& b)
{
    return a.available == b.available && a.percent == b.percent &&
        a.charging == b.charging && a.pluggedIn == b.pluggedIn &&
        a.saver == b.saver;
}

bool NetworkEqual(const NetworkSnapshot& a, const NetworkSnapshot& b)
{
    return a.available == b.available && a.connected == b.connected &&
        a.downloadBytesPerSec == b.downloadBytesPerSec &&
        a.uploadBytesPerSec == b.uploadBytesPerSec &&
        a.receivedBytes == b.receivedBytes && a.sentBytes == b.sentBytes;
}
}

SystemSnapshotService::~SystemSnapshotService()
{
    Stop();
    CloseGpuPdH();
}

bool SystemSnapshotService::InitGpuPdH()
{
    CloseGpuPdH();
    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, reinterpret_cast<HQUERY*>(&gpuQuery_));
    if (status != ERROR_SUCCESS) return false;

    status = PdhAddEnglishCounterW(reinterpret_cast<HQUERY>(gpuQuery_),
        L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
        0, reinterpret_cast<HCOUNTER*>(&gpuUtilCounter_));
    if (status != ERROR_SUCCESS)
    {
        status = PdhAddEnglishCounterW(reinterpret_cast<HQUERY>(gpuQuery_),
            L"\\GPU Engine(*engtype_Graphics)\\Utilization Percentage",
            0, reinterpret_cast<HCOUNTER*>(&gpuUtilCounter_));
    }
    if (status != ERROR_SUCCESS)
    {
        status = PdhAddEnglishCounterW(reinterpret_cast<HQUERY>(gpuQuery_),
            L"\\GPU Engine(*)\\Utilization Percentage",
            0, reinterpret_cast<HCOUNTER*>(&gpuUtilCounter_));
    }
    if (status != ERROR_SUCCESS) { CloseGpuPdH(); return false; }

    status = PdhAddEnglishCounterW(reinterpret_cast<HQUERY>(gpuQuery_),
        L"\\GPU Adapter Memory(*)\\Dedicated Usage",
        0, reinterpret_cast<HCOUNTER*>(&gpuMemCounter_));
    if (status != ERROR_SUCCESS) { gpuMemCounter_ = nullptr; }

    PdhCollectQueryData(reinterpret_cast<HQUERY>(gpuQuery_));
    return true;
}

void SystemSnapshotService::CloseGpuPdH()
{
    if (gpuUtilCounter_)
    {
        PdhRemoveCounter(reinterpret_cast<HCOUNTER>(gpuUtilCounter_));
        gpuUtilCounter_ = nullptr;
    }
    if (gpuMemCounter_)
    {
        PdhRemoveCounter(reinterpret_cast<HCOUNTER>(gpuMemCounter_));
        gpuMemCounter_ = nullptr;
    }
    if (gpuQuery_)
    {
        PdhCloseQuery(reinterpret_cast<HQUERY>(gpuQuery_));
        gpuQuery_ = nullptr;
    }
}

static double ReadPdhCounterAverage(void* counter)
{
    if (!counter) return 0.0;
    DWORD bufSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(reinterpret_cast<HCOUNTER>(counter),
        PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
    if (status != PDH_MORE_DATA || itemCount == 0) return 0.0;

    std::vector<BYTE> buf(bufSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    status = PdhGetFormattedCounterArrayW(reinterpret_cast<HCOUNTER>(counter),
        PDH_FMT_DOUBLE, &bufSize, &itemCount, items);
    if (status != ERROR_SUCCESS || itemCount == 0) return 0.0;

    double total = 0.0;
    double maxVal = 0.0;
    for (DWORD i = 0; i < itemCount; ++i)
    {
        double v = items[i].FmtValue.doubleValue;
        total += v;
        if (v > maxVal) maxVal = v;
    }
    return maxVal;
}

double SystemSnapshotService::ReadGpuUtilization()
{
    return std::clamp(ReadPdhCounterAverage(gpuUtilCounter_), 0.0, 100.0);
}

std::uint64_t SystemSnapshotService::ReadGpuMemBytes()
{
    if (!gpuMemCounter_) return 0;

    DWORD bufSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(reinterpret_cast<HCOUNTER>(gpuMemCounter_),
        PDH_FMT_LARGE, &bufSize, &itemCount, nullptr);
    if (status != PDH_MORE_DATA || itemCount == 0) return 0;

    std::vector<BYTE> buf(bufSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    status = PdhGetFormattedCounterArrayW(reinterpret_cast<HCOUNTER>(gpuMemCounter_),
        PDH_FMT_LARGE, &bufSize, &itemCount, items);
    if (status != ERROR_SUCCESS || itemCount == 0) return 0;

    std::uint64_t total = 0;
    for (DWORD i = 0; i < itemCount; ++i)
        total += items[i].FmtValue.largeValue;
    return total;
}

static bool PdhCollectIfNeeded(void* query)
{
    if (!query) return false;
    PDH_STATUS status = PdhCollectQueryData(reinterpret_cast<HQUERY>(query));
    return status == ERROR_SUCCESS;
}

bool SystemSnapshotService::Start(ChangedCallback callback)
{
    Stop();
    changedCallback_ = std::move(callback);
    worker_ = std::jthread([this](std::stop_token token) { WorkerMain(token); });
    return true;
}

void SystemSnapshotService::Stop()
{
    if (worker_.joinable())
    {
        worker_.request_stop();
        worker_.join();
    }
}

CpuSnapshot SystemSnapshotService::GetCpu() const { std::scoped_lock lock(mutex_); return cpu_; }
MemorySnapshot SystemSnapshotService::GetMemory() const { std::scoped_lock lock(mutex_); return memory_; }
GpuSnapshot SystemSnapshotService::GetGpu() const { std::scoped_lock lock(mutex_); return gpu_; }
BatterySnapshot SystemSnapshotService::GetBattery() const { std::scoped_lock lock(mutex_); return battery_; }
NetworkSnapshot SystemSnapshotService::GetNetwork() const { std::scoped_lock lock(mutex_); return network_; }
MediaSnapshot SystemSnapshotService::GetMedia() const { std::scoped_lock lock(mutex_); return media_; }
std::string SystemSnapshotService::GetLastError() const
{
    std::scoped_lock lock(mutex_);
    if (systemError_.empty()) return mediaError_;
    if (mediaError_.empty()) return systemError_;
    return systemError_ + "\n" + mediaError_;
}

bool SystemSnapshotService::RequestMediaPlayPause()
{
    if (!GetMedia().canPlayPause) return false;
    pendingMediaAction_.store(MediaAction::PlayPause);
    return true;
}

bool SystemSnapshotService::RequestMediaNext()
{
    if (!GetMedia().canNext) return false;
    pendingMediaAction_.store(MediaAction::Next);
    return true;
}

bool SystemSnapshotService::RequestMediaPrevious()
{
    if (!GetMedia().canPrevious) return false;
    pendingMediaAction_.store(MediaAction::Previous);
    return true;
}

void SystemSnapshotService::SetSystemError(const std::string& message)
{
    std::scoped_lock lock(mutex_);
    systemError_ = message;
}

void SystemSnapshotService::SetMediaError(const std::string& message)
{
    std::scoped_lock lock(mutex_);
    mediaError_ = message;
}

void SystemSnapshotService::WorkerMain(std::stop_token stopToken)
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (const winrt::hresult_error& error)
    {
        SetMediaError("Media runtime: " + winrt::to_string(error.message()));
    }

    while (!stopToken.stop_requested())
    {
        const bool systemChanged = SampleSystem();
        const bool mediaChanged = SampleMedia();
        if ((systemChanged || mediaChanged) && changedCallback_)
            changedCallback_(systemChanged, mediaChanged);
        for (int i = 0; i < 10 && !stopToken.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool SystemSnapshotService::SampleSystem()
{
    std::string errors;
    auto addError = [&errors](const char* message) {
        if (!errors.empty()) errors += "\n";
        errors += message;
    };

    CpuSnapshot cpu;
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    cpu.logicalProcessors = systemInfo.dwNumberOfProcessors;

    {
        static std::string cachedCpuName;
        if (cachedCpuName.empty())
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                0, KEY_READ, &key) == ERROR_SUCCESS)
            {
                wchar_t buf[256]{};
                DWORD size = sizeof(buf);
                if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS)
                {
                    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 1)
                    {
                        cachedCpuName.resize(static_cast<size_t>(len));
                        WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                            cachedCpuName.data(), len, nullptr, nullptr);
                        cachedCpuName.resize(static_cast<size_t>(len - 1));
                    }
                }
                RegCloseKey(key);
            }
        }
        cpu.name = cachedCpuName;
    }
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user))
    {
        const auto idleValue = FileTimeValue(idle);
        const auto kernelValue = FileTimeValue(kernel);
        const auto userValue = FileTimeValue(user);
        const auto totalDelta = (kernelValue - previousKernel_) + (userValue - previousUser_);
        const auto idleDelta = idleValue - previousIdle_;
        cpu.available = previousKernel_ != 0 && totalDelta > 0;
        if (cpu.available)
            cpu.usagePercent = std::clamp(100.0 * static_cast<double>(totalDelta - idleDelta) /
                static_cast<double>(totalDelta), 0.0, 100.0);
        previousIdle_ = idleValue;
        previousKernel_ = kernelValue;
        previousUser_ = userValue;
    }
    else
    {
        addError("CPU sampling failed.");
    }

    MemorySnapshot memory;
    MEMORYSTATUSEX memoryStatus{ sizeof(memoryStatus) };
    if (GlobalMemoryStatusEx(&memoryStatus))
    {
        memory.available = true;
        memory.totalBytes = memoryStatus.ullTotalPhys;
        memory.freeBytes = memoryStatus.ullAvailPhys;
        memory.usedBytes = memory.totalBytes - memory.freeBytes;
        memory.usagePercent = static_cast<double>(memoryStatus.dwMemoryLoad);
    }
    else
    {
        addError("Memory sampling failed.");
    }

    BatterySnapshot battery;
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power))
    {
        battery.available = power.BatteryFlag != 128 && power.BatteryLifePercent != 255;
        battery.percent = battery.available ? power.BatteryLifePercent : 0.0;
        battery.pluggedIn = power.ACLineStatus == 1;
        battery.charging = (power.BatteryFlag & 8) != 0;
        battery.saver = power.SystemStatusFlag != 0;
    }
    else
    {
        addError("Battery sampling failed.");
    }

    NetworkSnapshot network;
    const auto networkSampleTime = std::chrono::steady_clock::now();
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR && table)
    {
        network.available = true;
        for (ULONG i = 0; i < table->NumEntries; ++i)
        {
            const auto& row = table->Table[i];
            if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK ||
                row.OperStatus != IfOperStatusUp ||
                row.MediaConnectState != MediaConnectStateConnected)
                continue;
            network.connected = true;
            network.receivedBytes += row.InOctets;
            network.sentBytes += row.OutOctets;
        }
        FreeMibTable(table);
        const double elapsedSeconds = previousNetworkSample_.time_since_epoch().count() == 0
            ? 0.0
            : std::chrono::duration<double>(networkSampleTime - previousNetworkSample_).count();
        if (elapsedSeconds > 0.0 && previousReceived_ > 0 &&
            network.receivedBytes >= previousReceived_)
            network.downloadBytesPerSec = static_cast<std::uint64_t>(
                (network.receivedBytes - previousReceived_) / elapsedSeconds);
        if (elapsedSeconds > 0.0 && previousSent_ > 0 &&
            network.sentBytes >= previousSent_)
            network.uploadBytesPerSec = static_cast<std::uint64_t>(
                (network.sentBytes - previousSent_) / elapsedSeconds);
        previousReceived_ = network.receivedBytes;
        previousSent_ = network.sentBytes;
        previousNetworkSample_ = networkSampleTime;
    }
    else
    {
        addError("Network sampling failed.");
    }

    GpuSnapshot gpu;
    {
        Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            std::uint64_t bestVram = 0;
            for (UINT idx = 0; idx < 4; ++idx)
            {
                Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                if (FAILED(factory->EnumAdapters1(idx, &adapter))) break;

                DXGI_ADAPTER_DESC desc{};
                if (FAILED(adapter->GetDesc(&desc))) continue;
                if (desc.DedicatedVideoMemory == 0) continue;

                if (desc.DedicatedVideoMemory > bestVram)
                {
                    bestVram = desc.DedicatedVideoMemory;
                    gpu.available = true;
                    gpu.vramTotalBytes = desc.DedicatedVideoMemory;
                    int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0)
                    {
                        gpu.name.resize(len - 1);
                        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, &gpu.name[0], len, nullptr, nullptr);
                    }

                    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
                    adapter.As(&adapter3);
                }
                }
            }
        }

        if (gpu.available && !gpuQuery_)
            InitGpuPdH();
        if (gpu.available && gpuQuery_ && PdhCollectIfNeeded(gpuQuery_))
        {
            gpu.usagePercent = ReadGpuUtilization();
            std::uint64_t memBytes = ReadGpuMemBytes();
            if (memBytes > 0)
                gpu.vramUsedBytes = memBytes;
        }

    bool changed = false;
    {
        std::scoped_lock lock(mutex_);
        changed = !CpuEqual(cpu_, cpu) || !MemoryEqual(memory_, memory) ||
            !GpuEqual(gpu_, gpu) || !BatteryEqual(battery_, battery) ||
            !NetworkEqual(network_, network);
        cpu_ = cpu;
        memory_ = memory;
        gpu_ = gpu;
        battery_ = battery;
        network_ = network;
    }
    SetSystemError(errors);
    return changed;
}

bool SystemSnapshotService::SampleMedia()
{
    using namespace winrt::Windows::Media::Control;
    MediaSnapshot next;
    try
    {
        static GlobalSystemMediaTransportControlsSessionManager manager =
            GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto session = manager.GetCurrentSession();
        if (session)
        {
            next.available = true;
            next.sourceApp = session.SourceAppUserModelId().c_str();
            auto playback = session.GetPlaybackInfo();
            auto controls = playback.Controls();
            next.canPlayPause = controls.IsPlayPauseToggleEnabled();
            next.canNext = controls.IsNextEnabled();
            next.canPrevious = controls.IsPreviousEnabled();
            switch (playback.PlaybackStatus())
            {
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
                next.playbackStatus = "playing"; break;
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
                next.playbackStatus = "paused"; break;
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
                next.playbackStatus = "stopped"; break;
            default:
                next.playbackStatus = "closed"; break;
            }
            auto properties = session.TryGetMediaPropertiesAsync().get();
            next.title = properties.Title().c_str();
            next.artist = properties.Artist().c_str();
            next.album = properties.AlbumTitle().c_str();

            switch (pendingMediaAction_.exchange(MediaAction::None))
            {
            case MediaAction::PlayPause: session.TryTogglePlayPauseAsync().get(); break;
            case MediaAction::Next: session.TrySkipNextAsync().get(); break;
            case MediaAction::Previous: session.TrySkipPreviousAsync().get(); break;
            default: break;
            }
        }
        else
        {
            pendingMediaAction_.store(MediaAction::None);
        }
        SetMediaError({});
    }
    catch (const winrt::hresult_error& error)
    {
        SetMediaError("Media session: " + winrt::to_string(error.message()));
        pendingMediaAction_.store(MediaAction::None);
    }
    catch (...)
    {
        SetMediaError("Media session: unexpected failure.");
        pendingMediaAction_.store(MediaAction::None);
    }

    bool changed = false;
    {
        std::scoped_lock lock(mutex_);
        changed = !MediaEqual(media_, next);
        media_ = std::move(next);
    }
    return changed;
}
