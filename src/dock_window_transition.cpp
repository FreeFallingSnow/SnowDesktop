#include "dock_window_transition.h"
#include "dock_window_rules.h"
#include "animation_settings.h"
#include "dock_window_capture_isolation.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

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
    const RECT bounds = surface_ == DockWindowTransitionSurface::Snapshot
        ? snapshotHostRect_ : lastFrameRect_;
    const int radius = surface_ == DockWindowTransitionSurface::Snapshot
        ? 0 : ResolveDockWindowTransitionCornerRadius(bounds, dockRect_);
    if (!ApplyOcclusion(bounds, radius))
        CompleteImmediately();
}

void DockWindowTransition::LogPresentation(int requestedEffect,
    DockWindowTransitionCapturePolicy requestedPolicy,
    DockWindowTransitionCapturePolicy actualPolicy,
    const wchar_t* fallbackStage)
{
    if (!diagnosticCallback_)
        return;
    wchar_t message[512]{};
    swprintf_s(message,
        L"Dock transition: hwnd=%p direction=%ls requested=%d effective=%d "
        L"policy=%d/%d snapshot=%ls surface=%d fallback=%ls result=0x%08lX",
        static_cast<void*>(sourceWindow_),
        direction_ == DockWindowTransitionDirection::Minimize ? L"minimize" : L"restore",
        requestedEffect, effect_, static_cast<int>(requestedPolicy),
        static_cast<int>(actualPolicy), snapshotSource_, static_cast<int>(surface_),
        fallbackStage, static_cast<unsigned long>(snapshotResult_));
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

RECT ResolveDockWindowSnapshotHostRect(
    const RECT& from, const RECT& to) noexcept
{
    return {
        std::min(from.left, to.left),
        std::min(from.top, to.top),
        std::max(from.right, to.right),
        std::max(from.bottom, to.bottom)
    };
}

SIZE ConstrainDockWindowSnapshotSize(
    SIZE source, LONG maximumWidth,
    LONG maximumHeight) noexcept
{
    if (source.cx <= 0 || source.cy <= 0 ||
        maximumWidth <= 0 || maximumHeight <= 0)
        return {};
    const double scale = std::min({
        1.0,
        static_cast<double>(maximumWidth) /
            static_cast<double>(source.cx),
        static_cast<double>(maximumHeight) /
            static_cast<double>(source.cy)
    });
    return {
        std::max<LONG>(1, static_cast<LONG>(
            std::lround(source.cx * scale))),
        std::max<LONG>(1, static_cast<LONG>(
            std::lround(source.cy * scale)))
    };
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
    d2dDevice_.Reset();
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
    ID2D1Device* d2dDevice,
    IDCompositionDesktopDevice* compositionDevice)
{
    instance_ = instance;
    animationScheduler_ = animationScheduler;
    d2dDevice_ = d2dDevice;
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

    hwnd_ = CreateWindowExW(
        kDockWindowTransitionExStyle,
        kDockWindowTransitionClassName,
        L"Dock Window Transition",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!hwnd_)
        return false;

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
    DockWindowTransitionCapturePolicy capturePolicy,
    HWND keepBelowWindow)
{
    return Start(
        sourceWindow, dockRect,
        DockWindowTransitionDirection::Minimize,
        {}, capturePolicy, keepBelowWindow);
}

bool DockWindowTransition::PrimeMinimizeSnapshot(
    HWND sourceWindow)
{
    if (IsActive() ||
        !SystemWindowAnimationsEnabled() ||
        !sourceWindow || !IsWindow(sourceWindow))
        return false;

    HWND root = GetAncestor(sourceWindow, GA_ROOT);
    sourceWindow = root ? root : sourceWindow;
    RECT windowRect{};
    if (!ResolveVisibleWindowRect(
            sourceWindow, windowRect))
        return false;
    lastVisibleRects_[sourceWindow] = windowRect;
    return PrepareSnapshot(
        sourceWindow, windowRect,
        DockWindowTransitionDirection::Minimize,
        false) != nullptr;
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
        DockWindowTransitionCapturePolicy::SnapshotPreferred,
        keepBelowWindow);
}

