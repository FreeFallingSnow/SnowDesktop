#pragma once

#include <windows.h>
#include <array>
#include <atomic>
#include <cstddef>

namespace snowdesktop::taskbar_hook
{
// Private Explorer-instance handoff, independent of the host IPC generation.
inline constexpr wchar_t kHookOwnerProperty[] = L"SnowDesktop.Taskbar.HookOwner.v1";
inline constexpr wchar_t kRetireHookMessageName[] = L"SnowDesktop.Taskbar.RetireHook.v1";
inline std::atomic_bool hookInstanceRetired{false};
inline std::atomic<DWORD> taskViewMonitorThread{0};

inline void MarkHookInstanceRetired() noexcept
{
    hookInstanceRetired.store(true);
    if (const DWORD thread = taskViewMonitorThread.load())
        PostThreadMessageW(thread, WM_QUIT, 0, 0);
}

// The owner serializes this set. Disable restores only our entry points; keep
// the MinHook records/trampolines and pinned DLL alive for calls already in
// flight. A later owner can then install its hooks on the original bytes.
template<std::size_t Capacity>
class RetainedHookSet
{
public:
    bool Contains(void* target) const noexcept
    {
        for (std::size_t i = 0; i < count_; ++i)
            if (targets_[i] == target) return true;
        return false;
    }

    bool Remember(void* target) noexcept
    {
        if (!target) return false;
        if (Contains(target)) return true;
        if (count_ == Capacity) return false;
        targets_[count_++] = target;
        return true;
    }

    template<typename Change>
    bool Disable(Change&& change) noexcept
    {
        bool stopped = true;
        for (std::size_t i = 0; i < count_; ++i)
        {
            if (!enabled_[i]) continue;
            if (change(targets_[i], false)) enabled_[i] = false;
            else stopped = false;
        }
        return stopped;
    }

    template<typename Change>
    bool Enable(Change&& change) noexcept
    {
        for (std::size_t i = 0; i < count_; ++i)
        {
            if (enabled_[i]) continue;
            if (!change(targets_[i], true))
            {
                Disable(change);
                return false;
            }
            enabled_[i] = true;
        }
        return true;
    }

private:
    std::array<void*, Capacity> targets_{};
    std::array<bool, Capacity> enabled_{};
    std::size_t count_ = 0;
};
}
