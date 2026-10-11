#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>

namespace snowdesktop::tray
{
inline UINT ChangeMessage() { return RegisterWindowMessageW(L"SnowDesktop.Tray.Changed.v2"); }

// Owned under the Service mutex. Each subscriber has at most one queued
// change. A token rejects notifications left behind by a closed/reopened UI.
class ChangeNotifications
{
public:
    using Post = std::function<bool(HWND, std::uint64_t)>;
    explicit ChangeNotifications(Post post = [](HWND window, std::uint64_t token) {
        return PostMessageW(window, ChangeMessage(), static_cast<WPARAM>(token), 0) != FALSE;
    }) : post_(std::move(post)) {}
    void Subscribe(HWND window)
    {
        if (window) subscribers_[window] = {++nextToken_, false};
    }
    void Unsubscribe(HWND window) { subscribers_.erase(window); }
    void Publish()
    {
        for (auto& [window, subscriber] : subscribers_)
            if (!subscriber.pending) subscriber.pending = post_(window, subscriber.token);
    }
    bool Acknowledge(HWND window, std::uint64_t token)
    {
        const auto found = subscribers_.find(window);
        if (found == subscribers_.end() || found->second.token != token || !found->second.pending) return false;
        found->second.pending = false;
        return true;
    }
private:
    struct Subscriber { std::uint64_t token; bool pending; };
    inline static std::atomic<std::uint64_t> nextToken_{0};
    std::map<HWND, Subscriber> subscribers_;
    Post post_;
};
}