bool DockWindowTransition::Start(
    HWND sourceWindow, RECT dockRect,
    DockWindowTransitionDirection direction,
    RestoreCallback restoreCallback,
    DockWindowTransitionCapturePolicy capturePolicy,
    HWND keepBelowWindow)
{
    if (!SystemWindowAnimationsEnabled() ||
        !sourceWindow || !IsWindow(sourceWindow) ||
        !IsUsableRect(dockRect))
        return false;

    HWND root = GetAncestor(sourceWindow, GA_ROOT);
    sourceWindow = root ? root : sourceWindow;
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
    sourceWindow_ = sourceWindow;
    direction_ = direction;
    restoreCallback_ = std::move(restoreCallback);
    effect_ = snowdesktop::animation::RuntimeWindowEffect();
    const int requestedEffect = effect_;
    const auto requestedPolicy = capturePolicy;
    capturePolicy = ResolveDockWindowCapturePolicy(effect_, capturePolicy);
    snapshotSource_ = L"policy-skip";
    snapshotResult_ = S_OK;
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

    const CachedSnapshot* snapshot = nullptr;
    if (capturePolicy !=
        DockWindowTransitionCapturePolicy::LiveThumbnailOnly)
    {
        snapshot = PrepareSnapshot(
            sourceWindow_, windowRect,
            direction_, true);
    }
    if (!EnsureWindow())
    {
        Cancel();
        return false;
    }

    bool snapshotAvailable = false;
    const wchar_t* fallbackStage = L"none";
    if (snapshot)
    {
        snapshotAvailable =
            CreateCompositionSnapshot(*snapshot);
        if (!snapshotAvailable)
            fallbackStage = L"snapshot-upload";
    }
    else
    {
        fallbackStage = snapshotSource_;
    }

    bool liveThumbnailAvailable = false;
    if (!snapshotAvailable)
    {
        liveThumbnailAvailable =
            SUCCEEDED(DwmRegisterThumbnail(
                hwnd_, sourceWindow_, &thumbnail_)) &&
            thumbnail_;
    }
    surface_ =
        ResolveDockWindowTransitionSurface(
            snapshotAvailable,
            liveThumbnailAvailable,
            capturePolicy);
    if (surface_ ==
        DockWindowTransitionSurface::None)
    {
        LogPresentation(requestedEffect, requestedPolicy, capturePolicy, L"no-surface");
        Cancel();
        return false;
    }

    effect_ = snowdesktop::dock_genie::EffectiveEffect(effect_, snapshotAvailable);
    if (effect_ == 3 && !CreateGenieStrips())
    {
        // Keep the selected preference. Only this presentation falls back.
        ClearGenieStrips();
        effect_ = 1;
        fallbackStage = L"genie-strips";
    }
    LogPresentation(requestedEffect, requestedPolicy, capturePolicy, fallbackStage);
    animationDurationMs_ = TransitionDuration(effect_);

    snapshotHostRect_ =
        surface_ ==
            DockWindowTransitionSurface::Snapshot
        ? ResolveDockWindowSnapshotHostRect(
            fromRect_, toRect_)
        : fromRect_;
    const int hostWidth = std::max(
        1L, snapshotHostRect_.right -
            snapshotHostRect_.left);
    const int hostHeight = std::max(
        1L, snapshotHostRect_.bottom -
            snapshotHostRect_.top);
    // The host callback keeps entire Dock content/backdrop pairs above us.
    // Only legacy callers without that callback use the single-window anchor.
    const HWND insertAfter = !presentationCallback_ &&
        keepBelowWindow && IsWindow(keepBelowWindow)
        ? keepBelowWindow : HWND_TOPMOST;
    SetWindowPos(
        hwnd_, insertAfter,
        snapshotHostRect_.left,
        snapshotHostRect_.top,
        hostWidth, hostHeight,
        SWP_NOACTIVATE);

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
        presentationHr = compositionSnapshotActive_ &&
                compositionDevice_
            ? compositionDevice_->WaitForCommitCompletion()
            : E_NOTIMPL;
        if (FAILED(presentationHr))
            presentationHr = DwmFlush();
    }
    if (FAILED(presentationHr))
    {
        Cancel();
        return false;
    }

    animationStartTimeMs_ =
        MonotonicTimeMilliseconds();
    awaitingRestoreVisibility_ = false;
    restoreCleanupDeadlineMs_ = 0.0;
    restoreVisibleTimeMs_ = 0.0;
    restoreFadeStartTimeMs_ = 0.0;
    if (!ScheduleAnimationWake())
    {
        Cancel();
        return false;
    }
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
    restoreCleanupDeadlineMs_ = 0.0;
    restoreVisibleTimeMs_ = 0.0;
    restoreFadeStartTimeMs_ = 0.0;
    awaitingRestoreVisibility_ = false;
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

