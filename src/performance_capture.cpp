#include "performance_capture.h"
#include "performance_trace.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>

#ifndef SNOWDESKTOP_VERSION
#define SNOWDESKTOP_VERSION "test"
#endif

namespace snowdesktop::performance
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::size_t MaximumGroups = 4096;
thread_local ScopeToken* currentScope = nullptr;

std::uint64_t Nanoseconds() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}
std::uint64_t FileTimeValue(FILETIME value) noexcept
{
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
        value.dwLowDateTime;
}
bool ThreadCpu(std::uint64_t& value) noexcept
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return false;
    value = (FileTimeValue(kernel) + FileTimeValue(user)) * 100;
    return true;
}
std::string Utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "invalid-utf16";
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        result.data(), size, nullptr, nullptr);
    return result;
}
std::string Quote(std::string_view value)
{
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value)
    {
        if (ch == '\"' || ch == '\\')
        {
            result += '\\';
            result += static_cast<char>(ch);
        }
        else if (ch < 32)
        {
            result += "\\u00";
            result += hex[ch >> 4];
            result += hex[ch & 15];
        }
        else result += static_cast<char>(ch);
    }
    return result + '\"';
}

struct Event
{
    std::uint64_t id = 0, parent = 0, correlation = 0, start = 0;
    std::uint64_t wall = 0, selfWall = 0, cpu = 0, selfCpu = 0;
    DWORD thread = 0;
    std::string module, phase, owner;
    bool gauge = false, cpuAvailable = false;
    double value = 0;
};
struct Aggregate
{
    std::uint64_t count = 0, wall = 0, selfWall = 0, cpu = 0, selfCpu = 0;
    std::uint64_t maximumWall = 0, cpuSamples = 0;
    std::uint64_t firstAt = 0, lastAt = 0;
    std::array<std::uint64_t, 32> histogram{};
    double minimum = 0, maximum = 0, last = 0;
};
using GroupKey = std::tuple<bool, std::string, std::string, std::string>;

using SummaryKey = std::tuple<std::string, std::string, std::wstring>;
struct SummaryKeyLess
{
    using is_transparent = void;
    template<typename A, typename B> bool operator()(const A& a, const B& b) const
    {
        const auto view = [](const auto& key) {
            return std::tuple<std::string_view, std::string_view, std::wstring_view>{
                std::get<0>(key), std::get<1>(key), std::get<2>(key) };
        };
        return view(a) < view(b);
    }
};
struct SummaryGroup
{
    std::string ownerUtf8;
    const std::wstring* owner = nullptr;
    Aggregate aggregate;
};
struct SummaryBuffer
{
    std::uint64_t generation = 0;
    std::mutex mutex;
    std::map<SummaryKey, SummaryGroup, SummaryKeyLess> groups;
};
thread_local std::weak_ptr<SummaryBuffer> localSummary;
std::atomic<std::uint64_t> summaryGeneration{ 0 };

