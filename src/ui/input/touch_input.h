#pragma once

#include <Windows.h>
#include <windowsx.h>
#include <algorithm>
#include <cstdlib>
#include <optional>

namespace snowdesktop::touch_input
{
inline thread_local bool touchMode = false;
inline thread_local ULONGLONG lastTouchPress = 0;
inline thread_local bool routingTouch = false;
inline thread_local WPARAM routedMouseButtons = 0;
inline thread_local POINT routedPoint{};
inline thread_local std::optional<bool> panHorizontal;

inline bool IsPointerMessage(UINT message) noexcept
{
    return message == WM_POINTERDOWN || message == WM_POINTERUPDATE ||
        message == WM_POINTERUP || message == WM_POINTERCAPTURECHANGED ||
        message == WM_POINTERENTER || message == WM_POINTERLEAVE;
}

inline bool IsTouchPointer(WPARAM pointer) noexcept
{
    POINTER_INPUT_TYPE type{};
    return GetPointerType(GET_POINTERID_WPARAM(pointer), &type) && type == PT_TOUCH;
}

inline bool IsPromotedTouch() noexcept
{
    constexpr ULONG_PTR mask = 0xffffff80;
    constexpr ULONG_PTR touchSignature = 0xff515780;
    return (static_cast<ULONG_PTR>(GetMessageExtraInfo()) & mask) == touchSignature;
}

inline bool Observe(UINT message, WPARAM pointer = 0) noexcept
{
    if (IsPointerMessage(message))
    {
        if (!IsTouchPointer(pointer)) return false;
        if (message == WM_POINTERDOWN || message == WM_POINTERUPDATE || message == WM_POINTERUP)
            touchMode = true;
        if (message == WM_POINTERDOWN || message == WM_POINTERUP) lastTouchPress = GetTickCount64();
        return true;
    }
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) lastTouchPress = 0;
    if (message < WM_MOUSEFIRST || message > WM_MOUSELAST) return false;
    INPUT_MESSAGE_SOURCE source{};
    const bool sourceKnown = GetCurrentInputMessageSource(&source) != FALSE;
    if (routingTouch || IsPromotedTouch() || (sourceKnown && source.deviceType == IMDT_TOUCH))
    {
        touchMode = true;
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) lastTouchPress = GetTickCount64();
        return true;
    }
    if (sourceKnown && (source.deviceType == IMDT_MOUSE || source.deviceType == IMDT_PEN ||
        source.deviceType == IMDT_TOUCHPAD))
    {
        touchMode = false;
        lastTouchPress = 0;
    }
    return false;
}

inline bool CanHover() noexcept { return !touchMode; }
inline bool RecentlyPressed() noexcept
{
    return lastTouchPress && GetTickCount64() - lastTouchPress <= 700;
}

class RoutedTouch final
{
public:
    explicit RoutedTouch(POINT screen, WPARAM buttons = 0, std::optional<bool> horizontal = std::nullopt) noexcept
        : previous_(routingTouch), previousButtons_(routedMouseButtons), previousPoint_(routedPoint),
          previousPan_(panHorizontal)
    {
        routingTouch = touchMode = true;
        routedMouseButtons = buttons;
        routedPoint = screen;
        panHorizontal = horizontal;
    }
    ~RoutedTouch()
    {
        routingTouch = previous_;
        routedMouseButtons = previousButtons_;
        routedPoint = previousPoint_;
        panHorizontal = previousPan_;
    }
    RoutedTouch(const RoutedTouch&) = delete;
    RoutedTouch& operator=(const RoutedTouch&) = delete;

private:
    bool previous_;
    WPARAM previousButtons_;
    POINT previousPoint_;
    std::optional<bool> previousPan_;
};

inline LRESULT RouteMouse(HWND window, UINT message, POINT screen, WPARAM buttons = 0)
{
    POINT client = screen;
    if (!ScreenToClient(window, &client)) return 0;
    const RoutedTouch scope(screen, buttons);
    return SendMessageW(window, message, buttons, MAKELPARAM(client.x, client.y));
}

inline void RouteClick(HWND window, POINT screen, bool context)
{
    RouteMouse(window, context ? WM_RBUTTONDOWN : WM_LBUTTONDOWN,
        screen, context ? MK_RBUTTON : MK_LBUTTON);
    if (IsWindow(window)) RouteMouse(window, context ? WM_RBUTTONUP : WM_LBUTTONUP, screen);
}

inline WPARAM PanWheel(int pixels) noexcept
{
    return MAKEWPARAM(0, 2 * std::clamp(pixels, -16384, 16383));
}

inline POINT ScreenPoint(LPARAM packed) noexcept
{
    return {GET_X_LPARAM(packed), GET_Y_LPARAM(packed)};
}

inline bool IsCanceled(WPARAM pointer) noexcept
{
    POINTER_INFO info{};
    return GetPointerInfo(GET_POINTERID_WPARAM(pointer), &info) &&
        (info.pointerFlags & POINTER_FLAG_CANCELED) != 0;
}

inline int Dips(int amount, UINT dpi) noexcept { return MulDiv(amount, dpi ? dpi : 96, 96); }

struct Contact
{
    UINT32 id = 0;
    POINT start{};
    POINT last{};
    ULONGLONG started = 0;

    bool Active() const noexcept { return id != 0; }
    bool Owns(WPARAM pointer) const noexcept { return Active() && id == GET_POINTERID_WPARAM(pointer); }
    void Begin(WPARAM pointer, POINT screen) noexcept
    {
        id = GET_POINTERID_WPARAM(pointer);
        start = last = screen;
        started = GetTickCount64();
    }
    void Reset() noexcept { *this = {}; }
    bool Moved(POINT point, int distance) const noexcept
    {
        return std::abs(point.x - start.x) >= distance || std::abs(point.y - start.y) >= distance;
    }
    bool Held() const noexcept { return GetTickCount64() - started >= 550; }
};

enum class GestureAction { Pending, Pan, Drag, Swipe, Consumed };

class TapSequence
{
public:
    void Reset() noexcept { window_ = nullptr; }
    bool Complete(HWND window, POINT screen) noexcept
    {
        const ULONGLONG now = GetTickCount64();
        const bool doubleTap = window == window_ && now - time_ <= GetDoubleClickTime() &&
            std::abs(screen.x - location_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
            std::abs(screen.y - location_.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        window_ = doubleTap ? nullptr : window;
        location_ = screen;
        time_ = now;
        return doubleTap;
    }

private:
    HWND window_ = nullptr;
    POINT location_{};
    ULONGLONG time_ = 0;
};

inline void ConfigureVerticalPan(HWND window)
{
    GESTURECONFIG config{GID_PAN, GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY,
        GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY};
    SetGestureConfig(window, 0, 1, &config, sizeof(config));
}

class VerticalPan
{
public:
    bool Read(LPARAM message, POINT& origin, int& delta)
    {
        GESTUREINFO info{};
        info.cbSize = sizeof(info);
        const auto handle = reinterpret_cast<HGESTUREINFO>(message);
        if (!GetGestureInfo(handle, &info) || info.dwID != GID_PAN) return false;
        CloseGestureInfoHandle(handle);
        const POINT point{info.ptsLocation.x, info.ptsLocation.y};
        if (!active_ || (info.dwFlags & GF_BEGIN)) origin_ = last_ = point;
        active_ = (info.dwFlags & GF_END) == 0;
        origin = origin_;
        delta = point.y - last_.y;
        last_ = point;
        touchMode = true;
        return true;
    }

private:
    bool active_ = false;
    POINT origin_{};
    POINT last_{};
};
}