bool DockWindowTransition::CaptureSnapshot(
    HWND window, const RECT& sourceRect,
    CachedSnapshot& snapshot)
{
    snapshotResult_ = E_FAIL;
    if (!window || !IsWindow(window) ||
        !IsUsableRect(sourceRect))
        return false;

    const SIZE sourceSize{
        sourceRect.right - sourceRect.left,
        sourceRect.bottom - sourceRect.top
    };
    const SIZE pixelSize =
        ConstrainDockWindowSnapshotSize(sourceSize);
    if (pixelSize.cx <= 0 || pixelSize.cy <= 0)
        return false;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc)
        return false;
    HDC snapshotDc = CreateCompatibleDC(screenDc);
    if (!snapshotDc)
    {
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize =
        sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth =
        pixelSize.cx;
    bitmapInfo.bmiHeader.biHeight =
        -pixelSize.cy;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bitmapBits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screenDc, &bitmapInfo,
        DIB_RGB_COLORS, &bitmapBits,
        nullptr, 0);
    if (!bitmap || !bitmapBits)
    {
        if (bitmap)
            DeleteObject(bitmap);
        DeleteDC(snapshotDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    HGDIOBJ previousBitmap =
        SelectObject(snapshotDc, bitmap);
    SetStretchBltMode(snapshotDc, HALFTONE);
    SetBrushOrgEx(snapshotDc, 0, 0, nullptr);
    BOOL captured = FALSE;
    const HWND foregroundBeforeCapture = GetForegroundWindow();
    snowdesktop::dock_capture::IsolationReport isolationReport;
    {
        snowdesktop::dock_capture::ScopedWindowCaptureIsolation isolation(
            window, sourceRect, &isolationReport);
        if (isolation.Ready())
        {
            captured = StretchBlt(
                snapshotDc,
                0, 0, pixelSize.cx, pixelSize.cy,
                screenDc,
                sourceRect.left, sourceRect.top,
                sourceSize.cx, sourceSize.cy,
                SRCCOPY | CAPTUREBLT);
            if (!captured)
            {
                const DWORD error = GetLastError();
                snapshotResult_ = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            }
            GdiFlush();
        }
        else
        {
            snapshotSource_ = L"capture-isolation-failed";
            const auto& failure = isolationReport.preparationFailure;
            snapshotResult_ = FAILED(failure.result) ? failure.result :
                HRESULT_FROM_WIN32(failure.error ? failure.error : ERROR_GEN_FAILURE);
        }
    }
    if (diagnosticCallback_)
    {
        wchar_t message[320]{};
        swprintf_s(message,
            L"Dock taskbar phase: capture target=%p excluded=%zu hidden=%zu "
            L"foreground=%p/%p captured=%d preparation=%ls",
            static_cast<void*>(window), isolationReport.excludedWindows,
            isolationReport.hiddenWindows, static_cast<void*>(foregroundBeforeCapture),
            static_cast<void*>(GetForegroundWindow()), captured ? 1 : 0,
            isolationReport.preparationFailure.operation
                ? isolationReport.preparationFailure.operation : L"ok");
        diagnosticCallback_(message);
    }
    if (isolationReport.restorationFailure.operation && diagnosticCallback_)
    {
        const auto& failure = isolationReport.restorationFailure;
        wchar_t message[256]{};
        swprintf_s(message,
            L"Dock capture isolation: restore=%ls hwnd=%p error=%lu result=0x%08lX",
            failure.operation, static_cast<void*>(failure.window), failure.error,
            static_cast<unsigned long>(failure.result));
        diagnosticCallback_(message);
    }

    if (previousBitmap)
        SelectObject(snapshotDc, previousBitmap);
    if (captured)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(
            window, &processId);
        snapshotResult_ = S_OK;
        snapshot.processId = processId;
        snapshot.pixelSize = pixelSize;
        snapshot.sourceRect = sourceRect;
        snapshot.capturedTick =
            GetTickCount64();
        snapshot.lastUsedTick =
            snapshot.capturedTick;
        const auto* firstPixel =
            static_cast<const std::uint32_t*>(
                bitmapBits);
        snapshot.pixels.assign(
            firstPixel,
            firstPixel +
                static_cast<std::size_t>(
                    pixelSize.cx) *
                static_cast<std::size_t>(
                    pixelSize.cy));
    }

    DeleteObject(bitmap);
    DeleteDC(snapshotDc);
    ReleaseDC(nullptr, screenDc);
    return captured != FALSE &&
        !snapshot.pixels.empty();
}

