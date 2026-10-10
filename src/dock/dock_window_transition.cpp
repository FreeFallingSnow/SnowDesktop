#include "dock_window_transition.h"
#include "dock_minimize_protocol.h"
#include "dock_window_source_cloak.h"
#include "dock_window_rules.h"
#include "settings/animation_settings.h"
#include "platform/shell_overlay_window.h"
#include "diagnostics/performance_trace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>

namespace
{

constexpr wchar_t kDockWindowTransitionClassName[] =
    L"SnowDesktopDockWindowTransition";

BOOL CALLBACK CollectAnimationMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return FALSE;
    try { reinterpret_cast<std::vector<RECT>*>(parameter)->push_back(info.rcMonitor); }
    catch (...) { return FALSE; }
    return TRUE;
}

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

    return snowdesktop::animation::RuntimeAnimationsEnabled() &&
        snowdesktop::animation::RuntimeWindowEffect() != 0;
}

double TransitionDuration(int effect) noexcept
{
    return (effect == 3 ? 360.0 : effect == 2 ? 180.0 : 240.0) *
        snowdesktop::animation::RuntimeDurationScale();
}

double MonotonicTimeMilliseconds() noexcept
{
    LARGE_INTEGER counter{};
    static const double ticksPerMillisecond = [] {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) ||
            frequency.QuadPart <= 0)
            return 0.0;
        return static_cast<double>(
            frequency.QuadPart) / 1000.0;
    }();
    if (!QueryPerformanceCounter(&counter) ||
        ticksPerMillisecond <= 0.0)
    {
        return static_cast<double>(GetTickCount64());
    }
    return static_cast<double>(
        counter.QuadPart) / ticksPerMillisecond;
}

HRGN CreateDockWindowTransitionRegion(
    const RECT& bounds, int cornerRadius)
{
    if (cornerRadius <= 0)
    {
        return CreateRectRgn(
            bounds.left, bounds.top,
            bounds.right, bounds.bottom);
    }
    const int diameter = cornerRadius * 2;
    return CreateRoundRectRgn(
        bounds.left, bounds.top,
        bounds.right, bounds.bottom,
        diameter, diameter);
}

} // namespace

HRGN CreateDockWindowTransitionOcclusionRegion(
    const RECT& hostBounds, int cornerRadius,
    const std::vector<RECT>& occluders)
{
    const RECT localBounds{0, 0,
        hostBounds.right - hostBounds.left,
        hostBounds.bottom - hostBounds.top};
    HRGN region = CreateDockWindowTransitionRegion(localBounds, cornerRadius);
    if (!region)
        return nullptr;
    for (const RECT& occluder : occluders)
    {
        RECT intersection{};
        if (!IntersectRect(&intersection, &hostBounds, &occluder))
            continue;
        OffsetRect(&intersection, -hostBounds.left, -hostBounds.top);
        HRGN excluded = CreateRectRgnIndirect(&intersection);
        const bool succeeded = excluded &&
            CombineRgn(region, region, excluded, RGN_DIFF) != ERROR;
        if (excluded)
            DeleteObject(excluded);
        if (!succeeded)
        {
            DeleteObject(region);
            return nullptr;
        }
    }
    return region;
}

bool DockWindowTransition::ApplyOcclusion(
    const RECT& hostBounds, int cornerRadius)
{
    const auto occluders = occlusionRectsProvider_
        ? occlusionRectsProvider_() : std::vector<RECT>{};
    if (hasOcclusionRegion_ && cornerRadius == occlusionCornerRadius_ &&
        EqualRect(&hostBounds, &occlusionHostBounds_) &&
        occluders.size() == occlusionRects_.size() &&
        std::equal(occluders.begin(), occluders.end(), occlusionRects_.begin(),
            [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }))
        return true;

    HRGN region = CreateDockWindowTransitionOcclusionRegion(
        hostBounds, cornerRadius, occluders);
    if (!region)
        return false;
    if (!SetWindowRgn(hwnd_, region, FALSE))
    {
        DeleteObject(region);
        return false;
    }
    occlusionHostBounds_ = hostBounds;
    occlusionCornerRadius_ = cornerRadius;
    occlusionRects_ = occluders;
    hasOcclusionRegion_ = true;
    return true;
}

void DockWindowTransition::RefreshOcclusion()
{
    if (!presenting_ || !hwnd_)
        return;
    const RECT bounds = UsesCompositionImage()
        ? imageHostRect_ : lastFrameRect_;
    const int radius = UsesCompositionImage()
        ? 0 : ResolveDockWindowTransitionCornerRadius(bounds, dockRect_);
    if (!ApplyOcclusion(bounds, radius))
        CompleteImmediately();
}

void DockWindowTransition::LogPresentation(int requestedEffect,
    const wchar_t* fallbackStage)
{
    if (!diagnosticCallback_)
        return;
    wchar_t message[512]{};
    swprintf_s(message,
        L"Dock transition: hwnd=%p direction=%ls requested=%d effective=%d "
        L"image=%ls fallback=%ls result=0x%08lX",
        static_cast<void*>(sourceWindow_),
        direction_ == DockWindowTransitionDirection::Minimize ? L"minimize" : L"restore",
        requestedEffect, effect_, imageSource_,
        fallbackStage, static_cast<unsigned long>(imageResult_));
    diagnosticCallback_(message);
}

double EaseDockWindowTransition(double progress) noexcept
{
    progress = std::clamp(progress, 0.0, 1.0);
    return progress * progress * (3.0 - 2.0 * progress);
}

BYTE ResolveDockWindowTransitionOpacity(
    DockWindowTransitionDirection direction,
    double progress) noexcept
{
    const double eased =
        EaseDockWindowTransition(progress);
    const double opacity =
        direction ==
            DockWindowTransitionDirection::Minimize
        ? 255.0 * (1.0 - eased)
        : 255.0 * eased;
    return static_cast<BYTE>(std::clamp(
        static_cast<int>(std::lround(opacity)),
        0, 255));
}

