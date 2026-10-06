#pragma once
#include <windows.h>
#include <functional>
#include <utility>

namespace snowdesktop
{
// UI-thread only. A bar HWND owns one pending continuation; no heap payload is
// attached to the Windows message, so destruction and hiding need no drain.
class StatusBarActivationQueue
{
public:
    bool Post(HWND window, UINT message, std::function<void()> callback)
    {
        if (!window || !callback) return false;
        pending_ = std::move(callback);
        if (!posted_)
        {
            posted_ = PostMessageW(window, message, 0, 0) != FALSE;
            if (!posted_) pending_ = {};
        }
        return posted_;
    }
    std::function<void()> Take()
    {
        posted_ = false;
        return std::exchange(pending_, {});
    }
    void Cancel() { pending_ = {}; }
private:
    std::function<void()> pending_;
    bool posted_ = false;
};
}