struct State
{
    std::mutex mutex;
    std::mutex waitMutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<LRESULT> status{ ProtocolIdle };
    std::atomic<bool> stopRequested{ false };
    bool recording = false;
    std::uint64_t generation = 0, nextId = 0, started = 0, ended = 0;
    std::uint64_t droppedEvents = 0, droppedGroups = 0, probeErrors = 0;
    CaptureOptions options;
    std::vector<Event> events;
    std::map<GroupKey, Aggregate> groups;
    std::vector<std::shared_ptr<SummaryBuffer>> summaryBuffers;
    std::atomic<std::size_t> summaryGroupCount{ 0 };
    std::atomic<std::uint64_t> summaryDroppedGroups{ 0 };
    std::map<std::pair<std::string, std::string>, std::vector<std::uint64_t>> drawOrigins;
    std::uint64_t droppedLinks = 0;
    HANDLE output = INVALID_HANDLE_VALUE;
    std::filesystem::path partial;
    ~State()
    {
        stopRequested.store(true);
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
};
State state;

void RecordLocked(Event event)
{
    const GroupKey key{ event.gauge, event.module, event.phase, event.owner };
    auto found = state.groups.find(key);
    if (found == state.groups.end() && state.groups.size() < MaximumGroups)
        found = state.groups.emplace(key, Aggregate{}).first;
    if (found == state.groups.end()) ++state.droppedGroups;
    else
    {
        auto& group = found->second;
        if (group.count == 0)
        {
            group.minimum = group.maximum = event.value;
            group.firstAt = event.start;
        }
        group.lastAt = event.start;
        ++group.count;
        group.minimum = std::min(group.minimum, event.value);
        group.maximum = std::max(group.maximum, event.value);
        group.last = event.value;
        group.wall += event.wall;
        group.selfWall += event.selfWall;
        group.maximumWall = std::max(group.maximumWall, event.wall);
        if (event.cpuAvailable)
        {
            ++group.cpuSamples;
            group.cpu += event.cpu;
            group.selfCpu += event.selfCpu;
        }
        std::size_t bucket = 0;
        std::uint64_t upper = 1000;
        while (event.wall > upper && bucket + 1 < group.histogram.size())
        {
            ++bucket;
            upper *= 2;
        }
        ++group.histogram[bucket];
    }
    if (state.options.mode == CaptureMode::Summary) return;
    if (state.events.size() < state.options.maximumEvents)
        state.events.push_back(std::move(event));
    else ++state.droppedEvents;
}

void BeginScope(ScopeToken& token, std::string_view module,
    std::string_view phase, std::wstring_view owner,
    std::uint64_t correlation) noexcept
{
    try
    {
        std::lock_guard lock(state.mutex);
        if (!state.recording) return;
        token.module = std::string(module.substr(0, 256));
        token.phase = std::string(phase.substr(0, 512));
        token.owner = Utf8(owner.substr(0, 256));
        token.previous = currentScope;
        if (currentScope && currentScope->generation == state.generation)
        {
            token.parent = currentScope->id;
            if (token.owner.empty() && module == "lua")
                token.owner = currentScope->owner;
            if (!correlation) correlation = currentScope->correlation;
        }
        token.generation = state.generation;
        token.id = ++state.nextId;
        token.correlation = correlation;
        token.thread = GetCurrentThreadId();
        token.cpuAvailable = state.options.scopeCpu && ThreadCpu(token.cpuStart);
        token.start = Nanoseconds();
        currentScope = &token;
    }
    catch (...) { token.generation = 0; }
}

void EndScope(ScopeToken& token) noexcept
{
    if (!token.generation) return;
    const std::uint64_t ended = Nanoseconds();
    std::uint64_t cpuEnd = 0;
    const bool hasCpu = token.cpuAvailable && ThreadCpu(cpuEnd);
    currentScope = token.previous;
    const auto wall = ended - token.start;
    const auto cpu = hasCpu && cpuEnd >= token.cpuStart
        ? cpuEnd - token.cpuStart : 0;
    if (token.previous && token.previous->generation == token.generation)
    {
        token.previous->childWall += wall;
        token.previous->childCpu += cpu;
    }
    try
    {
        std::lock_guard lock(state.mutex);
        // A scope crossing stop/start must never contaminate a later session.
        if (!state.recording || token.generation != state.generation) return;
        Event event;
        event.id = token.id;
        event.parent = token.parent;
        event.correlation = token.correlation;
        event.start = token.start - state.started;
        event.wall = wall;
        event.selfWall = wall - std::min(wall, token.childWall);
        event.cpu = cpu;
        event.selfCpu = cpu - std::min(cpu, token.childCpu);
        event.cpuAvailable = hasCpu;
        event.thread = token.thread;
        event.module = std::move(token.module);
        event.phase = std::move(token.phase);
        event.owner = std::move(token.owner);
        RecordLocked(std::move(event));
    }
    catch (...) { /* Probes must not change application behavior. */ }
}

void RecordValue(std::string_view module, std::string_view phase,
    std::wstring_view owner, double value, std::uint64_t correlation) noexcept
{
    try
    {
        std::lock_guard lock(state.mutex);
        if (!state.recording || !std::isfinite(value)) return;
        Event event;
        event.id = ++state.nextId;
        event.start = Nanoseconds() - state.started;
        event.module = std::string(module.substr(0, 256));
        event.phase = std::string(phase.substr(0, 512));
        event.owner = Utf8(owner.substr(0, 256));
        event.gauge = true;
        event.value = value;
        event.thread = GetCurrentThreadId();
        event.correlation = correlation;
        if (currentScope && currentScope->generation == state.generation)
        {
            event.parent = currentScope->id;
            if (!correlation) event.correlation = currentScope->correlation;
        }
        RecordLocked(std::move(event));
    }
    catch (...) { /* A diagnostic allocation failure cannot fail a callback. */ }
}
void RecordDrawLink(std::string_view surface, std::wstring_view owner,
    bool consume) noexcept
{
    try
    {
        std::lock_guard lock(state.mutex);
        if (!state.recording || !currentScope ||
            currentScope->generation != state.generation) return;
        const auto key = std::make_pair(std::string(surface.substr(0, 64)), Utf8(owner.substr(0, 256)));
        if (!consume)
        {
            auto found = state.drawOrigins.find(key);
            if (found == state.drawOrigins.end() && state.drawOrigins.size() < 1024)
                found = state.drawOrigins.emplace(key, std::vector<std::uint64_t>{}).first;
            if (found == state.drawOrigins.end() || found->second.size() >= 16)
            { ++state.droppedLinks; return; }
            found->second.push_back(currentScope->id);
            return;
        }
        auto found = state.drawOrigins.find(key);
        if (found == state.drawOrigins.end()) return;
        for (auto source : found->second)
        {
            Event event;
            event.id = ++state.nextId;
            event.parent = currentScope->id;
            event.start = Nanoseconds() - state.started;
            event.module = "widget.draw.source";
            event.phase = key.first;
            event.owner = key.second;
            event.thread = GetCurrentThreadId();
            event.gauge = true;
            event.value = static_cast<double>(source);
            RecordLocked(std::move(event));
        }
        state.drawOrigins.erase(found);
    }
    catch (...) { /* Best-effort causality must not affect invalidation. */ }
}
void BeginSummary(ScopeToken& token, std::string_view module,
    std::string_view phase, std::wstring_view owner, std::uint64_t) noexcept
{
    try
    {
        const auto generation = summaryGeneration.load(std::memory_order_acquire);
        if (!generation) return;
        auto bufferRef = localSummary.lock();
        if (!bufferRef || bufferRef->generation != generation)
        {
            std::lock_guard lock(state.mutex);
            if (!state.recording || state.generation != generation) return;
            if (state.summaryBuffers.size() >= 64)
            { ++state.summaryDroppedGroups; return; }
            auto buffer = std::make_shared<SummaryBuffer>();
            buffer->generation = generation;
            state.summaryBuffers.push_back(buffer);
            bufferRef = std::move(buffer);
            localSummary = bufferRef;
        }
        module = module.substr(0, 256);
        phase = phase.substr(0, 512);
        owner = owner.substr(0, 256);
        if (owner.empty() && module == "lua" && currentScope &&
            currentScope->generation == generation && currentScope->summaryGroup)
            owner = *static_cast<SummaryGroup*>(currentScope->summaryGroup)->owner;
        auto& buffer = *bufferRef;
        std::lock_guard lock(buffer.mutex);
        if (summaryGeneration.load(std::memory_order_relaxed) != generation) return;
        auto found = buffer.groups.find(std::tuple{ module, phase, owner });
        if (found == buffer.groups.end())
        {
            if (state.summaryGroupCount.fetch_add(1) >= MaximumGroups)
            {
                --state.summaryGroupCount;
                ++state.summaryDroppedGroups;
                return;
            }
            found = buffer.groups.try_emplace(SummaryKey{
                std::string(module), std::string(phase), std::wstring(owner) }).first;
            found->second.owner = &std::get<2>(found->first);
            found->second.ownerUtf8 = Utf8(owner);
        }
        token.generation = generation;
        token.summaryGroup = &found->second;
        token.summaryBuffer = &buffer;
        token.summaryStorage = std::move(bufferRef);
        token.previous = currentScope;
        token.start = Nanoseconds();
        currentScope = &token;
    }
    catch (...) { ++state.summaryDroppedGroups; }
}

void EndSummary(ScopeToken& token) noexcept
{
    if (!token.generation) return;
    const auto ended = Nanoseconds();
    currentScope = token.previous;
    if (summaryGeneration.load(std::memory_order_acquire) != token.generation) return;
    const auto wall = ended - token.start;
    if (token.previous && token.previous->generation == token.generation)
        token.previous->childWall += wall;
    try
    {
        auto& buffer = *static_cast<SummaryBuffer*>(token.summaryBuffer);
        std::lock_guard lock(buffer.mutex);
        if (summaryGeneration.load(std::memory_order_relaxed) != token.generation) return;
        auto& group = static_cast<SummaryGroup*>(token.summaryGroup)->aggregate;
        ++group.count;
        group.wall += wall;
        group.selfWall += wall - std::min(wall, token.childWall);
        group.maximumWall = std::max(group.maximumWall, wall);
        std::size_t bucket = 0;
        std::uint64_t upper = 1000;
        while (wall > upper && bucket + 1 < group.histogram.size())
        { ++bucket; upper *= 2; }
        ++group.histogram[bucket];
    }
    catch (...) { ++state.summaryDroppedGroups; }
}

// Summary scopes retain per-thread aggregates only. Labels are interned once
// per group; hot calls do not allocate, transcode UTF-16, query thread CPU or
// contend on the global trace lock. A buffer lock protects stop/export races.
void MergeSummariesLocked()
{
    state.droppedGroups += state.summaryDroppedGroups.load();
    for (const auto& buffer : state.summaryBuffers)
    {
        std::lock_guard lock(buffer->mutex);
        for (const auto& [key, source] : buffer->groups)
        {
            if (!source.aggregate.count) continue;
            GroupKey targetKey{ false, std::get<0>(key), std::get<1>(key), source.ownerUtf8 };
            auto found = state.groups.find(targetKey);
            if (found == state.groups.end() && state.groups.size() >= MaximumGroups)
            { state.droppedGroups += source.aggregate.count; continue; }
            if (found == state.groups.end())
                found = state.groups.emplace(std::move(targetKey), Aggregate{}).first;
            auto& target = found->second;
            const auto& group = source.aggregate;
            target.count += group.count;
            target.wall += group.wall;
            target.selfWall += group.selfWall;
            target.maximumWall = std::max(target.maximumWall, group.maximumWall);
            for (std::size_t i = 0; i < group.histogram.size(); ++i)
                target.histogram[i] += group.histogram[i];
        }
        // Scope tokens own their buffer across stop; inactive threads retain
        // only a weak reference, so stopping releases unused group storage.
    }
}

void IgnoreDrawLink(std::string_view, std::wstring_view, bool) noexcept {}
const Hooks traceHooks{ BeginScope, EndScope, RecordValue, RecordDrawLink };
const Hooks summaryHooks{ BeginSummary, EndSummary, RecordValue, IgnoreDrawLink };

class GpuSampler
{
public:
    GpuSampler()
    {
        Scope setup("profiler", "gpu.initialize");
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return;
        Add(L"\\GPU Engine(*)\\Utilization Percentage", engine_);
        Add(L"\\GPU Process Memory(*)\\Dedicated Usage", dedicated_);
        Add(L"\\GPU Process Memory(*)\\Shared Usage", shared_);
        baseline_ = PdhCollectQueryData(query_) == ERROR_SUCCESS;
    }
    ~GpuSampler() { if (query_) PdhCloseQuery(query_); }
    void Sample()
    {
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS)
        {
            Value("process", "gpu_query_available", {}, 0);
            return;
        }
        if (!baseline_) { baseline_ = true; return; }
        const auto engineCount = Emit(engine_, "gpu_engine_percent");
        const auto dedicatedCount = Emit(dedicated_, "gpu_dedicated_bytes");
        const auto sharedCount = Emit(shared_, "gpu_shared_bytes");
        // No matching PID instance is unavailable, not a fabricated zero.
        Value("process", "gpu_engine_instances", {}, engineCount);
        Value("process", "gpu_dedicated_instances", {}, dedicatedCount);
        Value("process", "gpu_shared_instances", {}, sharedCount);
    }
private:
    void Add(const wchar_t* path, PDH_HCOUNTER& counter)
    {
        if (PdhAddEnglishCounterW(query_, path, 0, &counter) != ERROR_SUCCESS)
            counter = nullptr;
    }
    unsigned Emit(PDH_HCOUNTER counter, const char* name)
    {
        if (!counter) return 0;
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                &bytes, &count, nullptr) != PDH_MORE_DATA ||
            bytes > 16 * 1024 * 1024) return 0;
        std::vector<std::uint64_t> storage((bytes + 7) / 8);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                &bytes, &count, items) != ERROR_SUCCESS) return 0;
        const std::wstring prefix = L"pid_" +
            std::to_wstring(GetCurrentProcessId()) + L"_";
        unsigned matches = 0;
        for (DWORD i = 0; i < count; ++i)
        {
            const auto& item = items[i];
            if (!item.szName || !std::wstring_view(item.szName).starts_with(prefix) ||
                (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                 item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)) continue;
            Value("process", name, item.szName, item.FmtValue.doubleValue);
            ++matches;
        }
        return matches;
    }
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER engine_ = nullptr, dedicated_ = nullptr, shared_ = nullptr;
    bool baseline_ = false;
};

