#include "dock_window_transition.h"

#include <algorithm>
#include <cmath>

namespace
{

constexpr wchar_t kDockWindowTransitionClassName[] =
    L"SnowDesktopDockWindowTransition";

bool IsUsableRect(const RECT& rect)
{
    return rect.right - rect.left > 1 &&
        rect.bottom - rect.top > 1;
}

bool SystemWindowAnimationsEnabled()
{
    BOOL compositionEnabled = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&compositionEnabled)) ||
        !compositionEnabled)
        return false;

    ANIMATIONINFO animationInfo{ sizeof(animationInfo) };
    if (SystemParametersInfoW(
            SPI_GETANIMATION, sizeof(animationInfo),
            &animationInfo, 0) &&
        animationInfo.iMinAnimate == 0)
        return false;

    BOOL clientAreaAnimation = TRUE;
    if (SystemParametersInfoW(
            SPI_GETCLIENTAREAANIMATION, 0,
            &clientAreaAnimation, 0) &&
        !clientAreaAnimation)
        return false;
    return true;
}

} // namespace

double EaseDockWindowTransition(double progress) noexcept
{
    progress = std::clamp(progress, 0.0, 1.0);
    return progress * progress * (3.0 - 2.0 * progress);
}

RECT InterpolateDockWindowTransitionRect(
    const RECT& from, const RECT& to, double progress) noexcept
{
    const double eased = EaseDockWindowTransition(progress);
    const auto interpolate = [eased](LONG start, LONG end) {
        return static_cast<LONG>(std::lround(
            start + (end - start) * eased));
    };
    RECT result{
        interpolate(from.left, to.left),
        interpolate(from.top, to.top),
        interpolate(from.right, to.right),
        interpolate(from.bottom, to.bottom)
    };
    if (result.right <= result.left)
        result.right = result.left + 1;
    if (result.bottom <= result.top)
        result.bottom = result.top + 1;
    return result;
}

DockWindowTransition::~DockWindowTransition()
{
    Cancel();
    if (hwnd_)
        DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (instance_)
        UnregisterClassW(
            kDockWindowTransitionClassName, instance_);
}

bool DockWindowTransition::Initialize(HINSTANCE instance)
{
    instance_ = instance;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName =
        kDockWindowTransitionClassName;
    return RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool DockWindowTransition::EnsureWindow()
{
    if (hwnd_ && IsWindow(hwnd_))
        return true;
    if (!instance_)
        return false;

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
            WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kDockWindowTransitionClassName,
        L"Dock Window Transition",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!hwnd_)
        return false;

    const DWM_WINDOW_CORNER_PREFERENCE corner =
        DWMWCP_ROUND;
    DwmSetWindowAttribute(
        hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corner, sizeof(corner));
    return true;
}

bool DockWindowTransition::StartMinimize(
    HWND sourceWindow, RECT dockRect)
{
    return Start(
        sourceWindow, dockRect,
        DockWindowTransitionDirection::Minimize,
        {});
}

bool DockWindowTransition::StartRestore(
    HWND sourceWindow, RECT dockRect,
    RestoreCallback restoreCallback)
{
    if (!restoreCallback)
        return false;
    return Start(
        sourceWindow, dockRect,
        DockWindowTransitionDirection::Restore,
        std::move(restoreCallback));
}

