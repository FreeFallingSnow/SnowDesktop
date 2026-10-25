#include "performance_trace.h"
#include "performance_trace_control.h"
#include "perfetto_categories.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace snowdesktop::performance
{
namespace detail
{
struct DrawOrigins { std::array<std::uint64_t, 16> flows{}; std::size_t count = 0; };
struct TraceContext
{
    std::uint64_t generation = 0, salt = 0;
    std::vector<std::uint64_t> selected;
    std::atomic<std::uint64_t> nextOperation{ 1 };
    std::atomic<std::uint64_t> droppedOwners{ 0 }, droppedCounters{ 0 }, droppedLinks{ 0 }, probeErrors{ 0 };
    std::mutex identityMutex, counterMutex, drawMutex;
    std::unordered_map<std::uint64_t, std::wstring> owners;
    // Bounded identity and correlation registries, not event/timing storage.
    std::unordered_set<std::uint64_t> counters;
    std::unordered_map<std::uint64_t, DrawOrigins> origins;
};
}
namespace
{
std::atomic<bool> active{ false };
std::atomic<std::shared_ptr<detail::TraceContext>> context;
thread_local std::uint64_t currentOwner = 0, currentOperation = 0, currentGeneration = 0;
enum class Category : std::uint8_t { Resource, Lua, LuaDetail, Task, TaskDetail,
    Frame, Input, Dock, Popup, Layout, Render, Icon, Cache, Profiler };

std::uint64_t Hash(std::wstring_view text, std::uint64_t salt) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL ^ salt;
    for (const auto character : text) { hash ^= static_cast<std::uint16_t>(character); hash *= 1099511628211ULL; }
    return hash ? hash : 1;
}
std::uint64_t Hash(std::string_view text, std::uint64_t salt) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL ^ salt;
    for (const unsigned char character : text) { hash ^= character; hash *= 1099511628211ULL; }
    return hash ? hash : 1;
}
Category CategoryFor(std::string_view module) noexcept
{
    if (module == "profiler") return Category::Profiler;
    if (module == "lua" || module == "widget" || module.starts_with("widget.event")) return Category::Lua;
    if (module.starts_with("task.") || module.starts_with("shared.")) return Category::Task;
    if (module.find("memory") != module.npos || module == "process" ||
        module.ends_with(".state") || module == "widget.package") return Category::Resource;
    if (module.find("cache") != module.npos) return Category::Cache;
    if (module.find("layout") != module.npos) return Category::Layout;
    if (module.find("input") != module.npos) return Category::Input;
    if (module.starts_with("dock")) return Category::Dock;
    if (module.starts_with("popup")) return Category::Popup;
    if (module.starts_with("icon")) return Category::Icon;
    if (module.starts_with("desktop") || module.starts_with("composition")) return Category::Frame;
    return Category::Render;
}
bool Selected(const detail::TraceContext& state, std::uint64_t owner) noexcept
{ return !owner || state.selected.empty() || std::find(state.selected.begin(), state.selected.end(), owner) != state.selected.end(); }
std::uint64_t RegisterOwner(detail::TraceContext& state, std::wstring_view owner)
{
    if (owner.empty()) return 0;
    if (owner.size() > 256) { ++state.droppedOwners; return 0; }
    auto id = Hash(owner, state.salt);
    std::lock_guard lock(state.identityMutex);
    while (true)
    {
        const auto found = state.owners.find(id);
        if (found == state.owners.end()) break;
        if (found->second == owner) return id;
        ++id;
    }
    if (state.owners.size() >= 4096) { ++state.droppedOwners; return 0; }
    state.owners.emplace(id, owner);
    return id;
}

#define SNOW_TRACE_CATEGORIES(ACTION) \
    ACTION(Resource, "snow.resource") ACTION(Lua, "snow.lua.callback") \
    ACTION(LuaDetail, "snow.lua.detail") ACTION(Task, "snow.task") \
    ACTION(TaskDetail, "snow.task.detail") ACTION(Frame, "snow.frame") \
    ACTION(Input, "snow.input") ACTION(Dock, "snow.dock") ACTION(Popup, "snow.popup") \
    ACTION(Layout, "snow.layout") ACTION(Render, "snow.render") ACTION(Icon, "snow.icon") \
    ACTION(Cache, "snow.cache.detail") ACTION(Profiler, "snow.profiler")

