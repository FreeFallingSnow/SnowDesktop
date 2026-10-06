#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct CpuSnapshot
{
    bool available = false;
    double usagePercent = 0.0;
    unsigned int logicalProcessors = 0;
    std::string name;
};

struct MemorySnapshot
{
    bool available = false;
    std::uint64_t totalBytes = 0;
    std::uint64_t usedBytes = 0;
    std::uint64_t freeBytes = 0;
    double usagePercent = 0.0;
};

struct GpuSnapshot
{
    bool available = false;
    std::string name;
    double usagePercent = 0.0;
    std::uint64_t vramTotalBytes = 0;
    std::uint64_t vramUsedBytes = 0;
};

struct BatterySnapshot
{
    bool available = false;
    double percent = 0.0;
    bool charging = false;
    bool pluggedIn = false;
    bool saver = false;
};

struct NetworkSnapshot
{
    bool available = false;
    bool connected = false;
    std::uint64_t downloadBytesPerSec = 0;
    std::uint64_t uploadBytesPerSec = 0;
    std::uint64_t receivedBytes = 0;
    std::uint64_t sentBytes = 0;
};

struct MediaSnapshot
{
    bool available = false;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring sourceApp;
    std::string playbackStatus = "closed";
    bool canPlayPause = false;
    bool canNext = false;
    bool canPrevious = false;
};

class SystemSnapshotService
{
public:
    using ChangedCallback = std::function<void(bool systemChanged, bool mediaChanged)>;

    SystemSnapshotService() = default;
    ~SystemSnapshotService();

    bool Start(ChangedCallback callback);
    void Stop();

    CpuSnapshot GetCpu() const;
    MemorySnapshot GetMemory() const;
    GpuSnapshot GetGpu() const;
    BatterySnapshot GetBattery() const;
    NetworkSnapshot GetNetwork() const;
    MediaSnapshot GetMedia() const;

    bool RequestMediaPlayPause();
    bool RequestMediaNext();
    bool RequestMediaPrevious();
    std::string GetLastError() const;

private:
    enum class MediaAction { None, PlayPause, Next, Previous };

    void WorkerMain(std::stop_token stopToken);
    bool SampleSystem();
    bool SampleMedia();
    void SetSystemError(const std::string& message);
    void SetMediaError(const std::string& message);

    mutable std::mutex mutex_;
    CpuSnapshot cpu_;
    MemorySnapshot memory_;
    GpuSnapshot gpu_;
    BatterySnapshot battery_;
    NetworkSnapshot network_;
    MediaSnapshot media_;
    std::string systemError_;
    std::string mediaError_;
    ChangedCallback changedCallback_;
    std::jthread worker_;
    std::atomic<MediaAction> pendingMediaAction_{ MediaAction::None };

    // PDH GPU monitoring
    void* gpuQuery_{};
    void* gpuUtilCounter_{};
    void* gpuMemCounter_{};
    bool InitGpuPdH();
    void CloseGpuPdH();
    double ReadGpuUtilization();
    std::uint64_t ReadGpuMemBytes();

    std::uint64_t previousIdle_ = 0;
    std::uint64_t previousKernel_ = 0;
    std::uint64_t previousUser_ = 0;
    std::uint64_t previousReceived_ = 0;
    std::uint64_t previousSent_ = 0;
    std::chrono::steady_clock::time_point previousNetworkSample_{};
};
