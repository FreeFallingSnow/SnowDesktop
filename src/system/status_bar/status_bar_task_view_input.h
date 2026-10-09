#pragma once

#include "ui/input/low_level_mouse_hook.h"
#include "ui/render/ui_animation_scheduler.h"

#include <atomic>
#include <memory>

namespace snowdesktop
{
// Protect the original button even after Shell covers it. Only physical left
// presses in that button during the double-click interval are consumed; their
// matching releases must be consumed even if the pointer moves or time expires.
class TaskViewPointerGuard
{
public:
    TaskViewPointerGuard(RECT button, DWORD interval) : button_(button), interval_(interval) {}

    void Arm(DWORD now) noexcept
    {
        startedAt_ = now;
        state_.store(State::Armed, std::memory_order_release);
    }
    void Cancel() noexcept
    {
        auto state = state_.load();
        while (state != State::CancelledPress && state != State::Finished &&
            !state_.compare_exchange_weak(state,
                state == State::Pressed ? State::CancelledPress : State::Finished)) {}
    }
    bool CanStop(DWORD now) noexcept
    {
        auto state = state_.load(std::memory_order_acquire);
        if (state == State::Armed && Expired(now))
            state_.compare_exchange_strong(state, State::Finished);
        return state_.load() == State::Finished;
    }
    bool Filter(WPARAM message, POINT point, DWORD now, bool injected) noexcept
    {
        if (injected || (message != WM_LBUTTONDOWN && message != WM_LBUTTONUP)) return false;
        auto state = state_.load(std::memory_order_acquire);
        if (state == State::Pressed || state == State::CancelledPress)
        {
            if (message == WM_LBUTTONUP)
            {
                // Cancel can race this physical release. Preserve cancellation
                // instead of rearming a request the UI has already abandoned.
                if (!state_.compare_exchange_strong(state,
                    state == State::CancelledPress || Expired(now) ? State::Finished : State::Armed))
                    state_.store(State::Finished);
            }
            return true;
        }
        if (state != State::Armed) return false;
        if (Expired(now))
        {
            state_.compare_exchange_strong(state, State::Finished);
            return false;
        }
        return message == WM_LBUTTONDOWN && PtInRect(&button_, point) &&
            state_.compare_exchange_strong(state, State::Pressed);
    }
private:
    enum class State { Inactive, Armed, Pressed, CancelledPress, Finished };
    bool Expired(DWORD now) const noexcept { return now - startedAt_ >= interval_; }
    const RECT button_;
    const DWORD interval_;
    DWORD startedAt_ = 0; // Published once by Arm; immutable while the hook runs.
    std::atomic<State> state_{State::Inactive};
};

class StatusBarTaskViewInputHandoff
{
    struct Monitor
    {
        Monitor(RECT button, UINT interval, LowLevelMouseHook::Api api)
            : guard(button, interval), hook(api) {}
        ~Monitor()
        {
            hook.Stop(); // Join callbacks before clearing/releasing their owner.
            auto* expected = this;
            active_.compare_exchange_strong(expected, nullptr);
        }
        TaskViewPointerGuard guard;
        LowLevelMouseHook hook;
    };
public:
    // Installation and ownership run on the UI thread; input filtering runs on
    // LowLevelMouseHook's dedicated thread and never calls into DesktopApp.
    static std::shared_ptr<StatusBarTaskViewInputHandoff> Create(
        UiAnimationScheduler& scheduler, RECT button, UINT interval,
        LowLevelMouseHook::Api api = {})
    {
        auto monitor = std::make_shared<Monitor>(button, interval, api);
        Monitor* expected = nullptr;
        if (!active_.compare_exchange_strong(expected, monitor.get())) return {};
        if (!monitor->hook.Start(GetModuleHandleW(nullptr), &MouseHook)) return {};
        // This independent lifetime outlasts shortcut completion/cancellation.
        // Expiry and a new swallowed press compete atomically, so cleanup can
        // never unhook between a consumed down and its matching physical up.
        const auto token = scheduler.ScheduleInterval(16, [&scheduler, monitor](UiScheduleToken timer) {
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
        if (!armed_) monitor_->guard.Cancel();
    }
    void Begin() noexcept { monitor_->guard.Arm(GetTickCount()); armed_ = true; }
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
                (event->flags & LLMHF_INJECTED) != 0)) return 1;
        }
        return CallNextHookEx(nullptr, code, message, data);
    }
    inline static std::atomic<Monitor*> active_{nullptr};
    std::shared_ptr<Monitor> monitor_;
    bool armed_ = false; // UI thread only.
};
}
