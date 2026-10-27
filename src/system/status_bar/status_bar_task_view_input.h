#pragma once

#include "ui/input/low_level_mouse_hook.h"
#include "ui/render/ui_animation_scheduler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace snowdesktop
{
// Protect every visible bar while Shell creates its Task View input windows.
// Paired releases remain consumed after settling, cancellation or pointer exit.
class TaskViewPointerGuard
{
public:
    explicit TaskViewPointerGuard(std::vector<RECT> bounds) : bounds_(std::move(bounds)) {}

    void Arm(DWORD now, double remaining) noexcept
    {
        UpdateDeadline(now, remaining);
        state_.store(remaining > 0 ? kArmed : 0, std::memory_order_release);
    }
    void UpdateDeadline(DWORD now, double remaining) noexcept
    {
        if (remaining <= 0) { Cancel(); return; }
        // The hook also expires independently if the UI stops pumping timers.
        deadline_.store(now + static_cast<DWORD>(std::ceil((std::min)(remaining, 1500.0))),
            std::memory_order_release);
    }
    void Cancel() noexcept { state_.fetch_and(kButtons); }
    bool CanStop(DWORD now) noexcept
    {
        if ((state_.load() & kArmed) && Expired(now)) Cancel();
        return state_.load() == 0;
    }
    bool Filter(WPARAM message, POINT point, DWORD now, bool injected, DWORD mouseData = 0) noexcept
    {
        if (injected) return false;
        unsigned button = 0;
        bool down = false;
        switch (message)
        {
        case WM_LBUTTONDOWN: down = true; [[fallthrough]];
        case WM_LBUTTONUP: button = 1; break;
        case WM_RBUTTONDOWN: down = true; [[fallthrough]];
        case WM_RBUTTONUP: button = 2; break;
        case WM_MBUTTONDOWN: down = true; [[fallthrough]];
        case WM_MBUTTONUP: button = 4; break;
        case WM_XBUTTONDOWN: down = true; [[fallthrough]];
        case WM_XBUTTONUP: button = HIWORD(mouseData) == XBUTTON1 ? 8u : 16u; break;
        default: return false;
        }
        auto state = state_.load(std::memory_order_acquire);
        for (;;)
        {
            auto next = state;
            bool consume = true;
            if (down)
            {
                const bool expired = (state & kArmed) && Expired(now);
                consume = (state & kArmed) && !expired && Contains(point);
                if (expired) next &= ~kArmed;
                // A fresh physical press replaces a missing release (for example,
                // after switching devices). Never consume its release if its new
                // press is delivered; other buttons still own their paired releases.
                next = consume ? next | button : next & ~button;
            }
            else
            {
                if (!(state & button)) return false;
                next &= ~button;
            }
            if (state_.compare_exchange_weak(state, next)) return consume;
        }
    }
private:
    bool Contains(POINT point) const noexcept
    {
        for (const auto& bounds : bounds_) if (PtInRect(&bounds, point)) return true;
        return false;
    }
    bool Expired(DWORD now) const noexcept
    { return now - deadline_.load(std::memory_order_acquire) < 0x80000000u; }
    static constexpr unsigned kButtons = 31, kArmed = 32, kPending = 64;
    const std::vector<RECT> bounds_;
    std::atomic<DWORD> deadline_{0};
    std::atomic<unsigned> state_{kPending};
};

class StatusBarTaskViewInputHandoff
{
    struct Monitor
    {
        Monitor(std::vector<RECT> bounds, LowLevelMouseHook::Api api)
            : guard(std::move(bounds)), hook(api) {}
        ~Monitor()
        {
            hook.Stop(); // Join callbacks before clearing/releasing their owner.
            auto* expected = this;
            active_.compare_exchange_strong(expected, nullptr);
        }
        TaskViewPointerGuard guard;
        LowLevelMouseHook hook;
        bool armed = false; // UI thread only.
    };
public:
    // Installation and ownership run on the UI thread; input filtering runs on
    // LowLevelMouseHook's dedicated thread and never calls into DesktopApp.
    static std::shared_ptr<StatusBarTaskViewInputHandoff> Create(
        UiAnimationScheduler& scheduler, std::vector<RECT> bounds, std::function<double()> remaining,
        LowLevelMouseHook::Api api = {})
    {
        if (bounds.empty()) return {};
        auto monitor = std::make_shared<Monitor>(std::move(bounds), api);
        Monitor* expected = nullptr;
        if (!active_.compare_exchange_strong(expected, monitor.get())) return {};
        if (!monitor->hook.Start(GetModuleHandleW(nullptr), &MouseHook)) return {};
        // This independent lifetime outlasts shortcut completion/cancellation.
        // Expiry and a new swallowed press compete atomically, so cleanup can
        // never unhook between a consumed down and its matching physical up.
        const auto token = scheduler.ScheduleInterval(16, [&scheduler, monitor, remaining = std::move(remaining)](UiScheduleToken timer) {
            if (monitor->armed) monitor->guard.UpdateDeadline(GetTickCount(), remaining());
            if (monitor->guard.CanStop(GetTickCount())) scheduler.Cancel(timer);
        });
        if (!token) return {};
        return std::shared_ptr<StatusBarTaskViewInputHandoff>(
            new StatusBarTaskViewInputHandoff(std::move(monitor)));
    }
    ~StatusBarTaskViewInputHandoff() { ReleaseIfUnused(); }
    void Cancel() noexcept { monitor_->guard.Cancel(); }
    void ReleaseIfUnused() noexcept
    {
        // An unarmed request cannot have consumed input. Cancellation before
        // injection must not leave an idle monitor retained by the scheduler.
        if (!monitor_->armed) monitor_->guard.Cancel();
    }
    void Begin(double remaining) noexcept
    { monitor_->guard.Arm(GetTickCount(), remaining); monitor_->armed = true; }
private:
    explicit StatusBarTaskViewInputHandoff(std::shared_ptr<Monitor> monitor)
        : monitor_(std::move(monitor)) {}
    static LRESULT CALLBACK MouseHook(int code, WPARAM message, LPARAM data)
    {
        if (code == HC_ACTION && data)
        {
            const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            auto* monitor = active_.load();
            if (monitor && monitor->guard.Filter(message, event->pt, GetTickCount(),
                (event->flags & LLMHF_INJECTED) != 0, event->mouseData)) return 1;
        }
        return CallNextHookEx(nullptr, code, message, data);
    }
    inline static std::atomic<Monitor*> active_{nullptr};
    std::shared_ptr<Monitor> monitor_;
};
}
