#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace snowdesktop::performance
{
namespace detail { struct TraceContext; }
bool Enabled() noexcept;
std::uint64_t OwnerId(std::wstring_view instance) noexcept;

// SDK BEGIN/END enclose this object's synchronous lifetime. Capture-only native
// CPU/cycle snapshots preserve measured zeros; there is no private event store,
// timing stack or histogram beside the SDK.
class Scope final
{
public:
    Scope(std::string_view module, std::string_view phase,
        std::wstring_view owner = {}, std::uint64_t correlation = 0) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    std::shared_ptr<detail::TraceContext> context_;
    std::uint64_t previousOwner_ = 0, previousOperation_ = 0, previousGeneration_ = 0;
    std::uint8_t category_ = 0;
    bool open_ = false;
    std::uint64_t nativeCpuStart_ = 0, nativeCyclesStart_ = 0;
    std::uint32_t nativeThread_ = 0;
    bool nativeCpuSampled_ = false, nativeCyclesSampled_ = false;
};

// Gauges use native counter tracks; the finite event catalog uses instants.
// Cumulative resource semantics are explicit in the fixed analysis package.
void Value(std::string_view module, std::string_view name,
    std::wstring_view owner, double value, std::uint64_t correlation = 0) noexcept;
void DrawLink(std::string_view surface, std::wstring_view owner, bool consume) noexcept;
void TaskQueued(std::uint64_t id, std::string_view instanceUtf8) noexcept;
void TaskCompleted(std::uint64_t id, bool success) noexcept;
// Lives only around synchronous worker work; never across coroutine suspension.
class TaskExecution final
{
public:
    TaskExecution(std::uint64_t id, std::string_view worker) noexcept;
    ~TaskExecution();
    TaskExecution(const TaskExecution&) = delete;
    TaskExecution& operator=(const TaskExecution&) = delete;
private:
    std::shared_ptr<detail::TraceContext> context_;
    std::unique_ptr<Scope> scope_;
    std::uint64_t flow_ = 0, operation_ = 0, owner_ = 0;
};
}