int ResolveDockWindowTransitionCornerRadius(
    const RECT& frame,
    const RECT& dockRect) noexcept
{
    const int frameShortSide = std::min(
        std::max(0L, frame.right - frame.left),
        std::max(0L, frame.bottom - frame.top));
    const int dockShortSide = std::min(
        std::max(0L, dockRect.right - dockRect.left),
        std::max(0L, dockRect.bottom - dockRect.top));
    if (frameShortSide <= 1 ||
        dockShortSide <= 1)
        return 0;

    // Deriving the radius from the Dock target makes the mask naturally follow
    // the monitor DPI because Dock geometry is already expressed in physical
    // pixels. Keep tiny targets useful and guard against malformed giant ones.
    const int desiredRadius = std::clamp(
        static_cast<int>(std::lround(
            dockShortSide * 0.18)),
        4, 48);
    return std::min(
        desiredRadius,
        frameShortSide / 2);
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

RECT ResolveDockWindowImageHostRect(
    const RECT& from, const RECT& to) noexcept
{
    return {
        std::min(from.left, to.left),
        std::min(from.top, to.top),
        std::max(from.right, to.right),
        std::max(from.bottom, to.bottom)
    };
}

std::optional<RECT> ResolveDockWindowNonFullscreenHostRect(
    RECT host, std::span<const RECT> monitors) noexcept
{
    const auto valid = [](RECT rect) { return rect.right > rect.left && rect.bottom > rect.top; };
    if (!valid(host) || monitors.empty()) return std::nullopt;
    const auto covers = [](RECT rect, RECT monitor) {
        return rect.left <= monitor.left && rect.top <= monitor.top &&
            rect.right >= monitor.right && rect.bottom >= monitor.bottom;
    };
    std::array<RECT, 4> candidates{host, host, host, host};
    bool fullscreen = false;
    for (const RECT monitor : monitors)
    {
        if (!valid(monitor)) return std::nullopt;
        if (!covers(host, monitor)) continue;
        fullscreen = true;
        candidates[0].left = std::max(candidates[0].left, monitor.left + 1);
        candidates[1].top = std::max(candidates[1].top, monitor.top + 1);
        candidates[2].right = std::min(candidates[2].right, monitor.right - 1);
        candidates[3].bottom = std::min(candidates[3].bottom, monitor.bottom - 1);
    }
    if (!fullscreen) return host;
    std::optional<RECT> best;
    std::uint64_t largestArea = 0;
    for (const RECT candidate : candidates)
    {
        if (!valid(candidate) ||
            static_cast<std::int64_t>(candidate.left) - host.left > 1 ||
            static_cast<std::int64_t>(candidate.top) - host.top > 1 ||
            static_cast<std::int64_t>(host.right) - candidate.right > 1 ||
            static_cast<std::int64_t>(host.bottom) - candidate.bottom > 1)
            continue;
        if (std::any_of(monitors.begin(), monitors.end(),
                [&](RECT monitor) { return covers(candidate, monitor); })) continue;
        const auto area = static_cast<std::uint64_t>(static_cast<std::int64_t>(candidate.right) - candidate.left) *
            static_cast<std::uint64_t>(static_cast<std::int64_t>(candidate.bottom) - candidate.top);
        if (area > largestArea) { best = candidate; largestArea = area; }
    }
    return best;
}

DockWindowTransition::~DockWindowTransition()
{
    Cancel();
    if (compositionTarget_)
        compositionTarget_->SetRoot(nullptr);
    compositionClip_.Reset();
    compositionEffect_.Reset();
    compositionScaleTransform_.Reset();
    compositionVisual_.Reset();
    compositionTarget_.Reset();
    compositionDevice_.Reset();
    if (hwnd_)
        DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (instance_)
        UnregisterClassW(
            kDockWindowTransitionClassName, instance_);
}

bool DockWindowTransition::Initialize(
    HINSTANCE instance,
    snowdesktop::UiAnimationScheduler* animationScheduler,
    ID2D1Device*,
    IDCompositionDesktopDevice* compositionDevice)
{
    instance_ = instance;
    animationScheduler_ = animationScheduler;
    compositionDevice_ = compositionDevice;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName =
        kDockWindowTransitionClassName;
    const bool registered =
        RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    if (registered)
        EnsureWindow();
    return registered;
}

bool DockWindowTransition::EnsureWindow()
{
    if (hwnd_ && IsWindow(hwnd_))
        return true;
    if (!instance_)
        return false;

    hwnd_ = snowdesktop::CreateShellOverlayWindowEx(
        kDockWindowTransitionExStyle,
        kDockWindowTransitionClassName,
        L"Dock Window Transition",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!hwnd_)
        return false;
    if (!snowdesktop::dock_source_cloak::RegisterOwner(hwnd_))
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    const DWM_WINDOW_CORNER_PREFERENCE corner =
        kDockWindowTransitionCornerPreference;
    DwmSetWindowAttribute(
        hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corner, sizeof(corner));
    const DWMNCRENDERINGPOLICY ncRendering =
        kDockWindowTransitionNcRenderingPolicy;
    DwmSetWindowAttribute(
        hwnd_, DWMWA_NCRENDERING_POLICY,
        &ncRendering, sizeof(ncRendering));
    const COLORREF borderColor =
        kDockWindowTransitionBorderColor;
    DwmSetWindowAttribute(
        hwnd_, DWMWA_BORDER_COLOR,
        &borderColor, sizeof(borderColor));
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(
        hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED,
        &disableTransitions,
        sizeof(disableTransitions));
    return true;
}

bool DockWindowTransition::StartMinimize(
    HWND sourceWindow, RECT dockRect,
    HWND keepBelowWindow)
{
    return Start(
        sourceWindow, dockRect,
        DockWindowTransitionDirection::Minimize,
        {}, keepBelowWindow);
}

bool DockWindowTransition::StartExternalMinimize(HWND sourceWindow, RECT dockRect,
    DWORD deadline, HWND keepBelowWindow)
{
    if (IsActive() || !snowdesktop::dock_minimize::RequestIsCurrent(deadline, GetTickCount()))
        return false;
    if (!StartMinimize(sourceWindow, dockRect,
            keepBelowWindow))
        return false;
    // A timeout releases the target UI thread even while capture/composition
    // is still running here. Never retain an overlay prepared after that point.
    if (!snowdesktop::dock_minimize::RequestIsCurrent(deadline, GetTickCount()) ||
        IsIconic(sourceWindow))
    {
        Cancel();
        return false;
    }
    externalMinimize_ = true;
    externalMinimizeObserved_ = false;
    externalMinimizeDeadline_ = deadline;
    return true;
}

void DockWindowTransition::CancelExternalMinimize(HWND sourceWindow, DWORD deadline)
{
    if (externalMinimize_ && sourceWindow_ == sourceWindow && externalMinimizeDeadline_ == deadline)
        Cancel();
}

bool DockWindowTransition::StartRestore(
    HWND sourceWindow, RECT dockRect,
    RestoreCallback restoreCallback,
    HWND keepBelowWindow)
{
    if (!restoreCallback)
        return false;
    return Start(
        sourceWindow, dockRect,
        DockWindowTransitionDirection::Restore,
        std::move(restoreCallback),
        keepBelowWindow);
}

bool DockWindowTransition::Start(
    HWND sourceWindow, RECT dockRect,
    DockWindowTransitionDirection direction,
    RestoreCallback restoreCallback,
    HWND keepBelowWindow)
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "prepare");
    nativeFallbackRequested_ = false;
    if (!SystemWindowAnimationsEnabled() ||
        !sourceWindow || !IsWindow(sourceWindow) ||
        !IsUsableRect(dockRect))
        return false;

    HWND root = GetAncestor(sourceWindow, GA_ROOT);
    sourceWindow = root ? root : sourceWindow;
    std::erase_if(lastVisibleRects_, [](const auto& entry) { return !IsWindow(entry.first); });
    const auto startAction =
        ResolveDockWindowTransitionStartAction(
            IsActive(),
            sourceWindow_ == sourceWindow,
            direction_ == direction,
            awaitingRestoreVisibility_);
    if (startAction ==
        DockWindowTransitionStartAction::ContinueActive)
    {
        if (direction ==
                DockWindowTransitionDirection::Restore &&
            restoreCallback)
        {
            restoreCallback_ =
                std::move(restoreCallback);
        }
        return true;
    }
    if (startAction ==
        DockWindowTransitionStartAction::ReverseActive)
    {
        return Reverse(
            direction,
            std::move(restoreCallback));
    }
    if (startAction ==
        DockWindowTransitionStartAction::
            InterruptRestoreHandoff)
    {
        // The restore image has already reached its destination and the real
        // window is only waiting for the asynchronous SW_RESTORE to settle.
        // A new minimize request must remove that old image immediately;
        // otherwise it can mask the new native request for the entire cleanup
        // timeout. Start a fresh custom transition only when the real window
        // is already available to capture.
        Finish();
        if (direction ==
                DockWindowTransitionDirection::Minimize &&
            IsIconic(sourceWindow))
            return false;
    }

    Cancel();
    if (sourceCloaked_) return false;
    sourceWindow_ = sourceWindow;
    direction_ = direction;
    restoreCallback_ = std::move(restoreCallback);
    effect_ = snowdesktop::animation::RuntimeWindowEffect();
    const int requestedEffect = effect_;
    imageSource_ = L"unavailable";
    imageResult_ = S_OK;
    genieEdge_ = static_cast<snowdesktop::dock_genie::Edge>(
        std::clamp(snowdesktop::animation::RuntimeDockPosition(), 0, 3));

    RECT windowRect{};
    if (direction_ == DockWindowTransitionDirection::Minimize)
    {
        if (!ResolveVisibleWindowRect(sourceWindow_, windowRect))
        {
            Cancel();
            return false;
        }
        lastVisibleRects_[sourceWindow_] = windowRect;
    }
    else
    {
        // 失败检查：恢复动画只应在窗口确实处于最小化时播放。最小化命令
        // 可能被无响应进程丢弃（窗口从未进入 IsIconic），此时播放动画只会
        // 在幻影位置等待一个永远不会发生的恢复，看起来像动画卡死。
        if (!ResolveRestoreWindowRect(sourceWindow_, windowRect) ||
            !IsIconic(sourceWindow_))
        {
            Cancel();
            return false;
        }
    }
    windowRect_ = windowRect;
    dockRect_ = dockRect;
    fromRect_ = direction_ ==
            DockWindowTransitionDirection::Minimize
        ? windowRect_ : dockRect_;
    toRect_ = direction_ ==
            DockWindowTransitionDirection::Minimize
        ? dockRect_ : windowRect_;
    if (effect_ == 2)
        fromRect_ = toRect_ = windowRect_;
    collapseFrom_ = direction_ == DockWindowTransitionDirection::Minimize ? 0.0 : 1.0;
    collapseTo_ = 1.0 - collapseFrom_;
    lastCollapse_ = collapseFrom_;
    animationFromOpacity_ =
        ResolveDockWindowTransitionOpacity(
            direction_, 0.0);
    animationToOpacity_ =
        ResolveDockWindowTransitionOpacity(
            direction_, 1.0);
    animationDurationMs_ = TransitionDuration(effect_);

    if (!EnsureWindow())
    {
        Cancel();
        return false;
    }

    if (!CreateSharedWindowImage())
    {
        LogPresentation(requestedEffect, L"native-system-animation");
        Cancel();
        nativeFallbackRequested_ = true;
        return false;
    }

    imageHostRect_ =
        UsesCompositionImage()
        ? ResolveDockWindowImageHostRect(
            fromRect_, toRect_)
        : fromRect_;
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectAnimationMonitor,
            reinterpret_cast<LPARAM>(&animationMonitorRects_)))
    {
        Cancel();
        nativeFallbackRequested_ = true;
        return false;
    }
    const auto safeHost = ResolveDockWindowNonFullscreenHostRect(
        imageHostRect_, animationMonitorRects_);
    if (!safeHost)
    {
        if (diagnosticCallback_)
            diagnosticCallback_(L"Dock animation fallback: host would cover a complete monitor");
        Cancel();
        nativeFallbackRequested_ = true;
        return false;
    }
    imageHostRect_ = *safeHost;
    const int hostWidth = std::max(
        1L, imageHostRect_.right -
            imageHostRect_.left);
    const int hostHeight = std::max(
        1L, imageHostRect_.bottom -
            imageHostRect_.top);
    // The host callback keeps entire Dock content/backdrop pairs above us.
    // Only legacy callers without that callback use the single-window anchor.
    const HWND insertAfter = !presentationCallback_ &&
        keepBelowWindow && IsWindow(keepBelowWindow)
        ? keepBelowWindow : HWND_TOPMOST;
    SetWindowPos(
        hwnd_, insertAfter,
        imageHostRect_.left,
        imageHostRect_.top,
        hostWidth, hostHeight,
        SWP_NOACTIVATE);

    if (!ApplyFrame(0.0))
    {
        if (diagnosticCallback_) diagnosticCallback_(L"Dock animation aborted: stage=first-frame");
        Cancel();
        return false;
    }

    presenting_ = true;
    if (presentationCallback_)
        presentationCallback_(hwnd_);
    RefreshOcclusion();
    if (!presenting_)
        return false;
    if (diagnosticCallback_)
        diagnosticCallback_(L"Dock taskbar phase: overlay-before-show");
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (diagnosticCallback_)
        diagnosticCallback_(L"Dock taskbar phase: overlay-after-show");
    HRESULT presentationHr = S_OK;
    if (RequiresDockWindowTransitionCompositionBarrier(direction_))
    {
        presentationHr = compositionImageActive_ &&
                compositionDevice_
            ? compositionDevice_->WaitForCommitCompletion()
            : E_NOTIMPL;
        if (FAILED(presentationHr))
            presentationHr = DwmFlush();
    }
    if (FAILED(presentationHr))
    {
        if (diagnosticCallback_) diagnosticCallback_(L"Dock animation aborted: stage=presentation-barrier");
        Cancel();
        return false;
    }

    // The animation owns only its temporary app-cloak bit. DWM continues to
    // compose this window, so its shared image stays available. Avoid changing
    // the source's setter-only native transition policy, which cannot be read
    // reliably and may already have been disabled by the application.
    bool acquired = false;
    const HRESULT cloakHr = snowdesktop::dock_source_cloak::Acquire(sourceWindow_, hwnd_, acquired);
    if (acquired)
    {
        sourceCloaked_ = true;
        sourceCloakWindow_ = sourceWindow_;
    }
    if (FAILED(cloakHr))
    {
        if (diagnosticCallback_)
        {
            wchar_t diagnostic[128]{};
            swprintf_s(diagnostic, L"Dock animation aborted: stage=source-cloak hr=0x%08X",
                static_cast<unsigned>(cloakHr));
            diagnosticCallback_(diagnostic);
        }
        Cancel();
        return false;
    }

    animationStartTimeMs_ =
        MonotonicTimeMilliseconds();
    minimizeObserved_ = false;
    minimizeCleanupDeadlineMs_ = animationStartTimeMs_ + animationDurationMs_ + kMinimizeCleanupTimeoutMs;
    awaitingRestoreVisibility_ = false;
    restoreCleanupDeadlineMs_ = 0.0;
    restoreVisibleTimeMs_ = 0.0;
    restoreFadeStartTimeMs_ = 0.0;
    RequestRestoreForAnimation();
    if (!ScheduleAnimationWake())
    {
        if (diagnosticCallback_) diagnosticCallback_(L"Dock animation aborted: stage=schedule");
        Cancel();
        return false;
    }
    LogPresentation(requestedEffect, L"shared-window");
    return true;
}