void DockWindowTransition::PurgeSnapshotCache()
{
    for (auto iterator = snapshotCache_.begin();
         iterator != snapshotCache_.end();)
    {
        DWORD processId = 0;
        if (!IsWindow(iterator->first))
        {
            lastVisibleRects_.erase(iterator->first);
            iterator = snapshotCache_.erase(iterator);
            continue;
        }
        GetWindowThreadProcessId(
            iterator->first, &processId);
        if (!processId ||
            processId != iterator->second.processId)
        {
            lastVisibleRects_.erase(iterator->first);
            iterator = snapshotCache_.erase(iterator);
            continue;
        }
        ++iterator;
    }
}

const DockWindowTransition::CachedSnapshot*
DockWindowTransition::PrepareSnapshot(
    HWND window, const RECT& sourceRect,
    DockWindowTransitionDirection direction,
    bool allowFreshMinimizeSnapshot)
{
    PurgeSnapshotCache();
    snapshotSource_ = L"capture-failed";
    snapshotResult_ = S_OK;
    if (direction ==
        DockWindowTransitionDirection::Minimize)
    {
        const ULONGLONG now = GetTickCount64();
        const auto primed =
            snapshotCache_.find(window);
        if (allowFreshMinimizeSnapshot &&
            primed != snapshotCache_.end() &&
            !primed->second.pixels.empty() &&
            EqualRect(
                &primed->second.sourceRect,
                &sourceRect) != FALSE &&
            now >= primed->second.capturedTick &&
            now - primed->second.capturedTick <=
                kPrimedSnapshotLifetimeMs)
        {
            primed->second.lastUsedTick = now;
            snapshotSource_ = L"primed-cache";
            return &primed->second;
        }

        CachedSnapshot captured;
        if (!CaptureSnapshot(
                window, sourceRect, captured))
            return nullptr;

        std::size_t cachedBytes = 0;
        for (const auto& [cachedWindow, entry] :
             snapshotCache_)
        {
            if (cachedWindow != window)
                cachedBytes +=
                    entry.pixels.size() *
                    sizeof(std::uint32_t);
        }
        const std::size_t capturedBytes =
            captured.pixels.size() *
            sizeof(std::uint32_t);
        while (!snapshotCache_.empty() &&
            ((!snapshotCache_.contains(window) &&
              snapshotCache_.size() >=
                  kMaximumCachedSnapshots) ||
             cachedBytes + capturedBytes >
                 kMaximumCachedSnapshotBytes))
        {
            auto oldest =
                snapshotCache_.end();
            for (auto iterator =
                     snapshotCache_.begin();
                 iterator !=
                     snapshotCache_.end();
                 ++iterator)
            {
                if (iterator->first == window)
                    continue;
                if (oldest == snapshotCache_.end() ||
                    PreferDockSnapshotEviction(
                        IsIconic(iterator->first) != FALSE,
                        iterator->second.lastUsedTick,
                        IsIconic(oldest->first) != FALSE,
                        oldest->second.lastUsedTick))
                    oldest = iterator;
            }
            if (oldest == snapshotCache_.end())
                break;
            cachedBytes -=
                oldest->second.pixels.size() *
                sizeof(std::uint32_t);
            lastVisibleRects_.erase(
                oldest->first);
            snapshotCache_.erase(oldest);
        }
        auto [iterator, inserted] =
            snapshotCache_.insert_or_assign(
                window, std::move(captured));
        (void)inserted;
        snapshotSource_ = L"fresh-capture";
        return &iterator->second;
    }

    const auto cached =
        snapshotCache_.find(window);
    if (cached == snapshotCache_.end() ||
        cached->second.pixels.empty())
    {
        snapshotSource_ = L"restore-cache-miss";
        return nullptr;
    }
    cached->second.lastUsedTick =
        GetTickCount64();
    snapshotSource_ = L"restore-cache";
    return &cached->second;
}