bool DockWindowTransition::Start(
    HWND sourceWindow, RECT dockRect,
    DockWindowTransitionDirection direction,
    RestoreCallback restoreCallback)
{
    Cancel();
    if (!SystemWindowAnimationsEnabled() ||
        !sourceWindow || !IsWindow(sourceWindow) ||
        !IsUsableRect(dockRect) || !EnsureWindow())
        return false;

    HWND root = GetAncestor(sourceWindow, GA_ROOT);
    sourceWindow_ = root ? root : sourceWindow;
    direction_ = direction;
    restoreCallback_ = std::move(restoreCallback);

    RECT windowRect{};
    if (direction_ == DockWindowTransitionDirection::Minimize)
    {
        if (!ResolveVisibleWindowRect(sourceWindow_, windowRect))
        {
            Cancel();
            return false;
        }
        lastVisibleRects_[sourceWindow_] = windowRect;
        fromRect_ = windowRect;
        toRect_ = dockRect;
    }
    else
    {
        if (!ResolveRestoreWindowRect(sourceWindow_, windowRect))
        {
            Cancel();
            return false;
        }
        fromRect_ = dockRect;
        toRect_ = windowRect;
    }

    const int initialWidth =
        std::max(1L, fromRect_.right - fromRect_.left);
    const int initialHeight =
        std::max(1L, fromRect_.bottom - fromRect_.top);
    SetWindowPos(
        hwnd_, HWND_TOPMOST,
        fromRect_.left, fromRect_.top,
        initialWidth, initialHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    if (FAILED(DwmRegisterThumbnail(
            hwnd_, sourceWindow_, &thumbnail_)) ||
        !thumbnail_)
    {
        Cancel();
        return false;
    }

    if (!ApplyFrame(0.0))
    {
        Cancel();
        return false;
    }

    const BOOL disableTransitions = TRUE;
    if (FAILED(DwmSetWindowAttribute(
            sourceWindow_,
            DWMWA_TRANSITIONS_FORCEDISABLED,
            &disableTransitions,
            sizeof(disableTransitions))))
    {
        Cancel();
        return false;
    }
    nativeTransitionsDisabled_ = true;
    UpdateWindow(hwnd_);
    DwmFlush();

    animationStartTick_ = GetTickCount64();
    awaitingRestoreVisibility_ = false;
    restoreCleanupDeadline_ = 0;
    if (!SetCoalescableTimer(
            hwnd_, kAnimationTimerId,
            kAnimationTimerIntervalMs, nullptr,
            TIMERV_NO_COALESCING))
    {
        Cancel();
        return false;
    }
    return true;
}

bool DockWindowTransition::ResolveVisibleWindowRect(
    HWND window, RECT& rect) const
{
    if (SUCCEEDED(DwmGetWindowAttribute(
            window, DWMWA_EXTENDED_FRAME_BOUNDS,
            &rect, sizeof(rect))) &&
        IsUsableRect(rect))
        return true;
    return GetWindowRect(window, &rect) &&
        IsUsableRect(rect);
}

bool DockWindowTransition::ResolveRestoreWindowRect(
    HWND window, RECT& rect) const
{
    const auto cached = lastVisibleRects_.find(window);
    if (cached != lastVisibleRects_.end() &&
        IsUsableRect(cached->second))
    {
        rect = cached->second;
        return true;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(window, &placement))
        return false;

    if ((placement.flags & WPF_RESTORETOMAXIMIZED) != 0)
    {
        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        const HMONITOR monitor = MonitorFromWindow(
            window, MONITOR_DEFAULTTONEAREST);
        if (GetMonitorInfoW(monitor, &monitorInfo))
        {
            rect = monitorInfo.rcWork;
            return IsUsableRect(rect);
        }
    }

    rect = placement.rcNormalPosition;
    return IsUsableRect(rect);
}

bool DockWindowTransition::ApplyFrame(double progress)
{
    if (!hwnd_ || !thumbnail_)
        return false;

    const RECT frame = InterpolateDockWindowTransitionRect(
        fromRect_, toRect_, progress);
    const int width = std::max(1L, frame.right - frame.left);
    const int height =
        std::max(1L, frame.bottom - frame.top);
    const double eased = EaseDockWindowTransition(progress);
    const double opacity =
        direction_ == DockWindowTransitionDirection::Minimize
            ? 255.0 * (1.0 - eased)
            : 255.0 * eased;
    const BYTE frameOpacity = static_cast<BYTE>(std::clamp(
        static_cast<int>(std::lround(opacity)), 0, 255));
    const bool geometryChanged =
        !hasLastFrame_ ||
        EqualRect(&frame, &lastFrameRect_) == FALSE;
    const bool opacityChanged =
        !hasLastFrame_ ||
        frameOpacity != lastFrameOpacity_;
    if (!geometryChanged && !opacityChanged)
        return true;

    if (geometryChanged)
    {
        SetWindowPos(
            hwnd_, nullptr,
            frame.left, frame.top, width, height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                SWP_NOZORDER | SWP_NOSENDCHANGING |
                SWP_NOCOPYBITS);
    }

    DWM_THUMBNAIL_PROPERTIES properties{};
    properties.dwFlags =
        DWM_TNP_RECTDESTINATION |
        DWM_TNP_VISIBLE |
        DWM_TNP_OPACITY |
        DWM_TNP_SOURCECLIENTAREAONLY;
    properties.rcDestination = { 0, 0, width, height };
    properties.opacity = frameOpacity;
    properties.fVisible = TRUE;
    properties.fSourceClientAreaOnly = FALSE;
    if (FAILED(DwmUpdateThumbnailProperties(
            thumbnail_, &properties)))
        return false;
    lastFrameRect_ = frame;
    lastFrameOpacity_ = frameOpacity;
    hasLastFrame_ = true;
    return true;
}

void DockWindowTransition::OnTimer()
{
    if (!sourceWindow_ || !IsWindow(sourceWindow_))
    {
        Finish();
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (awaitingRestoreVisibility_)
    {
        if (!IsIconic(sourceWindow_) ||
            now >= restoreCleanupDeadline_)
            Finish();
        return;
    }

    const double progress = std::min(
        1.0,
        static_cast<double>(now - animationStartTick_) /
            static_cast<double>(kAnimationDurationMs));
    if (!ApplyFrame(progress))
    {
        Finish();
        return;
    }
    if (progress < 1.0)
        return;

    if (direction_ == DockWindowTransitionDirection::Restore &&
        restoreCallback_)
    {
        RestoreCallback callback =
            std::move(restoreCallback_);
        callback(sourceWindow_);
        awaitingRestoreVisibility_ = true;
        restoreCleanupDeadline_ =
            now + kRestoreCleanupTimeoutMs;
        return;
    }
    Finish();
}

void DockWindowTransition::SetNativeTransitionsDisabled(
    bool disabled)
{
    if (!sourceWindow_ || !IsWindow(sourceWindow_))
    {
        nativeTransitionsDisabled_ = false;
        return;
    }
    const BOOL value = disabled ? TRUE : FALSE;
    DwmSetWindowAttribute(
        sourceWindow_,
        DWMWA_TRANSITIONS_FORCEDISABLED,
        &value, sizeof(value));
    nativeTransitionsDisabled_ = disabled;
}

void DockWindowTransition::UnregisterThumbnail()
{
    if (thumbnail_)
        DwmUnregisterThumbnail(thumbnail_);
    thumbnail_ = nullptr;
}

void DockWindowTransition::Finish()
{
    if (hwnd_)
    {
        KillTimer(hwnd_, kAnimationTimerId);
        ShowWindow(hwnd_, SW_HIDE);
    }
    UnregisterThumbnail();
    if (nativeTransitionsDisabled_)
        SetNativeTransitionsDisabled(false);
    sourceWindow_ = nullptr;
    lastFrameRect_ = {};
    lastFrameOpacity_ = 0;
    hasLastFrame_ = false;
    animationStartTick_ = 0;
    restoreCleanupDeadline_ = 0;
    awaitingRestoreVisibility_ = false;
    restoreCallback_ = {};
}

void DockWindowTransition::Cancel()
{
    Finish();
}

bool DockWindowTransition::IsActive() const
{
    return sourceWindow_ != nullptr &&
        thumbnail_ != nullptr;
}

LRESULT CALLBACK DockWindowTransition::WindowProc(
    HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<DockWindowTransition*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(
            lParam);
        self = static_cast<DockWindowTransition*>(
            create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    }

    switch (message)
    {
    case WM_TIMER:
        if (self && wParam == kAnimationTimerId)
        {
            self->OnTimer();
            return 0;
        }
        break;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        if (self)
            self->hwnd_ = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(
        window, message, wParam, lParam);
}