HRESULT CreateSmoothStepAnimation(
    IDCompositionDesktopDevice* device,
    float from, float to,
    double durationMilliseconds,
    IDCompositionAnimation** animation)
{
    if (!device || !animation || durationMilliseconds <= 0.0)
        return E_INVALIDARG;
    *animation = nullptr;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> result;
    HRESULT hr = device->CreateAnimation(&result);
    const double duration = durationMilliseconds / 1000.0;
    const float delta = to - from;
    if (SUCCEEDED(hr))
    {
        hr = result->AddCubic(
            0.0, from, 0.0f,
            static_cast<float>(3.0 * delta /
                (duration * duration)),
            static_cast<float>(-2.0 * delta /
                (duration * duration * duration)));
    }
    if (SUCCEEDED(hr))
        hr = result->End(duration, to);
    if (SUCCEEDED(hr))
        *animation = result.Detach();
    return hr;
}

bool DockWindowTransition::Reverse(
    DockWindowTransitionDirection direction,
    RestoreCallback restoreCallback)
{
    if (!IsActive() ||
        !hasLastFrame_ ||
        awaitingRestoreVisibility_)
        return false;

    externalMinimize_ = false;
    externalMinimizeObserved_ = false;
    externalMinimizeDeadline_ = 0;

    if (!awaitingRestoreVisibility_)
    {
        const double progress = std::clamp(
            (MonotonicTimeMilliseconds() - animationStartTimeMs_) /
                std::max(1.0, animationDurationMs_),
            0.0, 1.0);
        if (!ApplyFrame(progress))
            return false;
        compositionTimelineActive_ = false;
    }

    const RECT currentFrame = lastFrameRect_;
    const RECT targetFrame =
        direction ==
            DockWindowTransitionDirection::Minimize
        ? dockRect_ : windowRect_;
    const auto maximumEdgeDistance =
        [](const RECT& first,
            const RECT& second) {
            return std::max({
                std::abs(
                    static_cast<double>(
                        first.left) -
                    static_cast<double>(
                        second.left)),
                std::abs(
                    static_cast<double>(
                        first.top) -
                    static_cast<double>(
                        second.top)),
                std::abs(
                    static_cast<double>(
                        first.right) -
                    static_cast<double>(
                        second.right)),
                std::abs(
                    static_cast<double>(
                        first.bottom) -
                    static_cast<double>(
                        second.bottom))
            });
        };
    const double fullDistance = std::max(
        1.0,
        maximumEdgeDistance(
            windowRect_, dockRect_));
    double remainingRatio = std::clamp(
        maximumEdgeDistance(
            currentFrame, targetFrame) /
            fullDistance,
        0.0, 1.0);

    const double targetCollapse =
        direction == DockWindowTransitionDirection::Minimize ? 1.0 : 0.0;
    if (effect_ == 3)
        remainingRatio = std::abs(targetCollapse - lastCollapse_);
    else if (effect_ == 2)
        remainingRatio = std::abs(static_cast<double>(lastFrameOpacity_) -
            (direction == DockWindowTransitionDirection::Minimize ? 0.0 : 255.0)) / 255.0;

    direction_ = direction;
    fromRect_ = currentFrame;
    toRect_ = targetFrame;
    if (effect_ == 2)
        fromRect_ = toRect_ = windowRect_;
    collapseFrom_ = lastCollapse_;
    collapseTo_ = targetCollapse;
    animationFromOpacity_ =
        lastFrameOpacity_;
    animationToOpacity_ =
        ResolveDockWindowTransitionOpacity(
            direction_, 1.0);
    animationDurationMs_ = std::clamp(
        TransitionDuration(effect_) * remainingRatio,
        static_cast<double>(
            kMinimumReverseDurationMs) * snowdesktop::animation::RuntimeDurationScale(),
        TransitionDuration(effect_));
    restoreCallback_ =
        std::move(restoreCallback);
    animationStartTimeMs_ =
        MonotonicTimeMilliseconds();
    minimizeObserved_ = false;
    minimizeCleanupDeadlineMs_ = animationStartTimeMs_ + animationDurationMs_ + kMinimizeCleanupTimeoutMs;
    restoreCleanupDeadlineMs_ = 0.0;
    restoreVisibleTimeMs_ = 0.0;
    restoreFadeStartTimeMs_ = 0.0;
    awaitingRestoreVisibility_ = false;
    preparingRestore_ = false;
    restoreRequested_ = false;
    restoreActivated_ = false;
    restoreGeometryStableTimeMs_ = 0.0;
    RequestRestoreForAnimation();
    if (!ScheduleAnimationWake())
    {
        CompleteRestoreAfterRenderFailure();
        Finish();
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

    if (snowdesktop::dock_window_rules::
            ShouldRestoreDockWindowMaximized(
                placement.flags, placement.showCmd))
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

bool DockWindowTransition::EnsureCompositionVisuals()
{
    if (!compositionDevice_ || !hwnd_ || !IsWindow(hwnd_))
    {
        imageResult_ = E_NOTIMPL;
        return false;
    }
    HRESULT hr = S_OK;
    if (!compositionTarget_)
    {
        hr = compositionDevice_->CreateTargetForHwnd(
            hwnd_, TRUE, &compositionTarget_);
        if (SUCCEEDED(hr))
            hr = compositionDevice_->CreateVisual(
                &compositionVisual_);
        if (SUCCEEDED(hr))
            hr = compositionDevice_->CreateEffectGroup(
                &compositionEffect_);
        if (SUCCEEDED(hr))
            hr = compositionDevice_->CreateScaleTransform(
                &compositionScaleTransform_);
        if (SUCCEEDED(hr))
            hr = compositionDevice_->CreateRectangleClip(
                &compositionClip_);
        if (SUCCEEDED(hr))
            hr = compositionVisual_->SetEffect(
                compositionEffect_.Get());
        if (SUCCEEDED(hr))
            hr = compositionVisual_->SetClip(
                compositionClip_.Get());
        if (SUCCEEDED(hr))
            hr = compositionVisual_->SetTransform(
                compositionScaleTransform_.Get());
        if (SUCCEEDED(hr))
            hr = compositionTarget_->SetRoot(
                compositionVisual_.Get());
        if (FAILED(hr))
        {
            imageResult_ = hr;
            compositionClip_.Reset();
            compositionEffect_.Reset();
            compositionScaleTransform_.Reset();
            compositionVisual_.Reset();
            compositionTarget_.Reset();
            return false;
        }
    }

    return true;
}

bool DockWindowTransition::CreateSharedWindowImage()
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "image.bind");
    compositionSharedWindowActive_ = false;
    SIZE size{};
    imageResult_ = snowdesktop::dock_thumbnail::SourceSize(sourceWindow_, size);
    if (FAILED(imageResult_) || !EnsureCompositionVisuals()) return false;
    compositionImageSize_ = size;
    compositionSourceRegion_ = snowdesktop::dock_thumbnail::SourceRegion(sourceWindow_, size);
    compositionSharedWindowActive_ = true;
    HRESULT hr = compositionVisual_->SetContent(nullptr);
    if (SUCCEEDED(hr)) hr = compositionEffect_->SetOpacity(0.0f);
    if (SUCCEEDED(hr)) hr = compositionClip_->SetLeft(0.0f);
    if (SUCCEEDED(hr)) hr = compositionClip_->SetTop(0.0f);
    if (SUCCEEDED(hr)) hr = compositionClip_->SetRight(static_cast<float>(size.cx));
    if (SUCCEEDED(hr)) hr = compositionClip_->SetBottom(static_cast<float>(size.cy));
    if (SUCCEEDED(hr) && effect_ == 3)
    {
        if (!CreateGenieStrips()) hr = FAILED(imageResult_) ? imageResult_ : E_FAIL;
    }
    else if (SUCCEEDED(hr))
    {
        hr = compositionWindowImage_.Create(hwnd_, sourceWindow_, compositionDevice_.Get(), size,
            &compositionSourceRegion_);
        if (SUCCEEDED(hr)) hr = compositionVisual_->AddVisual(compositionWindowImage_.visual.Get(), TRUE, nullptr);
    }
    if (FAILED(hr))
    {
        compositionVisual_->RemoveAllVisuals();
        ClearGenieStrips();
        compositionWindowImage_.Reset();
        compositionSharedWindowActive_ = false;
        compositionImageSize_ = {};
        imageResult_ = hr;
        return false;
    }
    imageSource_ = L"dwm-shared-window";
    imageResult_ = S_OK;
    compositionImageActive_ = true;
    return true;
}

