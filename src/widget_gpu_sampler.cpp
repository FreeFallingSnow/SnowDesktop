#include "widget_gpu_sampler.h"
#include "widget_gpu_usage.h"
#include "widget_gpu_counter_buffer.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <chrono>

namespace snowdesktop::widget_runtime
{
namespace
{
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
constexpr std::array<const wchar_t*, 3> Paths{
    L"\\GPU Engine(*)\\Utilization Percentage",
    L"\\GPU Adapter Memory(*)\\Dedicated Usage",
    L"\\GPU Adapter Memory(*)\\Shared Usage"};
std::string Utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), length, nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}
bool Valid(DWORD status) { return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA; }
std::uint64_t FileTime(const FILETIME& value)
{ return static_cast<std::uint64_t>(value.dwHighDateTime) << 32 | value.dwLowDateTime; }
}
struct WidgetGpuSampler::Impl
{
    struct Counter
    {
        HCOUNTER handle = nullptr;
        PDH_STATUS added = PDH_INVALID_HANDLE;
        WidgetGpuCounterBuffer formatted, raw;
    };
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIFactory7> notifications;
    HANDLE changed = nullptr;
    DWORD cookie = 0;
    bool registered = false;
    std::vector<WidgetGpuAdapterDataSnapshot> topology;
    HRESULT topologyStatus = S_OK;
    HQUERY query = nullptr;
    std::array<Counter, 3> counters;
    bool primed = false;
    Clock::time_point nextCounterAttempt{}, previousSample{};
    std::uint64_t wall = 0, awake = 0;
    WidgetGpuSamplingStatistics statistics;
    std::optional<WidgetGpuDiagnosticSample> diagnostic;
    ~Impl() { CloseQuery(); CloseTopology(); }
    void CloseQuery()
    {
        for (auto& counter : counters)
        {
            if (counter.handle) PdhRemoveCounter(counter.handle);
            counter.handle = nullptr;
        }
        if (query) PdhCloseQuery(query);
        query = nullptr; primed = false;
    }
    void CloseTopology()
    {
        if (registered) notifications->UnregisterAdaptersChangedEvent(cookie);
        registered = false; notifications.Reset();
        if (changed) CloseHandle(changed);
        changed = nullptr; factory.Reset(); topology.clear();
    }
    bool RefreshTopology()
    {
        if (factory && factory->IsCurrent() && (!changed || WaitForSingleObject(changed, 0) != WAIT_OBJECT_0)) return false;
        CloseTopology();
        ++statistics.topologyRefreshes;
        topologyStatus = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(topologyStatus)) return true;
        for (UINT index = 0; ; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const auto result = factory->EnumAdapters1(index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(result) || !adapter) { topologyStatus = FAILED(result) ? result : E_UNEXPECTED; break; }
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            WidgetGpuAdapterDataSnapshot entry;
            entry.luid = static_cast<std::uint64_t>(static_cast<std::uint32_t>(description.AdapterLuid.HighPart)) << 32 |
                description.AdapterLuid.LowPart;
            entry.id = WidgetGpuAdapterId(entry.luid); entry.name = Utf8(description.Description);
            entry.vendor = description.VendorId; entry.device = description.DeviceId;
            entry.dedicatedMemoryBytes = description.DedicatedVideoMemory;
            entry.sharedMemoryBytes = description.SharedSystemMemory;
            topology.push_back(std::move(entry));
        }
        if (FAILED(topologyStatus)) { CloseTopology(); return true; }
        if (SUCCEEDED(factory.As(&notifications)))
        {
            changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (changed) registered = SUCCEEDED(notifications->RegisterAdaptersChangedEvent(changed, &cookie));
            if (!registered && changed) { CloseHandle(changed); changed = nullptr; }
        }
        return true;
    }
    void EnsureCounters(Clock::time_point now)
    {
        if (now < nextCounterAttempt) return;
        nextCounterAttempt = now + std::chrono::seconds(5);
        if (!query)
        {
            const auto status = PdhOpenQueryW(nullptr, 0, &query);
            if (status != ERROR_SUCCESS)
            { for (auto& counter : counters) counter.added = status; return; }
            ++statistics.queryCreations;
        }
        bool any = false;
        for (std::size_t index = 0; index < counters.size(); ++index)
        {
            auto& counter = counters[index];
            if (!counter.handle)
            {
                counter.added = PdhAddEnglishCounterW(query, Paths[index], 0, &counter.handle);
                if (index == 0) primed = false;
            }
            any = any || counter.handle != nullptr;
        }
        if (!any) CloseQuery();
    }
    WidgetGpuDataSnapshot Sample(bool resetBaseline, bool captureRaw)
    {
        const auto start = Clock::now();
        ++statistics.samples;
        diagnostic = captureRaw ? std::optional<WidgetGpuDiagnosticSample>(std::in_place) : std::nullopt;
        WidgetGpuDataSnapshot result;
        result.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        ULONGLONG awakeNow = 0;
        const bool haveAwake = QueryUnbiasedInterruptTime(&awakeNow) != FALSE;
        const auto wallNow = GetTickCount64();
        const bool resumed = haveAwake && WidgetGpuResumed(wall, awake, wallNow, awakeNow / 10000);
        wall = haveAwake ? wallNow : 0; awake = awakeNow / 10000;
        const bool topologyChanged = RefreshTopology();
        if (resetBaseline || resumed || topologyChanged)
        { CloseQuery(); nextCounterAttempt = {}; }
        result.adapters = topology;
        LONGLONG collectedAt = 0;
        PDH_STATUS collected = PDH_NO_DATA;
        if (FAILED(topologyStatus))
        { result.error = "GPU adapter enumeration failed"; result.warmingUp = false; }
        else if (topology.empty())
        { result.error = "notPresent"; result.warmingUp = false; }
        else
        {
            EnsureCounters(start);
            collected = query ? PdhCollectQueryDataWithTime(query, &collectedAt) : PDH_INVALID_HANDLE;
            const bool readUsage = primed;
            result.warmingUp = counters[0].handle && !primed && collected == ERROR_SUCCESS;
            WidgetGpuUsageAccumulator usage;
            WidgetGpuMemoryAccumulator dedicated, shared;
            for (std::size_t index = 0; index < counters.size(); ++index)
            {
                auto& counter = counters[index];
                WidgetGpuCounterDiagnostic* detail = nullptr;
                if (diagnostic)
                {
                    detail = &diagnostic->counters.emplace_back();
                    detail->path = Paths[index]; detail->addStatus = static_cast<DWORD>(counter.added);
                }
                const auto before = counter.formatted.Growths() + counter.raw.Growths();
                PDH_STATUS formatted = counter.added;
                if (counter.handle && collected == ERROR_SUCCESS)
                {
                    const DWORD format = index == 0 ? PDH_FMT_DOUBLE | PDH_FMT_NOCAP100 : PDH_FMT_LARGE;
                    formatted = counter.formatted.ReadArray([&](DWORD* bytes, DWORD* count, void* values) {
                        return PdhGetFormattedCounterArrayW(counter.handle, format, bytes, count,
                            static_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(values));
                    });
                    if (formatted == ERROR_SUCCESS)
                    {
                        const auto* items = counter.formatted.Items<PDH_FMT_COUNTERVALUE_ITEM_W>();
                        for (DWORD row = 0; row < counter.formatted.Count(); ++row)
                        {
                            const auto& item = items[row];
                            if (detail) detail->formatted.push_back({item.szName ? item.szName : L"", item.FmtValue.CStatus,
                                index == 0 ? item.FmtValue.doubleValue : 0, index != 0 ? item.FmtValue.largeValue : 0});
                            if (!Valid(item.FmtValue.CStatus)) continue;
                            if (index == 0 && readUsage) usage.AddSample(item.szName, item.FmtValue.doubleValue);
                            else if (index == 1) dedicated.AddSample(item.szName, item.FmtValue.largeValue);
                            else if (index == 2) shared.AddSample(item.szName, item.FmtValue.largeValue);
                        }
                    }
                }
                else if (counter.handle) formatted = collected;
                if (detail)
                {
                    detail->formattedStatus = static_cast<DWORD>(formatted);
                    PDH_STATUS raw = counter.added;
                    if (counter.handle)
                    {
                        raw = counter.raw.ReadArray([&](DWORD* bytes, DWORD* count, void* values) {
                            return PdhGetRawCounterArrayW(counter.handle, bytes, count, static_cast<PDH_RAW_COUNTER_ITEM_W*>(values));
                        });
                        if (raw == ERROR_SUCCESS)
                        {
                            const auto* items = counter.raw.Items<PDH_RAW_COUNTER_ITEM_W>();
                            for (DWORD row = 0; row < counter.raw.Count(); ++row)
                            {
                                const auto& item = items[row];
                                detail->raw.push_back({item.szName ? item.szName : L"", item.RawValue.CStatus,
                                    item.RawValue.MultiCount, item.RawValue.FirstValue, item.RawValue.SecondValue, FileTime(item.RawValue.TimeStamp)});
                            }
                        }
                    }
                    detail->rawStatus = static_cast<DWORD>(raw);
                }
                statistics.bufferGrowths += counter.formatted.Growths() + counter.raw.Growths() - before;
            }
            bool anyUsage = false;
            const bool anyCompleteMemory = ApplyWidgetGpuMemory(result.adapters, dedicated, shared);
            for (auto& adapter : result.adapters)
            {
                const auto percent = usage.UsagePercent(adapter.luid);
                adapter.usageAvailable = percent.has_value(); adapter.usagePercent = percent.value_or(0);
                for (const auto& engine : usage.Engines(adapter.luid))
                    adapter.engines.push_back({engine.physical, engine.index, Utf8(engine.type),
                        engine.usagePercent, engine.rawTotal, engine.samples});
                anyUsage = anyUsage || adapter.usageAvailable;
            }
            // Preserve the existing Lua envelope; native views use independent
            // validity above, even when one memory counter is unavailable.
            result.available = anyUsage && anyCompleteMemory;
            if (collected != ERROR_SUCCESS)
            {
                result.error = "GPU utilization sampling failed"; result.warmingUp = false;
                CloseQuery();
            }
            else
            {
                primed = counters[0].handle != nullptr;
                if (!anyUsage && !result.warmingUp) result.error = "GPU utilization sampling unavailable";
                else if (!anyCompleteMemory) result.error = "GPU memory sampling unavailable";
            }
        }
        if (diagnostic)
        {
            diagnostic->statistics = statistics;
            diagnostic->collectStatus = static_cast<DWORD>(collected);
            diagnostic->topologyStatus = static_cast<DWORD>(topologyStatus);
            diagnostic->fileTime = static_cast<std::uint64_t>(collectedAt);
            diagnostic->resumed = resumed;
            diagnostic->elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
            diagnostic->intervalUs = previousSample == Clock::time_point{} ? 0 :
                std::chrono::duration_cast<std::chrono::microseconds>(start - previousSample).count();
        }
        previousSample = start;
        return result;
    }
};
WidgetGpuSampler::WidgetGpuSampler() = default;
WidgetGpuSampler::~WidgetGpuSampler() = default;
WidgetGpuDataSnapshot WidgetGpuSampler::Sample(bool resetBaseline, bool captureRaw)
{
    if (!impl_) impl_ = std::make_unique<Impl>();
    return impl_->Sample(resetBaseline, captureRaw);
}
void WidgetGpuSampler::Reset() { impl_.reset(); }
bool WidgetGpuSampler::Active() const noexcept { return impl_ && impl_->query != nullptr; }
const std::optional<WidgetGpuDiagnosticSample>& WidgetGpuSampler::Diagnostic() const
{
    static const std::optional<WidgetGpuDiagnosticSample> empty;
    return impl_ ? impl_->diagnostic : empty;
}
WidgetGpuSamplingStatistics WidgetGpuSampler::Statistics() const { return impl_ ? impl_->statistics : WidgetGpuSamplingStatistics{}; }
}