void SampleProcess()
{
    Scope scope("profiler", "sample.process");
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        Value("process", "cpu_total_ms", {},
            static_cast<double>(FileTimeValue(kernel) + FileTimeValue(user)) / 10000.0);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
    {
        Value("process", "private_commit_bytes", {}, static_cast<double>(memory.PrivateUsage));
        Value("process", "working_set_bytes", {}, static_cast<double>(memory.WorkingSetSize));
        Value("process", "page_fault_count", {}, memory.PageFaultCount);
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        Value("process", "handle_count", {}, handles);
}

std::string Report()
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(12);
    out << "{\"schemaVersion\":1,\"session\":" << Quote(state.options.session)
        << ",\"captureMode\":" << Quote(state.options.mode == CaptureMode::Summary ? "summary" : "trace")
        << ",\"scopeCpuEnabled\":" << (state.options.scopeCpu ? "true" : "false")
        << ",\"timelinePolicy\":" << Quote(state.options.mode == CaptureMode::Summary ? "none" : "prefix")
        << ",\"hostVersion\":" << Quote(SNOWDESKTOP_VERSION)
        << ",\"processId\":" << GetCurrentProcessId()
        << ",\"logicalProcessors\":" << GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
        << ",\"durationMs\":" << static_cast<double>(state.ended - state.started) / 1e6
        << ",\"stopReason\":" << Quote(state.stopRequested.load() ? "requested" : "duration")
        << ",\"eventLimit\":" << state.options.maximumEvents
        << ",\"droppedEvents\":" << state.droppedEvents
        << ",\"droppedGroups\":" << state.droppedGroups
        << ",\"droppedLinks\":" << state.droppedLinks
        << ",\"probeErrors\":" << state.probeErrors
        << ",\"limitations\":["
           "\"Scope CPU is optional; cpuSamples=0 means unavailable, not zero CPU. Enabled short-call CPU can quantize to zero.\","
           "\"Self timings exclude instrumented synchronous children only; inclusive rows overlap.\","
           "\"Task correlations are lifecycle links, not asynchronous CPU ownership.\","
           "\"GPU counters are per process/engine; shared DWM work and per-widget GPU time are not attributed.\","
           "\"Lua bytes exclude native heaps, textures and shared caches; surface bytes are estimates.\","
           "\"Trace mode retains a bounded timeline prefix; summary mode intentionally omits events and draw-source links.\","
           "\"Scopes spanning capture boundaries are omitted; counters are sampled, not exact peaks.\","
           "\"Profiler overhead is included in process totals; instrumentation itself is not fully timed.\"],\"groups\":[";
    bool comma = false;
    for (const auto& [key, group] : state.groups)
    {
        const auto& [gauge, module, phase, owner] = key;
        if (comma) out << ',';
        comma = true;
        out << "{\"kind\":" << Quote(gauge ? "gauge" : "scope")
            << ",\"module\":" << Quote(module) << ",\"phase\":" << Quote(phase)
            << ",\"owner\":" << Quote(owner) << ",\"count\":" << group.count;
        if (gauge)
            out << ",\"min\":" << group.minimum << ",\"max\":" << group.maximum
                << ",\"last\":" << group.last
                << ",\"firstAtMs\":" << static_cast<double>(group.firstAt) / 1e6
                << ",\"lastAtMs\":" << static_cast<double>(group.lastAt) / 1e6;
        else
        {
            std::uint64_t cumulative = 0, upper = 1000;
            for (std::size_t i = 0; i < group.histogram.size(); ++i)
            {
                cumulative += group.histogram[i];
                if (cumulative >= (group.count * 95 + 99) / 100) break;
                upper *= 2;
            }
            out << ",\"wallMs\":" << static_cast<double>(group.wall) / 1e6
                << ",\"selfWallMs\":" << static_cast<double>(group.selfWall) / 1e6
                << ",\"maxWallMs\":" << static_cast<double>(group.maximumWall) / 1e6
                << ",\"p95WallUpperMs\":" << static_cast<double>(upper) / 1e6
                << ",\"cpuMs\":" << static_cast<double>(group.cpu) / 1e6
                << ",\"selfCpuMs\":" << static_cast<double>(group.selfCpu) / 1e6
                << ",\"cpuSamples\":" << group.cpuSamples;
        }
        out << '}';
    }
    out << "],\"events\":[";
    comma = false;
    for (const auto& event : state.events)
    {
        if (comma) out << ',';
        comma = true;
        out << "{\"id\":" << event.id << ",\"parent\":" << event.parent
            << ",\"correlation\":" << event.correlation
            << ",\"thread\":" << event.thread
            << ",\"atMs\":" << static_cast<double>(event.start) / 1e6
            << ",\"module\":" << Quote(event.module)
            << ",\"phase\":" << Quote(event.phase)
            << ",\"owner\":" << Quote(event.owner)
            << ",\"kind\":" << Quote(event.gauge ? "gauge" : "scope");
        if (event.gauge) out << ",\"value\":" << event.value;
        else out << ",\"wallMs\":" << static_cast<double>(event.wall) / 1e6
            << ",\"selfWallMs\":" << static_cast<double>(event.selfWall) / 1e6
            << ",\"cpuMs\":" << static_cast<double>(event.cpu) / 1e6
            << ",\"selfCpuMs\":" << static_cast<double>(event.selfCpu) / 1e6
            << ",\"cpuAvailable\":" << (event.cpuAvailable ? "true" : "false");
        out << '}';
    }
    return out.str() + "]}";
}