bool DockWindowTransition::CreateCompositionSnapshot(
    const CachedSnapshot& snapshot)
{
    compositionSnapshotActive_ = false;
    compositionSurface_.Reset();
    compositionSnapshotSize_ = {};
    if (!compositionDevice_ || !d2dDevice_ ||
        !hwnd_ || !IsWindow(hwnd_) ||
        snapshot.pixelSize.cx <= 0 ||
        snapshot.pixelSize.cy <= 0 ||
        snapshot.pixels.empty())
    {
        snapshotResult_ = E_UNEXPECTED;
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
            snapshotResult_ = hr;
            compositionClip_.Reset();
            compositionEffect_.Reset();
            compositionScaleTransform_.Reset();
            compositionVisual_.Reset();
            compositionTarget_.Reset();
            return false;
        }
    }

    const UINT width = static_cast<UINT>(snapshot.pixelSize.cx);
    const UINT height = static_cast<UINT>(snapshot.pixelSize.cy);
    hr = compositionDevice_->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &compositionSurface_);
    if (FAILED(hr) || !compositionSurface_)
    {
        snapshotResult_ = FAILED(hr) ? hr : E_FAIL;
        return false;
    }

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    hr = compositionSurface_->BeginDraw(
        nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext),
        &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        snapshotResult_ = FAILED(hr) ? hr : E_FAIL;
        compositionSurface_.Reset();
        return false;
    }

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    context.Attach(rawContext);
    context->SetDpi(
        kDockWindowSnapshotRenderDpi,
        kDockWindowSnapshotRenderDpi);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(updateOffset.x),
        static_cast<float>(updateOffset.y)));
    context->Clear(D2D1::ColorF(0, 0, 0, 0));

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    const D2D1_BITMAP_PROPERTIES1 properties =
        D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_IGNORE),
            kDockWindowSnapshotRenderDpi,
            kDockWindowSnapshotRenderDpi);
    hr = context->CreateBitmap(
        D2D1::SizeU(width, height),
        snapshot.pixels.data(),
        width * sizeof(std::uint32_t),
        &properties, &bitmap);
    if (SUCCEEDED(hr) && bitmap)
    {
        context->DrawBitmap(
            bitmap.Get(),
            D2D1::RectF(
                0.0f, 0.0f,
                static_cast<float>(width),
                static_cast<float>(height)),
            1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR);
    }
    context.Reset();
    const HRESULT endDrawHr =
        compositionSurface_->EndDraw();
    if (FAILED(hr) || FAILED(endDrawHr))
    {
        snapshotResult_ = FAILED(hr) ? hr : endDrawHr;
        compositionSurface_.Reset();
        return false;
    }

    compositionSnapshotSize_ = snapshot.pixelSize;
    compositionVisual_->SetContent(compositionSurface_.Get());
    compositionEffect_->SetOpacity(0.0f);
    compositionClip_->SetLeft(0.0f);
    compositionClip_->SetTop(0.0f);
    compositionClip_->SetRight(static_cast<float>(width));
    compositionClip_->SetBottom(static_cast<float>(height));
    compositionSnapshotActive_ = true;
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
    if (compositionVisual_)
    {
        compositionVisual_->SetContent(compositionSurface_.Get());
        compositionVisual_->SetTransform(compositionScaleTransform_.Get());
        compositionVisual_->SetClip(compositionClip_.Get());
    }
    hasLastFrame_ = false;
}

