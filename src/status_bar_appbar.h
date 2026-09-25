#pragma once
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <functional>

namespace snowdesktop
{
// Injectable Shell boundary: tests exercise the real registration/negotiation
// sequence without modifying the user's desktop work area.
class StatusBarAppBar
{
public:
    using Send = std::function<UINT_PTR(DWORD, APPBARDATA&)>;
    explicit StatusBarAppBar(Send send = [](DWORD message, APPBARDATA& data) {
        return SHAppBarMessage(message, &data);
    }) : send_(std::move(send)) {}
    ~StatusBarAppBar() { Remove(); }
    StatusBarAppBar(const StatusBarAppBar&) = delete;
    StatusBarAppBar& operator=(const StatusBarAppBar&) = delete;

    bool Place(HWND window, UINT callback, UINT edge, int thickness, RECT monitor)
    {
        if (!window || edge > ABE_BOTTOM || thickness <= 0 || IsRectEmpty(&monitor)) return false;
        data_.cbSize = sizeof(data_);
        data_.hWnd = window;
        data_.uCallbackMessage = callback;
        if (!registered_)
        {
            registered_ = send_(ABM_NEW, data_) != 0;
            if (!registered_) return false;
            positioned_ = false;
        }
        data_.uEdge = edge;
        data_.rc = monitor;
        SetThickness(data_.rc, edge, thickness);
        send_(ABM_QUERYPOS, data_);
        SetThickness(data_.rc, edge, thickness);
        if (!positioned_ || edge != edge_ || !EqualRect(&data_.rc, &approved_))
        {
            send_(ABM_SETPOS, data_);
            approved_ = data_.rc;
            edge_ = edge;
            positioned_ = !IsRectEmpty(&approved_);
        }
        return positioned_;
    }
    void Remove()
    {
        const bool registered = registered_;
        registered_ = false;
        positioned_ = false;
        approved_ = {};
        if (registered) send_(ABM_REMOVE, data_);
    }
    // Explorer lost its registrations; do not remove another generation's bar.
    void ExplorerRestarted() { registered_ = false; positioned_ = false; }
    void Notify(DWORD message)
    {
        if (registered_) send_(message, data_);
    }
    RECT Bounds() const { return approved_; }
    bool Registered() const { return registered_; }
    static void SetThickness(RECT& rect, UINT edge, int thickness)
    {
        switch (edge)
        {
        case ABE_LEFT: rect.right = rect.left + thickness; break;
        case ABE_RIGHT: rect.left = rect.right - thickness; break;
        case ABE_TOP: rect.bottom = rect.top + thickness; break;
        case ABE_BOTTOM: rect.top = rect.bottom - thickness; break;
        }
    }
private:
    Send send_;
    APPBARDATA data_{};
    RECT approved_{};
    UINT edge_ = ABE_TOP;
    bool registered_ = false, positioned_ = false;
};

inline bool StatusBarFullscreenClient(RECT client, RECT monitor, bool visible,
    bool minimized, bool cloaked, bool shellWindow)
{
    return visible && !minimized && !cloaked && !shellWindow &&
        !IsRectEmpty(&client) && !IsRectEmpty(&monitor) &&
        client.left <= monitor.left && client.top <= monitor.top &&
        client.right >= monitor.right && client.bottom >= monitor.bottom;
}
}