void Worker() noexcept
{
    bool succeeded = false;
    try
    {
        const auto deadline = Clock::now() + std::chrono::seconds(state.options.seconds);
        SampleProcess();
        {
            // The query is session-local and is closed before report export.
            GpuSampler gpu;
            while (!state.stopRequested.load() && Clock::now() < deadline)
            {
                {
                    std::unique_lock lock(state.waitMutex);
                    state.wake.wait_until(lock, std::min(deadline,
                        Clock::now() + std::chrono::seconds(1)),
                        [] { return state.stopRequested.load(); });
                }
                SampleProcess();
                { Scope sample("profiler", "sample.gpu"); gpu.Sample(); }
            }
        }
    }
    catch (...) { ++state.probeErrors; }
    summaryGeneration.store(0, std::memory_order_release);
    captureHooks.store(nullptr, std::memory_order_release);
    {
        std::lock_guard lock(state.mutex);
        state.recording = false;
        state.ended = Nanoseconds();
        state.status.store(ProtocolFinishing);
        if (state.options.mode == CaptureMode::Summary) MergeSummariesLocked();
    }
    try
    {
        const auto report = Report();
        DWORD written = 0;
        succeeded = report.size() <= MAXDWORD &&
            WriteFile(state.output, report.data(), static_cast<DWORD>(report.size()),
                &written, nullptr) && written == report.size();
        if (succeeded) succeeded = FlushFileBuffers(state.output) != FALSE;
    }
    catch (...) { succeeded = false; }
    CloseHandle(state.output);
    state.output = INVALID_HANDLE_VALUE;
    if (succeeded)
        succeeded = MoveFileW(state.partial.c_str(), state.options.output.c_str()) != FALSE;
    // The final name is published only when the complete report is durable.
    // A failed partial is retained as evidence and never overwrites a report.
    {
        std::lock_guard lock(state.mutex);
        std::vector<Event>().swap(state.events);
        state.groups.clear();
        state.summaryBuffers.clear();
        state.drawOrigins.clear();
    }
    state.status.store(succeeded ? ProtocolIdle : ProtocolFailed);
}