bool DockWindowTransition::CreateGenieStrips()
{
    if (!compositionVisual_ || !compositionSurface_ || !compositionDevice_)
    {
        snapshotResult_ = E_UNEXPECTED;
        return false;
    }
    const float width = static_cast<float>(compositionSnapshotSize_.cx);
    const float height = static_cast<float>(compositionSnapshotSize_.cy);
    const bool vertical = snowdesktop::dock_genie::Vertical(genieEdge_);
    genieStrips_.reserve(snowdesktop::dock_genie::StripCount);
    for (std::size_t i = 0; i < snowdesktop::dock_genie::StripCount; ++i)
    {
        Microsoft::WRL::ComPtr<IDCompositionVisual2> strip;
        HRESULT hr = compositionDevice_->CreateVisual(&strip);
        if (FAILED(hr) || !strip)
        {
            snapshotResult_ = FAILED(hr) ? hr : E_FAIL;
            return false;
        }
        genieStrips_.push_back(strip);
        hr = strip->SetContent(compositionSurface_.Get());
        const float begin = static_cast<float>(i) /
            static_cast<float>(snowdesktop::dock_genie::StripCount);
        const float end = static_cast<float>(i + 1) /
            static_cast<float>(snowdesktop::dock_genie::StripCount);
        // Slight overlap avoids transparent seams under fractional GPU sampling.
        // Opacity belongs to the parent so overlapping strips do not darken.
        const D2D1_RECT_F clip = vertical
            ? D2D1::RectF(0, std::max(0.0f, begin * height - 0.35f),
                width, std::min(height, end * height + 0.35f))
            : D2D1::RectF(std::max(0.0f, begin * width - 0.35f), 0,
                std::min(width, end * width + 0.35f), height);
        if (SUCCEEDED(hr)) hr = strip->SetClip(clip);
        if (SUCCEEDED(hr)) hr = strip->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
        if (SUCCEEDED(hr)) hr = strip->SetBitmapInterpolationMode(
            DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (SUCCEEDED(hr)) hr = compositionVisual_->AddVisual(strip.Get(), TRUE, nullptr);
        if (FAILED(hr))
        {
            snapshotResult_ = hr;
            return false;
        }
    }
    HRESULT hr = compositionVisual_->SetContent(nullptr);
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetClip(static_cast<IDCompositionClip*>(nullptr));
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetOffsetX(0.0f);
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetOffsetY(0.0f);
    snapshotResult_ = hr;
    return SUCCEEDED(hr);
}

bool DockWindowTransition::ApplyGenieFrame(double collapsed, BYTE opacity)
{
    if (genieStrips_.size() != snowdesktop::dock_genie::StripCount ||
        !compositionDevice_ || !compositionEffect_)
        return false;
    const auto rect = [](const RECT& value) {
        return snowdesktop::dock_genie::Rect{
            static_cast<double>(value.left), static_cast<double>(value.top),
            static_cast<double>(value.right), static_cast<double>(value.bottom)};
    };
    HRESULT hr = S_OK;
    for (std::size_t i = 0; i < genieStrips_.size() && SUCCEEDED(hr); ++i)
    {
        const auto transform = snowdesktop::dock_genie::StripMatrix(
            rect(windowRect_), rect(dockRect_), genieEdge_, collapsed,
            compositionSnapshotSize_.cx, compositionSnapshotSize_.cy,
            static_cast<double>(i) / static_cast<double>(genieStrips_.size()),
            static_cast<double>(i + 1) / static_cast<double>(genieStrips_.size()),
            snapshotHostRect_.left, snapshotHostRect_.top);
        const D2D1_MATRIX_3X2_F matrix{
            transform.m11, transform.m12, transform.m21,
            transform.m22, transform.dx, transform.dy};
        hr = genieStrips_[i]->SetTransform(matrix);
    }
    if (SUCCEEDED(hr)) hr = compositionEffect_->SetOpacity(static_cast<float>(opacity) / 255.0f);
    if (SUCCEEDED(hr)) hr = compositionDevice_->Commit();
    return SUCCEEDED(hr);
}

bool DockWindowTransition::StartCompositionTimeline()
{
    if (!compositionSnapshotActive_ || !compositionDevice_ ||
        !compositionVisual_ || !compositionScaleTransform_ ||
        !compositionEffect_ || !compositionClip_ ||
        compositionSnapshotSize_.cx <= 0 ||
        compositionSnapshotSize_.cy <= 0 ||
        animationDurationMs_ <= 0.0)
        return false;

    const auto width = [](const RECT& rect) {
        return std::max(1L, rect.right - rect.left);
    };
    const auto height = [](const RECT& rect) {
        return std::max(1L, rect.bottom - rect.top);
    };
    const float fromScaleX = static_cast<float>(width(fromRect_)) /
        static_cast<float>(compositionSnapshotSize_.cx);
    const float fromScaleY = static_cast<float>(height(fromRect_)) /
        static_cast<float>(compositionSnapshotSize_.cy);
    const float toScaleX = static_cast<float>(width(toRect_)) /
        static_cast<float>(compositionSnapshotSize_.cx);
    const float toScaleY = static_cast<float>(height(toRect_)) /
        static_cast<float>(compositionSnapshotSize_.cy);
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
        static_cast<float>(fromRect_.left - snapshotHostRect_.left),
        static_cast<float>(toRect_.left - snapshotHostRect_.left),
        &offsetX);
    if (SUCCEEDED(hr))
        hr = create(
            static_cast<float>(fromRect_.top - snapshotHostRect_.top),
            static_cast<float>(toRect_.top - snapshotHostRect_.top),
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
    if (SUCCEEDED(hr))
        hr = compositionDevice_->Commit();
    compositionTimelineActive_ = SUCCEEDED(hr);
    return compositionTimelineActive_;
}

bool DockWindowTransition::ScheduleAnimationWake()
{
    if (!animationScheduler_)
        return false;
    if (animationToken_)
        animationScheduler_->Cancel(animationToken_);
    animationToken_ = 0;

    if (surface_ == DockWindowTransitionSurface::Snapshot && effect_ != 3)
    {
        if (!StartCompositionTimeline())
            return false;
        animationToken_ = animationScheduler_->ScheduleOnce(
            static_cast<UINT>(std::ceil(animationDurationMs_)) + 2,
            [this](snowdesktop::UiScheduleToken token) {
                if (animationToken_ != token)
                    return;
                animationToken_ = 0;
                compositionTimelineActive_ = false;
                const bool keep = OnAnimationFrame(
                    MonotonicTimeMilliseconds());
                if (keep && awaitingRestoreVisibility_ &&
                    animationScheduler_)
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
    if (!hwnd_ ||
        surface_ ==
            DockWindowTransitionSurface::None)
        return false;

    const RECT frame = InterpolateDockWindowTransitionRect(
        fromRect_, toRect_, progress);
    const int width = std::max(1L, frame.right - frame.left);
    const int height =
        std::max(1L, frame.bottom - frame.top);
    if (surface_ == DockWindowTransitionSurface::Snapshot &&
        !ApplyOcclusion(snapshotHostRect_, 0))
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

    if (surface_ ==
        DockWindowTransitionSurface::Snapshot)
    {
        if (!compositionSnapshotActive_ ||
            !compositionDevice_ ||
            !compositionVisual_ ||
            !compositionScaleTransform_ ||
            !compositionEffect_ ||
            !compositionClip_ ||
            compositionSnapshotSize_.cx <= 0 ||
            compositionSnapshotSize_.cy <= 0)
            return false;

        const float scaleX =
            static_cast<float>(width) /
            static_cast<float>(compositionSnapshotSize_.cx);
        const float scaleY =
            static_cast<float>(height) /
            static_cast<float>(compositionSnapshotSize_.cy);
        HRESULT hr = S_OK;
        if (geometryChanged)
        {
            hr = compositionVisual_->SetOffsetX(
                static_cast<float>(
                    frame.left - snapshotHostRect_.left));
            if (SUCCEEDED(hr))
                hr = compositionVisual_->SetOffsetY(
                    static_cast<float>(
                        frame.top - snapshotHostRect_.top));
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
    else
    {
        if (!thumbnail_)
            return false;
        if (geometryChanged)
        {
            SetWindowPos(
                hwnd_, nullptr,
                frame.left, frame.top,
                width, height,
                SWP_NOACTIVATE |
                    SWP_NOOWNERZORDER |
                    SWP_NOZORDER |
                    SWP_NOSENDCHANGING |
                    SWP_NOCOPYBITS);
            // Live thumbnails move their HWND every frame. Re-evaluate the
            // participating Docks so unrelated monitors keep their usual layer.
            if (presenting_ && presentationCallback_)
                presentationCallback_(hwnd_);
            if (!ApplyOcclusion(frame, cornerRadius))
                return false;
        }
        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags =
            DWM_TNP_RECTDESTINATION |
            DWM_TNP_VISIBLE |
            DWM_TNP_OPACITY |
            DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination =
            { 0, 0, width, height };
        properties.opacity = frameOpacity;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly =
            FALSE;
        if (FAILED(DwmUpdateThumbnailProperties(
                thumbnail_, &properties)))
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
    if (!sourceWindow_ || !IsWindow(sourceWindow_))
    {
        Finish();
        return false;
    }
    if (!snowdesktop::animation::RuntimeAnimationsEnabled())
    {
        CompleteImmediately();
        return false;
    }

    const double now = nowMilliseconds;
    if (awaitingRestoreVisibility_)
    {
        if (restoreFadeStartTimeMs_ > 0.0)
        {
            const double fadeProgress = std::min(
                1.0,
                (now - restoreFadeStartTimeMs_) /
                    static_cast<double>(
                        kRestoreSnapshotFadeDurationMs));
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
                compositionSnapshotActive_ && compositionDevice_
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
            // applications. Hold the opaque snapshot for one presentation,
            // then retire it without changing its final full-window geometry.
            ActivateRestoredWindowForHandoff();
            DwmFlush();
            fromRect_ = windowRect_;
            toRect_ = windowRect_;
            animationFromOpacity_ = lastFrameOpacity_;
            animationToOpacity_ = 0;
            animationStartTimeMs_ = now;
            animationDurationMs_ = static_cast<double>(
                kRestoreSnapshotFadeDurationMs);
            restoreFadeStartTimeMs_ = now;
            compositionTimelineActive_ = false;
            return true;
        }

        // 失败检查：恢复回调已执行，但窗口必须在清理超时内真正退出最小化。
        // 目标进程挂起（例如求解器无法处理 SW_RESTORE）时 IsIconic 会一直
        // 保持为真，此时必须中止动画而不是无限等待，否则过渡层会永久停留
        // 在窗口位置，表现为 Dock 卡死。
        if (!IsIconic(sourceWindow_))
        {
            ActivateRestoredWindowForHandoff();
            restoreVisibleTimeMs_ = now;
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

    if (direction_ == DockWindowTransitionDirection::Restore &&
        restoreCallback_)
    {
        restoreCallback_(
            sourceWindow_,
            DockWindowRestoreTransitionPhase::RequestRestore);
        awaitingRestoreVisibility_ = true;
        restoreCleanupDeadlineMs_ =
            now + static_cast<double>(
                kRestoreCleanupTimeoutMs);
        return true;
    }
    Finish();
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
    restoreCallback_(
        sourceWindow_,
        DockWindowRestoreTransitionPhase::ActivateRestored);
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
    if (animationScheduler_ && animationToken_)
        animationScheduler_->Cancel(animationToken_);
    animationToken_ = 0;
    HWND transitionWindow = hwnd_;
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
    UnregisterThumbnail();
    const bool hadCompositionSnapshot =
        compositionSnapshotActive_;
    if (compositionEffect_)
        compositionEffect_->SetOpacity(0.0f);
    ClearGenieStrips();
    if (compositionScaleTransform_)
    {
        compositionScaleTransform_->SetScaleX(1.0f);
        compositionScaleTransform_->SetScaleY(1.0f);
    }
    if (compositionVisual_)
        compositionVisual_->SetContent(nullptr);
    compositionSurface_.Reset();
    compositionSnapshotSize_ = {};
    if (compositionSnapshotActive_ && compositionDevice_)
        compositionDevice_->Commit();
    compositionSnapshotActive_ = false;
    compositionTimelineActive_ = false;
    if (nativeTransitionsDisabled_)
    {
        // Keep native transitions disabled until DWM has committed the
        // real minimized/restored state. Re-enabling too early lets the
        // system animation trail the snapshot as a dark second window.
        HRESULT completionHr =
            hadCompositionSnapshot && compositionDevice_
            ? compositionDevice_->WaitForCommitCompletion()
            : E_NOTIMPL;
        if (FAILED(completionHr))
            DwmFlush();
        SetNativeTransitionsDisabled(false);
    }
    sourceWindow_ = nullptr;
    surface_ =
        DockWindowTransitionSurface::None;
    fromRect_ = {};
    toRect_ = {};
    windowRect_ = {};
    dockRect_ = {};
    snapshotHostRect_ = {};
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
    effect_ = 1;
    collapseFrom_ = 0.0;
    collapseTo_ = 1.0;
    lastCollapse_ = 0.0;
    restoreCallback_ = {};
}

void DockWindowTransition::Cancel()
{
    Finish();
}

void DockWindowTransition::CompleteImmediately()
{
    // The source has already received the native minimize command. A restore
    // is deferred until the overlay reaches its destination, so complete that
    // request explicitly before removing the presentation window.
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
    return sourceWindow_ != nullptr &&
        surface_ !=
            DockWindowTransitionSurface::None;
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
            self->hwnd_ = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(
        window, message, wParam, lParam);
}
