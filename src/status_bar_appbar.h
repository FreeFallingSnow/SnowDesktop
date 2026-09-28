#pragma once
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <functional>
#include <optional>

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
    UINT Edge() const { return edge_; }
    bool Registered() const { return registered_; }
    RECT RestoreWorkArea(RECT work) const;
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

inline bool StatusBarFullscreenCandidate(LONG_PTR style, LONG_PTR extendedStyle,
    bool maximized, bool shellWindow)
{
    // Full-screen-sized desktop/HUD helpers and ordinary decorated maximized
    // windows do not own the monitor's full-screen presentation.
    return !shellWindow && !(extendedStyle & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) &&
        !(maximized && (style & WS_CAPTION) == WS_CAPTION);
}

// Shared by the production fullscreen observer and its lifecycle regression.
// An explicit Dock summon may activate our input proxy and minimize an
// exclusive-fullscreen application. Keep that observation separate from the
// merged bar's explicit reveal policy. An independently revealed status bar
// owns its lifetime after Dock closes. Switching applications or restoring the
// source to an ordinary window ends retention. nullopt is an observation failure.
class StatusBarFullscreenState final
{
public:
    bool Observe(std::optional<HWND> source, bool dockPromoted,
        bool foregroundOwned, bool retainedSourceEligible)
    {
        if ((!dockPromoted && !revealed_) || !foregroundOwned || !retainedSourceEligible)
            dockSource_ = nullptr;
        if (source) source_ = *source;
        // A reveal is prepared before Dock changes focus. A refresh in that
        // interval must retain the still-observed source for the later handoff.
        if (revealed_ && source_) dockSource_ = source_;
        const bool fullscreen = source_ != nullptr || dockSource_ != nullptr;
        if (!fullscreen) revealed_ = false;
        return fullscreen;
    }
    void BeginDockReveal(bool revealIndependentBar = false)
    {
        if (!source_) return;
        dockSource_ = source_;
        if (revealIndependentBar) revealed_ = true;
    }
    bool Revealed() const { return revealed_; }
    void DismissReveal() { revealed_ = false; }
    bool ShouldHide(bool interacting) const { return Source() && !revealed_ && !interacting; }
    HWND DockSource() const { return dockSource_; }
    HWND Source() const { return source_ ? source_ : dockSource_; }
private:
    HWND source_ = nullptr;
    HWND dockSource_ = nullptr;
    bool revealed_ = false;
};

// AppBar registration is authoritative even while its window is hidden or
// a Dock reservation is being undone. Applying this twice is idempotent.
inline RECT ConstrainStatusBarWorkArea(RECT area, RECT reserved, UINT edge)
{
    RECT overlap{};
    if (!IntersectRect(&overlap, &area, &reserved)) return area;
    switch (edge)
    {
    case ABE_TOP: area.top = (std::min)(area.bottom, reserved.bottom); break;
    case ABE_BOTTOM: area.bottom = (std::max)(area.top, reserved.top); break;
    case ABE_LEFT: area.left = (std::min)(area.right, reserved.right); break;
    case ABE_RIGHT: area.right = (std::max)(area.left, reserved.left); break;
    }
    return area;
}

// Restore only a contiguous owned strip adjoining this exact work-area edge.
// An inner AppBar, another monitor, or an already-restored area must stay put.
inline RECT RestoreAppBarWorkArea(RECT work, RECT reserved, UINT edge)
{
    if (IsRectEmpty(&work) || IsRectEmpty(&reserved)) return work;
    const bool spansWidth = reserved.left <= work.left && reserved.right >= work.right;
    const bool spansHeight = reserved.top <= work.top && reserved.bottom >= work.bottom;
    switch (edge)
    {
    case ABE_TOP: if (spansWidth && work.top == reserved.bottom) work.top = reserved.top; break;
    case ABE_BOTTOM: if (spansWidth && work.bottom == reserved.top) work.bottom = reserved.bottom; break;
    case ABE_LEFT: if (spansHeight && work.left == reserved.right) work.left = reserved.left; break;
    case ABE_RIGHT: if (spansHeight && work.right == reserved.left) work.right = reserved.right; break;
    }
    return work;
}

inline RECT StatusBarAppBar::RestoreWorkArea(RECT work) const
{
    return registered_ && positioned_ ? RestoreAppBarWorkArea(work, approved_, edge_) : work;
}

// Record the actual edge delta, not the Shell rectangle's entire thickness:
// the original work area may already exclude part or all of that rectangle.
inline RECT AppBarReservedWorkArea(RECT before, RECT after, UINT edge)
{
    if (IsRectEmpty(&before)) return {};
    RECT reserved = before;
    switch (edge)
    {
    case ABE_TOP: reserved.bottom = (std::clamp)(after.top, before.top, before.bottom); break;
    case ABE_BOTTOM: reserved.top = (std::clamp)(after.bottom, before.top, before.bottom); break;
    case ABE_LEFT: reserved.right = (std::clamp)(after.left, before.left, before.right); break;
    case ABE_RIGHT: reserved.left = (std::clamp)(after.right, before.left, before.right); break;
    default: return {};
    }
    return IsRectEmpty(&reserved) ? RECT{} : reserved;
}

inline void ConstrainHiddenStatusBarPosition(bool hidden, WINDOWPOS& position)
{
    if (!hidden) return;
    position.flags = (position.flags & ~SWP_SHOWWINDOW) | SWP_HIDEWINDOW | SWP_NOACTIVATE;
    if (!(position.flags & SWP_NOZORDER)) position.hwndInsertAfter = HWND_NOTOPMOST;
}

inline bool StatusBarOwnsPointer(bool available, RECT client, RECT mergedDock, POINT point)
{
    return available && PtInRect(&client, point) &&
        (IsRectEmpty(&mergedDock) || !PtInRect(&mergedDock, point));
}
}