UINT ControlMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(ControlMessageName);
    return message;
}
}

LRESULT Status() noexcept { return state.status.load(); }

bool Start(const CaptureOptions& options, std::string& error)
{
    error.clear();
    if (Status() == ProtocolRecording || Status() == ProtocolFinishing)
    { error = "captureBusy"; return false; }
    if (options.session.empty() || options.session.size() > 64 ||
        options.session.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos ||
        options.seconds < 1 || options.seconds > 600 ||
        options.maximumEvents < 1 || options.maximumEvents > 65536 ||
        (options.mode != CaptureMode::Trace && options.mode != CaptureMode::Summary) ||
        (options.mode == CaptureMode::Summary && options.scopeCpu) ||
        !options.output.is_absolute() || options.output.extension() != L".json")
    { error = "invalidCaptureOptions"; return false; }
    if (state.worker.joinable()) state.worker.join();
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(options.output.parent_path(), filesystemError) ||
        std::filesystem::exists(options.output, filesystemError) || filesystemError)
    { error = "outputMustBeNewInExistingDirectory"; return false; }
    const auto partial = std::filesystem::path(options.output.wstring() + L".partial");
    HANDLE output = CreateFileW(partial.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
    { error = "cannotReserveOutput"; return false; }
    try
    {
        std::lock_guard lock(state.mutex);
        state.options = options;
        state.partial = partial;
        state.events.clear();
        if (options.mode == CaptureMode::Trace) state.events.reserve(options.maximumEvents);
        state.groups.clear();
        state.summaryBuffers.clear();
        state.summaryGroupCount.store(0);
        state.summaryDroppedGroups.store(0);
        state.drawOrigins.clear();
        state.droppedLinks = 0;
        state.droppedEvents = state.droppedGroups = state.probeErrors = 0;
        ++state.generation;
        state.nextId = 0;
        state.started = Nanoseconds();
        state.stopRequested.store(false);
        state.output = output;
        state.recording = true;
        state.status.store(ProtocolRecording);
        summaryGeneration.store(options.mode == CaptureMode::Summary ? state.generation : 0,
            std::memory_order_release);
        captureHooks.store(options.mode == CaptureMode::Summary ? &summaryHooks : &traceHooks,
            std::memory_order_release);
        state.worker = std::thread(Worker);
        return true;
    }
    catch (...)
    {
        captureHooks.store(nullptr);
        summaryGeneration.store(0);
        state.recording = false;
        state.output = INVALID_HANDLE_VALUE;
        state.status.store(ProtocolFailed);
        CloseHandle(output);
        error = "captureAllocationFailed";
        return false;
    }
}