void DockWindowTransition::ClearGenieStrips()
{
    if (genieStrips_.empty())
        return;
    if (compositionVisual_)
        compositionVisual_->RemoveAllVisuals();
    for (auto& strip : genieStrips_)
        strip->SetContent(nullptr);
    genieStrips_.clear();
    genieWindowImages_.clear();
    if (compositionVisual_)
    {
        compositionVisual_->SetContent(nullptr);
        compositionVisual_->SetTransform(compositionScaleTransform_.Get());
        compositionVisual_->SetClip(compositionClip_.Get());
    }
    hasLastFrame_ = false;
}

bool DockWindowTransition::CreateGenieStrips()
{
    if (!compositionVisual_ || !compositionSharedWindowActive_ || !compositionDevice_)
    {
        imageResult_ = E_UNEXPECTED;
        return false;
    }
    const float width = static_cast<float>(compositionImageSize_.cx);
    const float height = static_cast<float>(compositionImageSize_.cy);
    const bool vertical = snowdesktop::dock_genie::Vertical(genieEdge_);
    genieStrips_.reserve(snowdesktop::dock_genie::StripCount);
    for (std::size_t i = 0; i < snowdesktop::dock_genie::StripCount; ++i)
    {
        Microsoft::WRL::ComPtr<IDCompositionVisual3> strip;
        snowdesktop::dock_thumbnail::SharedVisual image;
        HRESULT hr = image.Create(hwnd_, sourceWindow_, compositionDevice_.Get(), compositionImageSize_,
            &compositionSourceRegion_);
        strip = image.visual;
        if (SUCCEEDED(hr)) genieWindowImages_.push_back(std::move(image));
        if (FAILED(hr) || !strip)
        {
            imageResult_ = FAILED(hr) ? hr : E_FAIL;
            return false;
        }
        genieStrips_.push_back(strip);
        const float begin = static_cast<float>(i) /
            static_cast<float>(snowdesktop::dock_genie::StripCount);
        const float end = static_cast<float>(i + 1) /
            static_cast<float>(snowdesktop::dock_genie::StripCount);
        // Slight overlap avoids transparent seams under fractional GPU sampling.
        // Opacity belongs to the parent so overlapping strips do not darken.
        const D2D1_RECT_F clip = vertical
            ? D2D1::RectF(0, std::max(0.0f, begin * height - 0.75f),
                width, std::min(height, end * height + 0.75f))
            : D2D1::RectF(std::max(0.0f, begin * width - 0.75f), 0,
                std::min(width, end * width + 0.75f), height);
        if (SUCCEEDED(hr)) hr = strip->SetClip(clip);
        if (SUCCEEDED(hr)) hr = strip->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
        if (SUCCEEDED(hr)) hr = strip->SetBitmapInterpolationMode(
            DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (SUCCEEDED(hr)) hr = compositionVisual_->AddVisual(strip.Get(), TRUE, nullptr);
        if (FAILED(hr))
        {
            imageResult_ = hr;
            return false;
        }
    }
    HRESULT hr = compositionVisual_->SetContent(nullptr);
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetClip(static_cast<IDCompositionClip*>(nullptr));
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetOffsetX(0.0f);
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetOffsetY(0.0f);
    imageResult_ = hr;
    return SUCCEEDED(hr);
}

bool DockWindowTransition::ApplyGenieFrame(double collapsed, BYTE opacity)
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "genie.submit");
    if (genieStrips_.size() != snowdesktop::dock_genie::StripCount ||
        !compositionDevice_ || !compositionEffect_)
        return false;
    const auto rect = [](const RECT& value) {
        return snowdesktop::dock_genie::Rect{
            static_cast<double>(value.left), static_cast<double>(value.top),
            static_cast<double>(value.right), static_cast<double>(value.bottom)};
    };
    HRESULT hr = S_OK;
    const bool geometryChanged = !hasLastFrame_ || lastGenieCollapse_ != collapsed;
    const bool opacityChanged = !hasLastFrame_ || lastFrameOpacity_ != opacity;
    if (!geometryChanged && !opacityChanged) return true;
    for (std::size_t i = 0; geometryChanged && i < genieStrips_.size() && SUCCEEDED(hr); ++i)
    {
        const auto transform = snowdesktop::dock_genie::StripProjectiveMatrix(
            rect(windowRect_), rect(dockRect_), genieEdge_, collapsed,
            compositionImageSize_.cx, compositionImageSize_.cy,
            static_cast<double>(i) / static_cast<double>(genieStrips_.size()),
            static_cast<double>(i + 1) / static_cast<double>(genieStrips_.size()),
            imageHostRect_.left, imageHostRect_.top);
        const D2D1_MATRIX_4X4_F matrix{
            static_cast<float>(transform.m11), static_cast<float>(transform.m12), 0, static_cast<float>(transform.m14),
            static_cast<float>(transform.m21), static_cast<float>(transform.m22), 0, static_cast<float>(transform.m24),
            0, 0, 1, 0,
            static_cast<float>(transform.dx), static_cast<float>(transform.dy), 0, static_cast<float>(transform.w)};
        hr = genieStrips_[i]->SetTransform(matrix);
    }
    if (SUCCEEDED(hr) && opacityChanged)
        hr = compositionEffect_->SetOpacity(static_cast<float>(opacity) / 255.0f);
    if (SUCCEEDED(hr)) hr = compositionDevice_->Commit();
    if (SUCCEEDED(hr)) lastGenieCollapse_ = collapsed;
    return SUCCEEDED(hr);
}

