#pragma once

#include "window_support_protocol.h"
#include "ui/input/low_level_mouse_hook.h"
#include <atomic>
#include <mutex>

namespace snowdesktop::window_support
{
class PointerObserver final
{
public:
    ~PointerObserver() { Stop(); }
    bool Start()
    {
        if (hook_) return true;
        PointerObserver* expected = nullptr;
        if (!active_.compare_exchange_strong(expected, this)) return false;
        if (!hook_.Start(GetModuleHandleW(nullptr), &MouseProc))
        { active_.store(nullptr); return false; }
        return true;
    }
    void Stop()
    {
        hook_.Stop();
        PointerObserver* expected = this;
        active_.compare_exchange_strong(expected, nullptr);
        std::lock_guard guard(mutex_);
        count_ = head_ = 0;
    }
    bool Available() const noexcept { return static_cast<bool>(hook_); }
    std::uint32_t Buttons() const { std::lock_guard guard(mutex_); return buttons_; }
    void Collect(protocol::Response& response)
    {
        std::lock_guard guard(mutex_);
        response.pointerCount = 0;
        while (count_ && response.pointerCount < response.pointerEvents.size())
        {
            response.pointerEvents[response.pointerCount++] = events_[head_];
            head_ = (head_ + 1) % events_.size();
            --count_;
        }
        response.buttons = buttons_;
    }
private:
    static LRESULT CALLBACK MouseProc(int code, WPARAM message, LPARAM data)
    {
        if (code == HC_ACTION && data)
            if (auto* observer = active_.load())
            {
                const auto& value = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
                const auto msg = static_cast<UINT>(message);
                std::uint32_t button = 0;
                switch (msg)
                {
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: button = 1; break;
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: button = 2; break;
                case WM_MBUTTONDOWN: case WM_MBUTTONUP: button = 4; break;
                case WM_XBUTTONDOWN: case WM_XBUTTONUP: button = HIWORD(value.mouseData) == XBUTTON1 ? 8u : 16u; break;
                default: break;
                }
                if (button)
                {
                    std::lock_guard guard(observer->mutex_);
                    const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
                        msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
                    if (down) observer->buttons_ |= button;
                    else observer->buttons_ &= ~button;
                    if (observer->count_ == observer->events_.size())
                    {
                        observer->head_ = (observer->head_ + 1) % observer->events_.size();
                        --observer->count_;
                    }
                    const auto index = (observer->head_ + observer->count_) % observer->events_.size();
                    observer->events_[index] = {++observer->sequence_, value.pt, msg,
                        value.time, value.mouseData, value.flags, observer->buttons_};
                    ++observer->count_;
                }
            }
        return CallNextHookEx(nullptr, code, message, data);
    }
    inline static std::atomic<PointerObserver*> active_{nullptr};
    LowLevelMouseHook hook_;
    mutable std::mutex mutex_;
    std::array<protocol::PointerEvent, protocol::MaximumPointerEvents> events_{};
    std::size_t head_ = 0, count_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint32_t buttons_ = 0;
};
}
