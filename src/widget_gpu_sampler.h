#pragma once
#include "widget_gpu_usage.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetGpuEngineDataSnapshot
{
    std::uint32_t physical = 0, engine = 0;
    std::string type;
    double usagePercent = 0, rawTotal = 0;
    std::uint32_t samples = 0;
};
struct WidgetGpuAdapterDataSnapshot
{
    std::string id;
    std::string name;
    double usagePercent = 0.0;
    std::uint64_t dedicatedMemoryBytes = 0;
    std::uint64_t dedicatedUsedBytes = 0;
    std::uint64_t sharedMemoryBytes = 0;
    std::uint64_t sharedUsedBytes = 0;
    // Internal validity and diagnostic details. Existing Lua fields remain
    // unchanged until exposed through an explicitly versioned capability.
    bool usageAvailable = false;
    bool dedicatedUsageAvailable = false;
    bool sharedUsageAvailable = false;
    std::uint64_t luid = 0;
    std::uint32_t vendor = 0, device = 0;
    std::vector<WidgetGpuEngineDataSnapshot> engines;
};
struct WidgetGpuDataSnapshot
{
    bool available = false;
    bool warmingUp = true;
    std::vector<WidgetGpuAdapterDataSnapshot> adapters;
    std::int64_t timestampMs = 0;
    std::uint64_t revision = 0;
    std::string error;
};
inline bool ApplyWidgetGpuMemory(std::vector<WidgetGpuAdapterDataSnapshot>& adapters,
    const WidgetGpuMemoryAccumulator& dedicated, const WidgetGpuMemoryAccumulator& shared)
{
    bool anyComplete = false;
    for (auto& adapter : adapters)
    {
        const auto local = dedicated.UsageBytes(adapter.luid), system = shared.UsageBytes(adapter.luid);
        adapter.dedicatedUsageAvailable = local.has_value(); adapter.dedicatedUsedBytes = local.value_or(0);
        adapter.sharedUsageAvailable = system.has_value(); adapter.sharedUsedBytes = system.value_or(0);
        anyComplete = anyComplete || (local.has_value() && system.has_value());
    }
    return anyComplete;
}
struct WidgetGpuRawCounter
{
    std::wstring instance;
    std::uint32_t status = 0, multiCount = 0;
    std::int64_t first = 0, second = 0;
    std::uint64_t fileTime = 0;
};
struct WidgetGpuFormattedCounter
{
    std::wstring instance;
    std::uint32_t status = 0;
    double percent = 0;
    std::int64_t bytes = 0;
};
struct WidgetGpuCounterDiagnostic
{
    std::wstring path;
    std::uint32_t addStatus = 0, formattedStatus = 0, rawStatus = 0;
    std::vector<WidgetGpuFormattedCounter> formatted;
    std::vector<WidgetGpuRawCounter> raw;
};
struct WidgetGpuSamplingStatistics
{
    std::uint64_t samples = 0, queryCreations = 0, topologyRefreshes = 0, bufferGrowths = 0;
};
struct WidgetGpuDiagnosticSample
{
    WidgetGpuSamplingStatistics statistics;
    std::uint32_t collectStatus = 0, topologyStatus = 0;
    std::uint64_t fileTime = 0, elapsedUs = 0, intervalUs = 0;
    bool resumed = false;
    std::vector<WidgetGpuCounterDiagnostic> counters;
};
// Owned exclusively by the shared sampling worker. The optional diagnostic
// path uses the same collected interval; it does not start a second query.
class WidgetGpuSampler
{
public:
    WidgetGpuSampler();
    ~WidgetGpuSampler();
    WidgetGpuSampler(const WidgetGpuSampler&) = delete;
    WidgetGpuSampler& operator=(const WidgetGpuSampler&) = delete;
    WidgetGpuDataSnapshot Sample(bool resetBaseline = false, bool captureRaw = false);
    void Reset();
    bool Active() const noexcept;
    const std::optional<WidgetGpuDiagnosticSample>& Diagnostic() const;
    WidgetGpuSamplingStatistics Statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
