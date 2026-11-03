#pragma once

#include "runtime_module_types.h"
#include <atomic>

namespace snowdesktop::runtime_modules
{
extern std::atomic<std::uint32_t> policyMask;
extern std::atomic<std::uint64_t> policyRevision;
extern std::array<std::atomic<std::uint64_t>, kCount> moduleRevision;
extern std::array<std::atomic<std::uint32_t>, kCount> lastError;
// These leases describe live work, independently of a resident shared DLL.
extern std::array<std::atomic<unsigned>, kCount> active;

std::uint64_t Revision(Module module) noexcept;

bool Allowed(Module module) noexcept;

void Apply(const Policy& policy) noexcept;

bool ProtectAutoHide(bool dockEnabled, bool autoHide, bool suppress,
    const Policy& policy = {}) noexcept;

void SetActive(Module module, unsigned count) noexcept;

void SetError(Module module, std::uint32_t error) noexcept;

class Activity final
{
public:
    explicit Activity(Module module);
    ~Activity();
    Activity(const Activity&) = delete;
    Activity& operator=(const Activity&) = delete;
private:
    Module module_;
};
}