bool RequestStop(std::string_view session) noexcept
{
    std::lock_guard lock(state.mutex);
    if (!state.recording || session != state.options.session) return false;
    state.stopRequested.store(true);
    state.wake.notify_all();
    return true;
}
void Shutdown() noexcept
{
    state.stopRequested.store(true);
    state.wake.notify_all();
    if (state.worker.joinable()) state.worker.join();
}

bool IsControlMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    if (message == ControlMessage() && message != 0) return true;
    if (message == WM_TIMER && wParam == SampleTimer) return true;
    if (message != WM_COPYDATA || !lParam) return false;
    return reinterpret_cast<const COPYDATASTRUCT*>(lParam)->dwData == CopyDataTag;
}

LRESULT HandleControlMessage(HWND window, UINT message, WPARAM,
    LPARAM lParam, void (*sampleWidgets)(void*), void* context) noexcept
{
    try
    {
        if (message == ControlMessage()) return Status();
        if (message == WM_TIMER)
        {
            if (!Enabled()) KillTimer(window, SampleTimer);
            else if (sampleWidgets) sampleWidgets(context);
            return 1;
        }
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!data || data->dwData != CopyDataTag || !data->lpData ||
            data->cbData < sizeof(wchar_t) || data->cbData > 16384 ||
            data->cbData % sizeof(wchar_t) != 0) return 0;
        const auto* text = static_cast<const wchar_t*>(data->lpData);
        const auto length = data->cbData / sizeof(wchar_t);
        if (text[length - 1] != L'\0') return 0;
        std::wstring_view request(text, length - 1);
        if (request.find(L'\0') != std::wstring_view::npos) return 0;
        std::vector<std::wstring> fields;
        while (true)
        {
            const auto split = request.find(L'\n');
            fields.emplace_back(request.substr(0, split));
            if (fields.size() > 7) return 0;
            if (split == std::wstring_view::npos) break;
            request.remove_prefix(split + 1);
        }
        const bool version2 = fields.size() == 7 && fields[0] == L"2";
        if (!version2 && (fields.size() != 5 || fields[0] != L"1")) return 0;
        const auto session = Utf8(fields[2]);
        if (fields[1] == L"stop") return RequestStop(session) ? 1 : 0;
        if (fields[1] != L"start" || fields[3].empty() ||
            fields[3].size() > 3 ||
            fields[3].find_first_not_of(L"0123456789") != std::wstring::npos) return 0;
        CaptureOptions options;
        if (version2)
        {
            if ((fields[5] != L"summary" && fields[5] != L"trace") ||
                (fields[6] != L"0" && fields[6] != L"1")) return 0;
            options.mode = fields[5] == L"summary" ? CaptureMode::Summary : CaptureMode::Trace;
            options.scopeCpu = fields[6] == L"1";
        }
        options.session = session;
        options.seconds = static_cast<unsigned>(std::stoul(fields[3]));
        options.output = fields[4];
        std::string error;
        if (!Start(options, error)) return 0;
        if (!SetTimer(window, SampleTimer, 1000, nullptr))
        {
            (void)RequestStop(session);
            return 0;
        }
        if (sampleWidgets) sampleWidgets(context);
        return 1;
    }
    catch (...) { return 0; }
}
}
