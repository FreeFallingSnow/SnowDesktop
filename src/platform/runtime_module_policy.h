#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace snowdesktop::runtime_modules
{
enum class Module : std::uint8_t { Taskbar, Tray, WindowMinimize, StartMenu, WallpaperPreview, Count };
inline constexpr std::size_t kCount = static_cast<std::size_t>(Module::Count);
inline constexpr std::array<std::string_view, kCount> kKeys{
    "taskbar", "tray", "windowMinimize", "startMenu", "wallpaperPreview"};
struct Policy
{
    std::array<bool, kCount> allowed{true, true, true, true, true};
    bool Allows(Module module) const noexcept
    {
        const auto index = static_cast<std::size_t>(module);
        return index < kCount && allowed[index];
    }
    bool operator==(const Policy&) const = default;
};
inline std::atomic<std::uint32_t> policyMask{31};
inline std::atomic<std::uint64_t> policyRevision{0};
inline std::array<std::atomic<std::uint64_t>, kCount> moduleRevision{};
inline std::uint64_t Revision(Module module) noexcept
{ return moduleRevision[static_cast<std::size_t>(module)].load(std::memory_order_acquire); }
inline std::array<std::atomic<std::uint32_t>, kCount> lastError{};
inline bool Allowed(Module module) noexcept
{
    const auto index = static_cast<unsigned>(module);
    return index < kCount && (policyMask.load(std::memory_order_acquire) & (1u << index)) != 0;
}
inline void Apply(const Policy& policy) noexcept
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
inline bool ProtectAutoHide(bool dockEnabled, bool autoHide, bool suppress,
    const Policy& policy = {}) noexcept
{
    return dockEnabled && policy.Allows(Module::Taskbar) &&
        policy.Allows(Module::WindowMinimize) && (autoHide || suppress);
}
// These leases describe live work, independently of whether a shared DLL is resident.
inline std::array<std::atomic<unsigned>, kCount> active{};
inline void SetActive(Module module, unsigned count) noexcept
{ active[static_cast<std::size_t>(module)].store(count, std::memory_order_release); }
inline void SetError(Module module, std::uint32_t error) noexcept
{ lastError[static_cast<std::size_t>(module)].store(error, std::memory_order_release); }
class Activity final
{
public:
    explicit Activity(Module module) : module_(module)
    { active[static_cast<std::size_t>(module_)].fetch_add(1); }
    ~Activity() { active[static_cast<std::size_t>(module_)].fetch_sub(1); }
    Activity(const Activity&) = delete;
    Activity& operator=(const Activity&) = delete;
private:
    Module module_;
};
}
