#include "runtime_module_policy.h"

namespace snowdesktop::runtime_modules
{
std::atomic<std::uint32_t> policyMask{31};
std::atomic<std::uint64_t> policyRevision{0};
std::array<std::atomic<std::uint64_t>, kCount> moduleRevision{};
std::array<std::atomic<std::uint32_t>, kCount> lastError{};
std::array<std::atomic<unsigned>, kCount> active{};

bool Policy::Allows(Module module) const noexcept
{
    const auto index = static_cast<std::size_t>(module);
    return index < kCount && allowed[index];
}

std::uint64_t Revision(Module module) noexcept
{ return moduleRevision[static_cast<std::size_t>(module)].load(std::memory_order_acquire); }

bool Allowed(Module module) noexcept
{
    const auto index = static_cast<unsigned>(module);
    return index < kCount && (policyMask.load(std::memory_order_acquire) & (1u << index)) != 0;
}

void Apply(const Policy& policy) noexcept
{
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < kCount; ++i) if (policy.allowed[i]) mask |= 1u << i;
    const auto previous = policyMask.exchange(mask, std::memory_order_acq_rel);
    if (previous != mask)
    {
        for (std::size_t i = 0; i < kCount; ++i)
            if ((previous ^ mask) & (1u << i))
            {
                moduleRevision[i].fetch_add(1, std::memory_order_release);
                lastError[i].store(0, std::memory_order_release);
            }
        policyRevision.fetch_add(1, std::memory_order_release);
    }
}

bool ProtectAutoHide(bool dockEnabled, bool autoHide, bool suppress,
    const Policy& policy) noexcept
{
    return dockEnabled && policy.Allows(Module::Taskbar) &&
        policy.Allows(Module::WindowMinimize) && (autoHide || suppress);
}

void SetActive(Module module, unsigned count) noexcept
{ active[static_cast<std::size_t>(module)].store(count, std::memory_order_release); }

void SetError(Module module, std::uint32_t error) noexcept
{ lastError[static_cast<std::size_t>(module)].store(error, std::memory_order_release); }

Activity::Activity(Module module) : module_(module)
{ active[static_cast<std::size_t>(module_)].fetch_add(1); }
Activity::~Activity()
{ active[static_cast<std::size_t>(module_)].fetch_sub(1); }
}