bool Begin(Category category, std::string_view module, std::string_view phase,
    std::uint64_t owner, std::uint64_t operation)
{
#define SNOW_BEGIN_CASE(kind, label) case Category::kind: \
    if (!TRACE_EVENT_CATEGORY_ENABLED(label)) return false; \
    TRACE_EVENT_BEGIN(label, perfetto::DynamicString(phase.data(), phase.size()), \
        "module", perfetto::DynamicString(module.data(), module.size()), "owner", owner, "operation", operation); \
    return true;
    switch (category) { SNOW_TRACE_CATEGORIES(SNOW_BEGIN_CASE) }
#undef SNOW_BEGIN_CASE
    return false;
}
void End(Category category)
{
#define SNOW_END_CASE(kind, label) case Category::kind: TRACE_EVENT_END(label); break;
    switch (category) { SNOW_TRACE_CATEGORIES(SNOW_END_CASE) }
#undef SNOW_END_CASE
}
void Counter(Category category, const std::string& name, double value)
{
    const auto track = perfetto::CounterTrack(perfetto::DynamicString(name.data(), name.size()));
#define SNOW_COUNTER_CASE(kind, label) case Category::kind: TRACE_COUNTER(label, track, value); break;
    switch (category) { SNOW_TRACE_CATEGORIES(SNOW_COUNTER_CASE) }
#undef SNOW_COUNTER_CASE
}
#undef SNOW_TRACE_CATEGORIES
}

