#pragma once
#include <windows.h>

namespace snowdesktop
{
// A pending message contains only its receiving HWND, never a Window pointer.
// A failed post remains retryable; removing its receiver permits handoff.
class StatusBarFullscreenRequest
{
public:
    template<class Post>
    bool Queue(HWND window, Post&& post)
    {
        if (!window || recipient_) return false;
        if (!post(window)) return false;
        recipient_ = window;
        return true;
    }
    bool Consume(HWND window)
    {
        if (!window || recipient_ != window) return false;
        recipient_ = nullptr;
        return true;
    }
private:
    HWND recipient_ = nullptr;
};
}
