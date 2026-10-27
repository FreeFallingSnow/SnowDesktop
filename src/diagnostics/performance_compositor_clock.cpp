#include "performance_compositor_clock.h"
#include "performance_trace.h"
#include "performance_trace_control.h"
#include "perfetto_categories.h"
#include <windows.h>
#include <dcomp.h>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <tuple>

namespace snowdesktop::performance
{
namespace
{
template<class Function> Function Load(HMODULE module, const char* name)
{
    const auto address = GetProcAddress(module, name);
    Function function{}; static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function)); return function;
}
}
struct CompositorClock::Impl
{
    HMODULE library = nullptr;
    decltype(&DCompositionGetFrameId) frameId = nullptr;
    decltype(&DCompositionGetStatistics) statistics = nullptr;
    decltype(&DCompositionGetTargetStatistics) targetStatistics = nullptr;
    COMPOSITION_FRAME_ID previous = 0;
    LARGE_INTEGER frequency{}, begin{};
    std::uint64_t skipped = 0, failures = 0;
    using Target = std::tuple<DWORD,LONG,DWORD,LONG,UINT,UINT,UINT>;
    std::map<Target, std::pair<std::uint64_t, UINT64>> targets;
    Impl()
    {
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&begin);
        library = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library)
        {
            frameId = Load<decltype(frameId)>(library, "DCompositionGetFrameId");
            statistics = Load<decltype(statistics)>(library, "DCompositionGetStatistics");
            targetStatistics = Load<decltype(targetStatistics)>(library, "DCompositionGetTargetStatistics");
        }
        if (!frameId || !statistics || !targetStatistics || frequency.QuadPart <= 0 || FAILED(frameId(COMPOSITION_FRAME_ID_COMPLETED, &previous)))
        { frameId = nullptr; Value("compositor.state", "supported", {}, 0); }
        else Value("compositor.state", "supported", {}, 1);
    }
    ~Impl() { if (library) FreeLibrary(library); }
    void Sample()
    {
        if (!frameId) return;
        Scope sampling("profiler", "sample.compositor");
        COMPOSITION_FRAME_ID current = 0;
        if (FAILED(frameId(COMPOSITION_FRAME_ID_COMPLETED, &current)) || current < previous)
        { ++failures; return; }
        auto first = previous + 1;
        if (current >= first && current - first >= 256) { skipped += current - first - 255; first = current - 255; }
        const auto started = std::chrono::steady_clock::now();
        for (auto frame = first; frame <= current; ++frame)
        {
            if (std::chrono::steady_clock::now() - started > std::chrono::milliseconds(10)) { skipped += current - frame + 1; break; }
            COMPOSITION_FRAME_STATS details{};
            std::array<COMPOSITION_TARGET_ID, 16> ids{}; UINT count = 0;
            if (FAILED(statistics(frame, &details, static_cast<UINT>(ids.size()), ids.data(), &count))) { ++failures; continue; }
            if (count > ids.size()) { ++skipped; count = static_cast<UINT>(ids.size()); }
            for (UINT i = 0; i < count; ++i)
            {
                COMPOSITION_TARGET_STATS values{};
                if (FAILED(targetStatistics(frame, &ids[i], &values))) { ++failures; continue; }
                if (values.presentTime < static_cast<UINT64>(begin.QuadPart) || !values.vblankDuration) continue;
                const auto& id = ids[i];
                const Target key{id.displayAdapterLuid.LowPart, id.displayAdapterLuid.HighPart, id.renderAdapterLuid.LowPart,
                    id.renderAdapterLuid.HighPart, id.vidPnSourceId, id.vidPnTargetId, id.uniqueId};
                auto found = targets.find(key);
                if (found == targets.end())
                {
                    if (targets.size() >= 16) { ++skipped; continue; }
                    found = targets.emplace(key, std::pair{OperationId(0xdca00000ULL + targets.size() + 1), UINT64{0}}).first;
                }
                if (found->second.second == values.presentTime) continue;
                found->second.second = values.presentTime;
                const auto ticksPerMs = static_cast<double>(frequency.QuadPart) / 1000.0;
                TRACE_EVENT_INSTANT("snow.frame", "compositor.present", "target", found->second.first,
                    "present_qpc_ms", static_cast<double>(values.presentTime) / ticksPerMs,
                    "vblank_ms", static_cast<double>(values.vblankDuration) / ticksPerMs,
                    "present_count", values.presentedStats.presentCount,
                    "refresh_count", values.presentedStats.refreshCount, "outstanding", values.outstandingPresents);
            }
            if (frame == current) break; // Do not wrap a UINT64 cursor.
        }
        previous = current;
        Value("compositor.state", "history_skipped_total", {}, static_cast<double>(skipped));
        Value("compositor.state", "query_failures_total", {}, static_cast<double>(failures));
    }
};
CompositorClock::CompositorClock() : impl_(std::make_unique<Impl>()) {}
CompositorClock::~CompositorClock() = default;
void CompositorClock::Sample() { impl_->Sample(); }
}
