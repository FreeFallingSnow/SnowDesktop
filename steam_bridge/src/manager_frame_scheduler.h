// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>

namespace snowdesktop::steam_bridge
{
inline constexpr std::uintptr_t kManagerLiveResizeTimer = 0x5344;
inline constexpr std::uintptr_t kManagerLanguagePollTimer = 0x5345;
inline constexpr std::uintptr_t kManagerInteractiveFrameTimer = 0x5346;
inline constexpr std::uintptr_t kManagerOcclusionProbeTimer = 0x5347;

enum class ManagerTimerAction
{
    None,
    RenderFrame,
    PollLanguage,
    RequestFrame,
    ProbeOcclusion,
};

constexpr ManagerTimerAction ClassifyManagerTimer(
    std::uintptr_t timer) noexcept
{
    if (timer == kManagerLiveResizeTimer)
        return ManagerTimerAction::RenderFrame;
    if (timer == kManagerLanguagePollTimer)
        return ManagerTimerAction::PollLanguage;
    if (timer == kManagerInteractiveFrameTimer)
        return ManagerTimerAction::RequestFrame;
    if (timer == kManagerOcclusionProbeTimer)
        return ManagerTimerAction::ProbeOcclusion;
    return ManagerTimerAction::None;
}

struct ManagerInteractiveFrameState
{
    bool foreground = false;
    bool visible = false;
    bool minimized = false;
    bool mouseDown = false;
    bool wantsTextInput = false;
};

constexpr std::uint32_t ManagerInteractiveFrameInterval(
    const ManagerInteractiveFrameState& state) noexcept
{
    if (!state.foreground || !state.visible || state.minimized) return 0;
    if (state.mouseDown) return 16;
    return state.wantsTextInput ? 250 : 0;
}

class ManagerFrameScheduler
{
public:
    bool RequestFrame() noexcept
    {
        bool expected = false;
        return requested_.compare_exchange_strong(expected, true,
            std::memory_order_release, std::memory_order_relaxed);
    }

    bool BeginFrame(bool canRender) noexcept
    {
        if (!canRender) return false;
        return requested_.exchange(false, std::memory_order_acq_rel);
    }

    bool IsFrameRequested() const noexcept
    {
        return requested_.load(std::memory_order_acquire);
    }

    bool TryQueueWake() noexcept
    {
        bool expected = false;
        return wakeQueued_.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    void AcknowledgeWake() noexcept
    {
        wakeQueued_.store(false, std::memory_order_release);
    }

    bool IsWakeQueued() const noexcept
    {
        return wakeQueued_.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool requested_ = false;
    std::atomic_bool wakeQueued_ = false;
};
}