bool Enabled() noexcept { return active.load(std::memory_order_relaxed); }
std::uint64_t OwnerId(std::wstring_view owner) noexcept
{
    if (!Enabled()) return 0;
    const auto state = context.load();
    if (!state) return 0;
    try { return RegisterOwner(*state, owner); }
    catch (...) { ++state->probeErrors; return 0; }
}
Scope::Scope(std::string_view module, std::string_view phase,
    std::wstring_view owner, std::uint64_t correlation) noexcept
{
    if (!Enabled()) return;
    context_ = context.load();
    if (!context_) return;
    previousOwner_ = currentOwner;
    previousOperation_ = currentOperation;
    previousGeneration_ = currentGeneration;
    try
    {
        const bool shared = module.starts_with("shared.") || module == "composition.shared";
        currentOwner = owner.empty() ? (!shared && currentGeneration == context_->generation ? currentOwner : 0)
            : RegisterOwner(*context_, owner);
        currentOperation = correlation ? OperationId(correlation) :
            (!shared && currentGeneration == context_->generation && currentOperation ? currentOperation : context_->nextOperation.fetch_add(1));
        currentGeneration = context_->generation;
        const auto category = CategoryFor(module);
        category_ = static_cast<std::uint8_t>(category);
        if (Selected(*context_, currentOwner)) open_ = Begin(category, module, phase, currentOwner, currentOperation);
    }
    catch (...) { ++context_->probeErrors; }
}
Scope::~Scope()
{
    if (!context_) return;
    try { if (open_ && Enabled() && context.load() == context_) End(static_cast<Category>(category_)); }
    catch (...) { ++context_->probeErrors; }
    currentOwner = previousOwner_;
    currentOperation = previousOperation_;
    currentGeneration = previousGeneration_;
}
void Value(std::string_view module, std::string_view name, std::wstring_view owner,
    double value, std::uint64_t correlation) noexcept
{
    if (!Enabled()) return;
    const auto state = context.load();
    if (!state) return;
    try
    {
        if (!std::isfinite(value) || module.size() > 64 || name.size() > 128 ||
            module.find('|') != module.npos || name.find('|') != name.npos)
        { ++state->probeErrors; return; }
        const auto ownerId = RegisterOwner(*state, owner);
        if (module != "process" && !Selected(*state, ownerId)) return;
        char identity[17]{};
        std::snprintf(identity, sizeof(identity), "%016llx", static_cast<unsigned long long>(ownerId));
        std::string metric(name);
        if (module == "widget.package")
        {
            char package[17]{};
            std::snprintf(package, sizeof(package), "%016llx", static_cast<unsigned long long>(Hash(name, state->salt)));
            metric = "package-" + std::string(package);
        }
        const std::string label = std::string(module) + "|" + metric + "|" + identity;
        {
            std::lock_guard lock(state->counterMutex);
            const auto key = Hash(label, state->salt);
            if (!state->counters.contains(key) && state->counters.size() >= 4096)
            { ++state->droppedCounters; return; }
            state->counters.insert(key);
        }
        const auto category = module == "widget.composition" && (name == "surface_visible" || name == "surface_resident")
            ? Category::Resource : CategoryFor(module);
        Counter(category, label, value);
        if (correlation)
            TRACE_EVENT_INSTANT("snow.task", "numeric.operation", "module", perfetto::DynamicString(module.data(), module.size()),
                "operation", OperationId(correlation), "owner", ownerId);
    }
    catch (...) { ++state->probeErrors; }
}
void DrawLink(std::string_view surface, std::wstring_view owner, bool consume) noexcept
{
    if (!Enabled()) return;
    const auto state = context.load();
    if (!state) return;
    try
    {
        if (surface.size() > 64) { ++state->probeErrors; return; }
        const auto ownerId = RegisterOwner(*state, owner);
        if (!Selected(*state, ownerId)) return;
        const auto key = Hash(surface, ownerId);
        detail::DrawOrigins origins;
        std::uint64_t flow = 0;
        {
            std::lock_guard lock(state->drawMutex);
            const auto found = state->origins.find(key);
            if (consume)
            {
                if (found != state->origins.end()) { origins = found->second; state->origins.erase(found); }
            }
            else
            {
                if (found == state->origins.end() && state->origins.size() >= 1024)
                { ++state->droppedLinks; return; }
                auto& entry = state->origins[key];
                if (entry.count == entry.flows.size()) { ++state->droppedLinks; return; }
                flow = state->nextOperation.fetch_add(1);
                entry.flows[entry.count++] = flow;
            }
        }
        if (!consume)
            TRACE_EVENT_INSTANT("snow.frame", "draw.invalidate", perfetto::Flow::ProcessScoped(flow),
                "surface", perfetto::DynamicString(surface.data(), surface.size()), "owner", ownerId);
        else
            for (std::size_t index = 0; index < origins.count; ++index)
                TRACE_EVENT_INSTANT("snow.frame", "draw.consume", perfetto::TerminatingFlow::ProcessScoped(origins.flows[index]),
                    "surface", perfetto::DynamicString(surface.data(), surface.size()), "owner", ownerId,
                    "coalesced_count", origins.count);
    }
    catch (...) { ++state->probeErrors; }
}
void ActivateTrace(std::uint64_t generation, std::uint64_t salt, const std::vector<std::wstring>& selected)
{
    auto state = std::make_shared<detail::TraceContext>();
    state->generation = generation;
    state->salt = salt;
    for (const auto& owner : selected) state->selected.push_back(RegisterOwner(*state, owner));
    context.store(std::move(state));
    active.store(true, std::memory_order_release);
}
TraceQuality DeactivateTrace() noexcept
{
    active.store(false, std::memory_order_release);
    const auto state = context.exchange(nullptr);
    if (!state) return {};
    return { state->droppedOwners.load(), state->droppedCounters.load(), state->droppedLinks.load(), state->probeErrors.load() };
}
std::vector<TraceOwnerDescription> DescribeTraceOwners()
{
    std::vector<TraceOwnerDescription> result;
    const auto state = context.load();
    if (!state) return result;
    std::lock_guard lock(state->identityMutex);
    result.reserve(state->owners.size());
    for (const auto& [id, instance] : state->owners) result.push_back({ id, instance });
    return result;
}
std::uint64_t OperationId(std::uint64_t correlation) noexcept
{
    const auto state = context.load();
    if (!state || !correlation) return 0;
    auto value = correlation ^ state->salt;
    value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27; value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value ? value : 1;
}
}
