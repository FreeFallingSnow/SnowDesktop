#pragma once
#include "system_resource_view.h"
#include <set>

namespace snowdesktop::winui
{
// Explicit offline fixtures at the sampling boundary. Never construct the
// live provider, query devices or alter user subscription/settings state.
struct ResourcePreviewState
{
    widget_runtime::WidgetCpuDataSnapshot cpu;
    widget_runtime::WidgetMemoryDataSnapshot memory;
    widget_runtime::WidgetGpuDataSnapshot gpu;
    widget_runtime::WidgetNetworkTrafficDataSnapshot traffic;
    widget_runtime::WidgetResourceHistory history;
    std::set<std::string> subscriptions;
    unsigned closed = 0, reads = 0;
    std::int64_t now = 100000;
};
inline std::shared_ptr<ResourcePreviewState> ResourcePreviewFixture(std::string_view preset)
{
    auto state = std::make_shared<ResourcePreviewState>();
    state->cpu.available = true; state->cpu.warmingUp = false; state->cpu.usagePercent = preset == "idle" ? 0 : 13;
    state->cpu.logicalProcessors = 24; state->cpu.name = "Example 24-thread processor";
    state->memory.available = true; state->memory.totalBytes = 32ull << 30; state->memory.usedBytes = 12ull << 30;
    state->memory.freeBytes = 20ull << 30; state->memory.commitUsedBytes = 18ull << 30; state->memory.commitLimitBytes = 48ull << 30;
    state->traffic.available = true; state->traffic.warmingUp = false; state->traffic.connected = true;
    state->traffic.downloadBytesPerSecond = 2560000; state->traffic.uploadBytesPerSecond = 128000;
    state->traffic.receivedBytes = 12ull << 30; state->traffic.sentBytes = 1ull << 30;
    state->gpu.available = true; state->gpu.warmingUp = false;
    for (int i = 0; i < 2; ++i)
    {
        widget_runtime::WidgetGpuAdapterDataSnapshot adapter;
        adapter.id = "gpu-preview-" + std::to_string(i); adapter.name = i ? "Example integrated GPU" : "Example discrete GPU";
        adapter.usageAvailable = adapter.dedicatedUsageAvailable = adapter.sharedUsageAvailable = !(i && preset == "gpu-partial");
        adapter.usagePercent = i ? 61 : 23; adapter.dedicatedMemoryBytes = i ? 0 : 8ull << 30;
        adapter.dedicatedUsedBytes = i ? 0 : 2ull << 30; adapter.sharedUsedBytes = (i ? 512ull : 128ull) << 20;
        state->gpu.adapters.push_back(adapter);
    }
    for (int i = 0; i <= 60; ++i)
    {
        const auto time = state->now - 60000 + i * 1000;
        if (preset == "gap" && i >= 35 && i <= 39) continue;
        const auto percent = i == 60 ? state->cpu.usagePercent : 12 + (i % 9) * 2. + (i % 7) * .6;
        std::optional<double> cpu = preset == "idle" ? 0. : percent;
        if (preset == "gap" && i >= 20 && i <= 25) cpu.reset();
        state->history.Append("system.cpu", {}, {time, cpu, {}});
        state->history.Append("system.memory", {}, {time, 37.5 + (i % 6) * .2, {}});
        state->history.Append("system.network.traffic", {}, {time, i == 60 ? 2560000. : 1500000. + (i % 13) * 100000,
            i == 60 ? 128000. : 60000. + (i % 8) * 10000});
        for (int j = 0; j < 2; ++j)
            state->history.Append("system.gpu", "gpu-preview-" + std::to_string(j), {time,
                preset == "gpu-partial" && j ? std::nullopt : std::optional<double>(j ? 61 : 23), {}});
    }
    if (preset == "warming" || preset == "unavailable")
    {
        state->cpu.available = false; state->cpu.warmingUp = preset == "warming";
        state->history.Clear("system.cpu");
        state->history.Append("system.cpu", {}, {state->now, {}, {}});
    }
    return state;
}
inline SystemResourceSource ResourcePreviewSource(const std::shared_ptr<ResourcePreviewState>& state)
{
    SystemResourceSource source;
    source.cpu = [state] { ++state->reads; return state->cpu; };
    source.memory = [state] { ++state->reads; return state->memory; };
    source.gpu = [state] { ++state->reads; return state->gpu; };
    source.traffic = [state] { ++state->reads; return state->traffic; };
    source.history = [state](auto topic, auto identity) { ++state->reads; return state->history.Read(topic, identity); };
    source.subscribe = [state](auto topic) { state->subscriptions.emplace(topic); };
    source.close = [state] { ++state->closed; state->subscriptions.clear(); };
    source.now = [state] { return state->now; };
    return source;
}
}