bool DockWindowTransition::CommitCompositionTimeline(
    std::span<IDCompositionAnimation* const> animations)
{
    LARGE_INTEGER begin{}, frequency{};
    if (!QueryPerformanceCounter(&begin) || !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0) return false;
    DCOMPOSITION_FRAME_STATISTICS statistics{};
    if (SUCCEEDED(compositionDevice_->GetFrameStatistics(&statistics)) &&
        statistics.nextEstimatedFrameTime.QuadPart > begin.QuadPart &&
        statistics.nextEstimatedFrameTime.QuadPart - begin.QuadPart < frequency.QuadPart / 10)
        begin = statistics.nextEstimatedFrameTime;
    // All properties and the completion wake use the same clock. Building or
    // rebinding the scene must not consume the beginning of the animation.
    for (auto* animation : animations)
        if (FAILED(animation->SetAbsoluteBeginTime(begin))) return false;
    if (FAILED(compositionDevice_->Commit())) return false;
    animationStartTimeMs_ = static_cast<double>(begin.QuadPart) * 1000.0 /
        static_cast<double>(frequency.QuadPart);
    if (awaitingRestoreVisibility_ && restoreFadeStartTimeMs_ > 0.0)
        restoreFadeStartTimeMs_ = animationStartTimeMs_;
    if (direction_ == DockWindowTransitionDirection::Minimize)
        minimizeCleanupDeadlineMs_ = animationStartTimeMs_ + animationDurationMs_ + kMinimizeCleanupTimeoutMs;
    compositionTimelineActive_ = true;
    return true;
}

bool DockWindowTransition::StartCompositionTimeline(bool opacityOnly)
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "timeline.prepare");
    if (!compositionImageActive_ || !compositionDevice_ ||
        !compositionVisual_ || !compositionScaleTransform_ ||
        !compositionEffect_ || !compositionClip_ ||
        compositionImageSize_.cx <= 0 ||
        compositionImageSize_.cy <= 0 ||
        animationDurationMs_ <= 0.0)
        return false;

    if (opacityOnly)
    {
        Microsoft::WRL::ComPtr<IDCompositionAnimation> opacity;
        HRESULT hr = CreateSmoothStepAnimation(compositionDevice_.Get(),
            static_cast<float>(animationFromOpacity_) / 255.0f,
            static_cast<float>(animationToOpacity_) / 255.0f,
            animationDurationMs_, &opacity);
        if (SUCCEEDED(hr)) hr = compositionEffect_->SetOpacity(opacity.Get());
        const std::array<IDCompositionAnimation*, 1> animations{opacity.Get()};
        return SUCCEEDED(hr) && CommitCompositionTimeline(animations);
    }

    const auto width = [](const RECT& rect) {
        return std::max(1L, rect.right - rect.left);
    };
    const auto height = [](const RECT& rect) {
        return std::max(1L, rect.bottom - rect.top);
    };
    const float fromScaleX = static_cast<float>(width(fromRect_)) /
        static_cast<float>(compositionImageSize_.cx);
    const float fromScaleY = static_cast<float>(height(fromRect_)) /
        static_cast<float>(compositionImageSize_.cy);
    const float toScaleX = static_cast<float>(width(toRect_)) /
        static_cast<float>(compositionImageSize_.cx);
    const float toScaleY = static_cast<float>(height(toRect_)) /
        static_cast<float>(compositionImageSize_.cy);
    const float fromRadiusX = static_cast<float>(
        ResolveDockWindowTransitionCornerRadius(
            fromRect_, dockRect_)) / fromScaleX;
    const float fromRadiusY = static_cast<float>(
        ResolveDockWindowTransitionCornerRadius(
            fromRect_, dockRect_)) / fromScaleY;
    const float toRadiusX = static_cast<float>(
        ResolveDockWindowTransitionCornerRadius(
            toRect_, dockRect_)) / toScaleX;
    const float toRadiusY = static_cast<float>(
        ResolveDockWindowTransitionCornerRadius(
            toRect_, dockRect_)) / toScaleY;

    Microsoft::WRL::ComPtr<IDCompositionAnimation> offsetX;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> offsetY;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> scaleX;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> scaleY;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> opacity;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> radiusX;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> radiusY;
    auto create = [&](float from, float to,
                      IDCompositionAnimation** animation) {
        return CreateSmoothStepAnimation(
            compositionDevice_.Get(), from, to,
            animationDurationMs_, animation);
    };
    HRESULT hr = create(
        static_cast<float>(fromRect_.left - imageHostRect_.left),
        static_cast<float>(toRect_.left - imageHostRect_.left),
        &offsetX);
    if (SUCCEEDED(hr))
        hr = create(
            static_cast<float>(fromRect_.top - imageHostRect_.top),
            static_cast<float>(toRect_.top - imageHostRect_.top),
            &offsetY);
    if (SUCCEEDED(hr))
        hr = create(fromScaleX, toScaleX, &scaleX);
    if (SUCCEEDED(hr))
        hr = create(fromScaleY, toScaleY, &scaleY);
    if (SUCCEEDED(hr))
        hr = create(
            static_cast<float>(animationFromOpacity_) / 255.0f,
            static_cast<float>(animationToOpacity_) / 255.0f,
            &opacity);
    if (SUCCEEDED(hr))
        hr = create(fromRadiusX, toRadiusX, &radiusX);
    if (SUCCEEDED(hr))
        hr = create(fromRadiusY, toRadiusY, &radiusY);
    if (SUCCEEDED(hr))
        hr = compositionVisual_->SetOffsetX(offsetX.Get());
    if (SUCCEEDED(hr))
        hr = compositionVisual_->SetOffsetY(offsetY.Get());
    if (SUCCEEDED(hr))
        hr = compositionScaleTransform_->SetScaleX(scaleX.Get());
    if (SUCCEEDED(hr))
        hr = compositionScaleTransform_->SetScaleY(scaleY.Get());
    if (SUCCEEDED(hr))
        hr = compositionEffect_->SetOpacity(opacity.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetTopLeftRadiusX(radiusX.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetTopLeftRadiusY(radiusY.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetTopRightRadiusX(radiusX.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetTopRightRadiusY(radiusY.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetBottomLeftRadiusX(radiusX.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetBottomLeftRadiusY(radiusY.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetBottomRightRadiusX(radiusX.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetBottomRightRadiusY(radiusY.Get());
    const std::array<IDCompositionAnimation*, 7> animations{
        offsetX.Get(), offsetY.Get(), scaleX.Get(), scaleY.Get(),
        opacity.Get(), radiusX.Get(), radiusY.Get()};
    return SUCCEEDED(hr) && CommitCompositionTimeline(animations);
}

bool DockWindowTransition::ScheduleAnimationWake()
{
    if (!animationScheduler_)
        return false;
    if (animationToken_)
        animationScheduler_->Cancel(animationToken_);
    animationToken_ = 0;

    const bool retiring = awaitingRestoreVisibility_ && restoreFadeStartTimeMs_ > 0.0;
    if (UsesCompositionImage() && !preparingRestore_ && (effect_ != 3 || retiring))
    {
        if (!StartCompositionTimeline(retiring))
            return false;
        animationToken_ = animationScheduler_->ScheduleOnce(
            static_cast<UINT>(std::ceil(std::max(1.0,
                animationStartTimeMs_ + animationDurationMs_ - MonotonicTimeMilliseconds()))) + 2,
            [this](snowdesktop::UiScheduleToken token) {
                if (animationToken_ != token)
                    return;
                animationToken_ = 0;
                compositionTimelineActive_ = false;
                const bool keep = OnAnimationFrame(
                    MonotonicTimeMilliseconds());
                if (keep && !animationToken_ && animationScheduler_)
                {
                    animationToken_ =
                        animationScheduler_->StartAnimation(
                            snowdesktop::UiAnimationSurface::
                                WindowTransition,
                            [this](double nowMilliseconds) {
                                return OnAnimationFrame(
                                    nowMilliseconds);
                            });
                }
            });
    }
    else
    {
        animationToken_ = animationScheduler_->StartAnimation(
            snowdesktop::UiAnimationSurface::WindowTransition,
            [this](double nowMilliseconds) {
                return OnAnimationFrame(nowMilliseconds);
            });
    }
    return animationToken_ != 0;
}

bool DockWindowTransition::ApplyFrame(double progress)
{
    if (!hwnd_ || !compositionSharedWindowActive_)
        return false;

    RECT frame = InterpolateDockWindowTransitionRect(
        fromRect_, toRect_, progress);
    const int width = std::max(1L, frame.right - frame.left);
    const int height =
        std::max(1L, frame.bottom - frame.top);
    if (UsesCompositionImage() &&
        !ApplyOcclusion(imageHostRect_, 0))
        return false;
    const double eased =
        EaseDockWindowTransition(progress);
    BYTE frameOpacity =
        static_cast<BYTE>(std::clamp(
            static_cast<int>(std::lround(
                static_cast<double>(
                    animationFromOpacity_) +
                (static_cast<double>(
                    animationToOpacity_) -
                    static_cast<double>(
                        animationFromOpacity_)) *
                    eased)),
            0, 255));
    if (!awaitingRestoreVisibility_)
        lastCollapse_ = snowdesktop::dock_genie::Mix(collapseFrom_, collapseTo_, eased);
    if (effect_ == 3)
    {
        if (!awaitingRestoreVisibility_)
            frameOpacity = static_cast<BYTE>(std::clamp(
                static_cast<int>(std::lround(
                    snowdesktop::dock_genie::Opacity(lastCollapse_) * 255.0)), 0, 255));
        if (!ApplyGenieFrame(lastCollapse_, frameOpacity))
            return false;
        lastFrameRect_ = frame;
        lastFrameOpacity_ = frameOpacity;
        hasLastFrame_ = true;
        return true;
    }
    const int cornerRadius =
        ResolveDockWindowTransitionCornerRadius(
            frame, dockRect_);
    const bool geometryChanged =
        !hasLastFrame_ ||
        EqualRect(&frame, &lastFrameRect_) == FALSE;
    const bool opacityChanged =
        !hasLastFrame_ ||
        frameOpacity != lastFrameOpacity_;
    if (!geometryChanged && !opacityChanged)
        return true;

    if (UsesCompositionImage())
    {
        if (!compositionImageActive_ ||
            !compositionDevice_ ||
            !compositionVisual_ ||
            !compositionScaleTransform_ ||
            !compositionEffect_ ||
            !compositionClip_ ||
            compositionImageSize_.cx <= 0 ||
            compositionImageSize_.cy <= 0)
            return false;

        const float scaleX =
            static_cast<float>(width) /
            static_cast<float>(compositionImageSize_.cx);
        const float scaleY =
            static_cast<float>(height) /
            static_cast<float>(compositionImageSize_.cy);
        HRESULT hr = S_OK;
        if (geometryChanged)
        {
            hr = compositionVisual_->SetOffsetX(
                static_cast<float>(
                    frame.left - imageHostRect_.left));
            if (SUCCEEDED(hr))
                hr = compositionVisual_->SetOffsetY(
                    static_cast<float>(
                        frame.top - imageHostRect_.top));
            if (SUCCEEDED(hr))
                hr = compositionScaleTransform_->SetScaleX(scaleX);
            if (SUCCEEDED(hr))
                hr = compositionScaleTransform_->SetScaleY(scaleY);
            const float radiusX = scaleX > 0.0f
                ? static_cast<float>(cornerRadius) / scaleX
                : 0.0f;
            const float radiusY = scaleY > 0.0f
                ? static_cast<float>(cornerRadius) / scaleY
                : 0.0f;
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetTopLeftRadiusX(radiusX);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetTopLeftRadiusY(radiusY);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetTopRightRadiusX(radiusX);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetTopRightRadiusY(radiusY);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetBottomLeftRadiusX(radiusX);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetBottomLeftRadiusY(radiusY);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetBottomRightRadiusX(radiusX);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetBottomRightRadiusY(radiusY);
        }
        if (SUCCEEDED(hr) && opacityChanged)
            hr = compositionEffect_->SetOpacity(
                static_cast<float>(frameOpacity) / 255.0f);
        if (SUCCEEDED(hr))
            hr = compositionDevice_->Commit();
        if (FAILED(hr))
            return false;
    }
    lastFrameRect_ = frame;
    lastFrameOpacity_ = frameOpacity;
    hasLastFrame_ = true;
    return true;
}

bool DockWindowTransition::OnAnimationFrame(
    double nowMilliseconds)
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "frame");
    if (!sourceWindow_ || !IsWindow(sourceWindow_))
    {
        Finish();
        return false;
    }
    if (externalMinimize_)
    {
        if (IsIconic(sourceWindow_)) externalMinimizeObserved_ = true;
        else if (externalMinimizeObserved_ ||
            !snowdesktop::dock_minimize::RequestIsCurrent(externalMinimizeDeadline_, GetTickCount()))
        {
            // Another hook can veto the original action; a restore can also
            // arrive before our image reaches the Dock. Do not cover it.
            Finish();
            return false;
        }
    }
    if (direction_ == DockWindowTransitionDirection::Minimize)
    {
        if (IsIconic(sourceWindow_)) minimizeObserved_ = true;
        else if (minimizeObserved_)
        {
            // The app was restored while the minimize image was in flight.
            Finish();
            return false;
        }
    }
    if (!snowdesktop::animation::RuntimeAnimationsEnabled())
    {
        CompleteImmediately();
        return false;
    }

    const double now = nowMilliseconds;
    if (preparingRestore_)
    {
        if (!PrepareRestoredWindow(now)) return IsActive();
        if (effect_ != 3)
        {
            // The preparation poll owns a CPU frame token, but the restored
            // image now has its final geometry and can run independently.
            if (!ScheduleAnimationWake())
            {
                CompleteRestoreAfterRenderFailure();
                Finish();
            }
            return false;
        }
        return true;
    }
    if (awaitingRestoreVisibility_)
    {
        if (restoreFadeStartTimeMs_ > 0.0)
        {
            const double fadeProgress = std::min(
                1.0,
                (now - restoreFadeStartTimeMs_) /
                    static_cast<double>(
                        kRestoreImageFadeDurationMs));
            if (!ApplyFrame(fadeProgress))
            {
                // The real window is already restored and activated. If the
                // retiring overlay fails to render, remove it immediately
                // instead of invoking another restore command.
                ActivateRestoredWindowForHandoff();
                Finish();
                return false;
            }
            if (fadeProgress < 1.0)
                return true;

            // Ensure the transparent last frame reaches DWM before destroying
            // the topmost handoff surface. This prevents a one-frame exposure
            // of the previously maximized application underneath it.
            HRESULT presentationHr =
                compositionImageActive_ && compositionDevice_
                ? compositionDevice_->WaitForCommitCompletion()
                : E_NOTIMPL;
            if (FAILED(presentationHr))
                DwmFlush();
            ActivateRestoredWindowForHandoff();
            Finish();
            return false;
        }

        if (restoreVisibleTimeMs_ > 0.0)
        {
            if (now - restoreVisibleTimeMs_ <
                static_cast<double>(
                    kRestorePresentationDelayMs))
            {
                return true;
            }

            // IsIconic clearing precedes the first composed frame of some
            // applications. Hold the opaque shared image for one presentation,
            // then retire it without changing its final full-window geometry.
            ActivateRestoredWindowForHandoff();
            DwmFlush();
            fromRect_ = windowRect_;
            toRect_ = windowRect_;
            animationFromOpacity_ = lastFrameOpacity_;
            animationToOpacity_ = 0;
            animationStartTimeMs_ = now;
            animationDurationMs_ = static_cast<double>(
                kRestoreImageFadeDurationMs);
            restoreFadeStartTimeMs_ = now;
            compositionTimelineActive_ = false;
            if (!ScheduleAnimationWake()) Finish();
            return false;
        }

        // 失败检查：恢复回调已执行，但窗口必须在清理超时内真正退出最小化。
        // 目标进程挂起（例如求解器无法处理 SW_RESTORE）时 IsIconic 会一直
        // 保持为真，此时必须中止动画而不是无限等待，否则过渡层会永久停留
        // 在窗口位置，表现为 Dock 卡死。
        RECT restoredFrame{};
        if (RestoreGeometryReady(restoredFrame) && EqualRect(&restoredFrame, &windowRect_))
        {
            ActivateRestoredWindowForHandoff();
            // A failed cross-process release retains ownership; do not retire
            // the only visible image while its source is still cloaked.
            if (!sourceCloaked_)
            {
                DwmFlush();
                restoreVisibleTimeMs_ = now;
            }
            return true;
        }
        if (now >= restoreCleanupDeadlineMs_)
        {
            Finish();
            return false;
        }
        return true;
    }

    const double progress = std::min(
        1.0,
        (now - animationStartTimeMs_) /
            std::max(1.0, animationDurationMs_));
    if (!ApplyFrame(progress))
    {
        CompleteRestoreAfterRenderFailure();
        Finish();
        return false;
    }
    if (progress < 1.0)
        return true;

    if (direction_ == DockWindowTransitionDirection::Minimize && !IsIconic(sourceWindow_) &&
        now < minimizeCleanupDeadlineMs_)
    {
        // Posting SC_MINIMIZE acknowledges dispatch, not the application's
        // state change. Keep the source cloaked until that handoff
        // completes; some browser/toolkit UI threads process it late.
        return true;
    }

    if (direction_ == DockWindowTransitionDirection::Restore &&
        restoreCallback_)
    {
        // The source has been restoring underneath the visual timeline. Keep
        // its full-size image until the final source frame can be exposed.
        awaitingRestoreVisibility_ = true;
        restoreCleanupDeadlineMs_ =
            now + static_cast<double>(
                kRestoreCleanupTimeoutMs);
        return true;
    }
    Finish();
    return false;
}

void DockWindowTransition::RequestRestoreForAnimation()
{
    if (direction_ != DockWindowTransitionDirection::Restore ||
        !restoreCallback_ || restoreRequested_)
        return;
    restoreRequested_ = true;
    preparingRestore_ = true;
    restoreGeometryStableTimeMs_ = 0.0;
    restoreCleanupDeadlineMs_ = MonotonicTimeMilliseconds() + kRestoreCleanupTimeoutMs;
    // Request the native restore while the source is cloaked and our first
    // (collapsed, transparent) frame is already committed. This gives native
    // geometry and application painting the whole Genie timeline to settle.
    restoreCallback_(sourceWindow_, DockWindowRestoreTransitionPhase::RequestRestore);
}

bool DockWindowTransition::RestoreGeometryReady(RECT& frame) const
{
    if (!sourceWindow_ || IsIconic(sourceWindow_) || !IsWindowVisible(sourceWindow_) ||
        !ResolveVisibleWindowRect(sourceWindow_, frame))
        return false;
    if (UsesCompositionImage())
    {
        // IsIconic can clear while a toolkit still has the minimized-bar
        // geometry. Compare with DWM's retained full image, not that flag alone.
        return frame.right - frame.left == compositionImageSize_.cx &&
            frame.bottom - frame.top == compositionImageSize_.cy;
    }
    return false;
}

bool DockWindowTransition::PrepareRestoredWindow(double now)
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "restore.prepare");
    RECT frame{};
    if (!RestoreGeometryReady(frame))
    {
        restoreGeometryStableTimeMs_ = 0.0;
    }
    else if (restoreGeometryStableTimeMs_ <= 0.0 || !EqualRect(&frame, &restoreGeometryRect_))
    {
        restoreGeometryRect_ = frame;
        restoreGeometryStableTimeMs_ = now;
    }
    else if (now - restoreGeometryStableTimeMs_ >= kRestorePresentationDelayMs)
    {
        // Resolve both target geometry and source crop from the actual restored
        // window. WINDOWPLACEMENT and a minimized HWND cannot supply this crop.
        windowRect_ = frame;
        toRect_ = windowRect_;
        if (effect_ == 2) fromRect_ = toRect_;
        const RECT requestedHost = UsesCompositionImage()
            ? ResolveDockWindowImageHostRect(fromRect_, toRect_) : fromRect_;
        const auto safeHost = ResolveDockWindowNonFullscreenHostRect(requestedHost, animationMonitorRects_);
        bool prepared = safeHost.has_value();
        if (prepared)
        {
            imageHostRect_ = *safeHost;
            prepared = SetWindowPos(hwnd_, nullptr, imageHostRect_.left, imageHostRect_.top,
                imageHostRect_.right - imageHostRect_.left,
                imageHostRect_.bottom - imageHostRect_.top,
                SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        }
        if (prepared && compositionSharedWindowActive_)
        {
            ClearGenieStrips();
            compositionWindowImage_.Reset();
            compositionVisual_->RemoveAllVisuals();
            prepared = CreateSharedWindowImage();
        }
        hasLastFrame_ = false;
        compositionTimelineActive_ = false;
        if (prepared) prepared = ApplyFrame(0.0);
        if (!prepared)
        {
            if (diagnosticCallback_) diagnosticCallback_(L"Dock animation aborted: stage=restore-rebind");
            CompleteRestoreAfterRenderFailure();
            Finish();
            return false;
        }
        DwmFlush();
        animationStartTimeMs_ = MonotonicTimeMilliseconds();
        preparingRestore_ = false;
        if (diagnosticCallback_) diagnosticCallback_(L"Dock restore prepared: full-geometry before animation");
        return true;
    }
    if (now >= restoreCleanupDeadlineMs_)
    {
        if (diagnosticCallback_) diagnosticCallback_(L"Dock animation aborted: stage=restore-geometry-timeout");
        CompleteRestoreAfterRenderFailure();
        Finish();
    }
    return false;
}

void DockWindowTransition::
CompleteRestoreAfterRenderFailure()
{
    if (direction_ !=
            DockWindowTransitionDirection::Restore ||
        !restoreCallback_ ||
        !sourceWindow_ ||
        !IsWindow(sourceWindow_))
        return;
    RestoreCallback callback =
        std::move(restoreCallback_);
    callback(
        sourceWindow_,
        DockWindowRestoreTransitionPhase::
            FallbackWithoutAnimation);
}

void DockWindowTransition::
ActivateRestoredWindowForHandoff()
{
    if (!restoreCallback_ || !sourceWindow_ ||
        !IsWindow(sourceWindow_) || IsIconic(sourceWindow_))
        return;
    ReleaseSourceCloak();
    if (sourceCloaked_ || restoreActivated_) return;
    restoreActivated_ = true;
    restoreCallback_(
        sourceWindow_,
        DockWindowRestoreTransitionPhase::ActivateRestored);
}

void DockWindowTransition::ReleaseSourceCloak()
{
    if (!sourceCloaked_) return;
    if (sourceCloakWindow_ && IsWindow(sourceCloakWindow_))
    {
        const HRESULT hr = snowdesktop::dock_source_cloak::Release(sourceCloakWindow_, hwnd_);
        if (FAILED(hr))
        {
            if (diagnosticCallback_) diagnosticCallback_(L"Dock source uncloak failed");
            return;
        }
    }
    sourceCloaked_ = false;
    sourceCloakWindow_ = nullptr;
}

void DockWindowTransition::Finish()
{
    externalMinimize_ = false;
    externalMinimizeObserved_ = false;
    externalMinimizeDeadline_ = 0;
    minimizeObserved_ = false;
    minimizeCleanupDeadlineMs_ = 0.0;
    if (animationScheduler_ && animationToken_)
        animationScheduler_->Cancel(animationToken_);
    animationToken_ = 0;
    HWND transitionWindow = hwnd_;
    const bool hadCompositionImage = compositionImageActive_;
    // Retire the visible scene before changing HWND visibility, Dock layers,
    // strip transforms or its source. Restoring the full-size image while
    // the previous scene is still being composed can flash the application.
    if (hadCompositionImage && compositionDevice_)
    {
        if (compositionEffect_) compositionEffect_->SetOpacity(0.0f);
        if (compositionVisual_)
        {
            compositionVisual_->RemoveAllVisuals();
            compositionVisual_->SetContent(nullptr);
        }
        if (SUCCEEDED(compositionDevice_->Commit()))
            compositionDevice_->WaitForCommitCompletion();
    }
    if (presenting_ || sourceCloaked_) DwmFlush();
    if (presenting_ && diagnosticCallback_)
        diagnosticCallback_(L"Dock taskbar phase: overlay-before-hide");
    if (transitionWindow)
    {
        ShowWindow(
            transitionWindow, SW_HIDE);
        SetWindowRgn(
            transitionWindow, nullptr, FALSE);
    }
    if (presenting_ && diagnosticCallback_)
        diagnosticCallback_(L"Dock taskbar phase: overlay-after-hide");
    const bool wasPresenting = std::exchange(presenting_, false);
    hasOcclusionRegion_ = false;
    occlusionRects_.clear();
    occlusionHostBounds_ = {};
    if (wasPresenting && presentationCallback_)
        presentationCallback_(nullptr);
    if (wasPresenting && diagnosticCallback_)
        diagnosticCallback_(L"Dock taskbar phase: after-dock-layer-restore");
    if (compositionEffect_)
        compositionEffect_->SetOpacity(0.0f);
    ClearGenieStrips();
    compositionWindowImage_.Reset();
    compositionSharedWindowActive_ = false;
    if (compositionScaleTransform_)
    {
        compositionScaleTransform_->SetScaleX(1.0f);
        compositionScaleTransform_->SetScaleY(1.0f);
    }
    if (compositionVisual_)
        compositionVisual_->SetContent(nullptr);
    compositionImageSize_ = {};
    compositionSourceRegion_ = {};
    if (compositionImageActive_ && compositionDevice_)
        compositionDevice_->Commit();
    compositionImageActive_ = false;
    compositionTimelineActive_ = false;
    if (sourceCloaked_)
    {
        // Release the real window only after retiring the composition scene
        // and acknowledging the minimized/restored frame.
        DwmFlush();
        ReleaseSourceCloak();
    }
    sourceWindow_ = nullptr;
    fromRect_ = {};
    toRect_ = {};
    windowRect_ = {};
    dockRect_ = {};
    imageHostRect_ = {};
    animationMonitorRects_.clear();
    lastFrameRect_ = {};
    lastFrameOpacity_ = 0;
    hasLastFrame_ = false;
    animationStartTimeMs_ = 0.0;
    animationDurationMs_ =
        static_cast<double>(
            kAnimationDurationMs);
    restoreCleanupDeadlineMs_ = 0.0;
    restoreVisibleTimeMs_ = 0.0;
    restoreFadeStartTimeMs_ = 0.0;
    animationFromOpacity_ = 255;
    animationToOpacity_ = 0;
    awaitingRestoreVisibility_ = false;
    preparingRestore_ = false;
    restoreRequested_ = false;
    restoreActivated_ = false;
    restoreGeometryStableTimeMs_ = 0.0;
    restoreGeometryRect_ = {};
    effect_ = 1;
    collapseFrom_ = 0.0;
    collapseTo_ = 1.0;
    lastCollapse_ = 0.0;
    lastGenieCollapse_ = -1.0;
    restoreCallback_ = {};
}

void DockWindowTransition::Cancel()
{
    Finish();
}

void DockWindowTransition::CompleteImmediately()
{
    // The source has already received the native minimize command. A restore
    // starts underneath the overlay, so complete any pending source restoration
    // explicitly before removing the presentation window.
    if (direction_ == DockWindowTransitionDirection::Restore &&
        restoreCallback_ && sourceWindow_ && IsWindow(sourceWindow_))
    {
        if (IsIconic(sourceWindow_))
            CompleteRestoreAfterRenderFailure();
        else
            ActivateRestoredWindowForHandoff();
    }
    Finish();
}

bool DockWindowTransition::IsActive() const
{
    return sourceWindow_ != nullptr && compositionSharedWindowActive_;
}

bool DockWindowTransition::IsActiveFor(
    HWND window) const
{
    if (!IsActive() ||
        !window ||
        !IsWindow(window))
        return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root)
        root = window;
    return sourceWindow_ == root;
}

DockWindowTransitionDirection
DockWindowTransition::GetDirection() const
{
    return direction_;
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
        {
            self->ReleaseSourceCloak();
            self->hwnd_ = nullptr;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(
        window, message, wParam, lParam);
}
