#pragma once

#include <d2d1.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui/render/ui_animation_scheduler.h"
#include "dock_genie_rules.h"
#include "dock_window_shared_thumbnail.h"

enum class DockWindowTransitionDirection
{
    Minimize,
    Restore,
};

enum class DockWindowRestoreTransitionPhase
{
    RequestRestore,
    ActivateRestored,
    FallbackWithoutAnimation,
};

enum class DockWindowTransitionStartAction
{
    StartNew,
    ContinueActive,
    ReverseActive,
    InterruptRestoreHandoff,
};

constexpr DockWindowTransitionStartAction
ResolveDockWindowTransitionStartAction(
    bool active,
    bool sameWindow,
    bool sameDirection,
    bool awaitingRestoreVisibility = false) noexcept
{
    if (!active || !sameWindow)
        return DockWindowTransitionStartAction::StartNew;
    if (sameDirection)
        return DockWindowTransitionStartAction::ContinueActive;
    return awaitingRestoreVisibility
        ? DockWindowTransitionStartAction::
            InterruptRestoreHandoff
        : DockWindowTransitionStartAction::ReverseActive;
}

constexpr bool RequiresDockWindowTransitionCompositionBarrier(
    DockWindowTransitionDirection direction) noexcept
{
    return direction ==
        DockWindowTransitionDirection::Minimize;
}

double EaseDockWindowTransition(double progress) noexcept;
BYTE ResolveDockWindowTransitionOpacity(
    DockWindowTransitionDirection direction,
    double progress) noexcept;
int ResolveDockWindowTransitionCornerRadius(
    const RECT& frame,
    const RECT& dockRect) noexcept;
RECT InterpolateDockWindowTransitionRect(
    const RECT& from, const RECT& to, double progress) noexcept;
RECT ResolveDockWindowImageHostRect(
    const RECT& from, const RECT& to) noexcept;
// Keep temporary animation HWNDs from covering any complete monitor. At most
// one physical pixel may be clipped; unsafe spanning layouts use native fallback.
std::optional<RECT> ResolveDockWindowNonFullscreenHostRect(
    RECT host, std::span<const RECT> monitors) noexcept;

inline constexpr DWORD kDockWindowTransitionExStyle =
    WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
    WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
    WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP;
inline constexpr DWM_WINDOW_CORNER_PREFERENCE
    kDockWindowTransitionCornerPreference =
        DWMWCP_DONOTROUND;
inline constexpr DWMNCRENDERINGPOLICY
    kDockWindowTransitionNcRenderingPolicy =
        DWMNCRP_DISABLED;
inline constexpr COLORREF
    kDockWindowTransitionBorderColor =
        DWMWA_COLOR_NONE;

// Coordinates are physical screen pixels; the returned region belongs to the
// caller and is local to hostBounds. This also handles negative monitor origins.
HRGN CreateDockWindowTransitionOcclusionRegion(
    const RECT& hostBounds, int cornerRadius,
    const std::vector<RECT>& occluders);

/**
 * @brief 使用 DWM 共享窗口图像在应用窗口与 Dock 图标之间播放过渡。
 *
 * 不采集或缓存窗口截图。共享图像不可用时由调用方执行系统窗口操作。
 * 呈现窗口不抢焦点且鼠标穿透，由宿主维护 Dock 层级。
 */
class DockWindowTransition
{
public:
    using RestoreCallback = std::function<void(
        HWND, DockWindowRestoreTransitionPhase)>;

    DockWindowTransition() = default;
    ~DockWindowTransition();

    DockWindowTransition(const DockWindowTransition&) = delete;
    DockWindowTransition& operator=(const DockWindowTransition&) = delete;

    bool Initialize(
        HINSTANCE instance,
        snowdesktop::UiAnimationScheduler* animationScheduler,
        ID2D1Device* d2dDevice,
        IDCompositionDesktopDevice* compositionDevice);
    bool StartMinimize(
        HWND sourceWindow, RECT dockRect,
        HWND keepBelowWindow = nullptr);
    bool StartRestore(
        HWND sourceWindow, RECT dockRect,
        RestoreCallback restoreCallback,
        HWND keepBelowWindow = nullptr);
    bool StartExternalMinimize(HWND sourceWindow, RECT dockRect, DWORD deadline,
        HWND keepBelowWindow = nullptr);
    void CancelExternalMinimize(HWND sourceWindow, DWORD deadline);
    void SetPresentationCallback(std::function<void(HWND)> callback)
    {
        presentationCallback_ = std::move(callback);
    }
    void SetOcclusionRectsProvider(std::function<std::vector<RECT>()> provider)
    {
        occlusionRectsProvider_ = std::move(provider);
    }
    void SetDiagnosticCallback(std::function<void(const wchar_t*)> callback)
    {
        diagnosticCallback_ = std::move(callback);
    }
    HWND GetPresentationWindow() const noexcept
    {
        return presenting_ ? hwnd_ : nullptr;
    }
    void RefreshOcclusion();
    void Cancel();
    // Settings changes must not abandon an in-flight restore request.
    void CompleteImmediately();
    bool IsActive() const;
    bool RequiresNativeAnimationFallback() const noexcept { return nativeFallbackRequested_; }
    bool IsActiveFor(HWND window) const;
    bool IsPresentationWindow(HWND window) const noexcept
    {
        return window && window == hwnd_;
    }
    DockWindowTransitionDirection GetDirection() const;

private:
    static constexpr ULONGLONG kAnimationDurationMs = 240;
    static constexpr ULONGLONG
        kMinimumReverseDurationMs = 80;
    static constexpr ULONGLONG kRestoreCleanupTimeoutMs = 1000;
    static constexpr ULONGLONG kMinimizeCleanupTimeoutMs = 1000;
    static constexpr ULONGLONG
        kRestorePresentationDelayMs = 16;
    static constexpr ULONGLONG
        kRestoreImageFadeDurationMs = 56;
    static LRESULT CALLBACK WindowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureWindow();
    bool Start(
        HWND sourceWindow, RECT dockRect,
        DockWindowTransitionDirection direction,
        RestoreCallback restoreCallback,
        HWND keepBelowWindow);
    bool Reverse(
        DockWindowTransitionDirection direction,
        RestoreCallback restoreCallback);
    bool ResolveVisibleWindowRect(HWND window, RECT& rect) const;
    bool ResolveRestoreWindowRect(HWND window, RECT& rect) const;
    bool EnsureCompositionVisuals();
    bool CreateSharedWindowImage();
    bool UsesCompositionImage() const noexcept
    {
        return compositionSharedWindowActive_;
    }
    bool CreateGenieStrips();
    bool ApplyGenieFrame(double progress, BYTE opacity);
    void ClearGenieStrips();
    bool StartCompositionTimeline(bool opacityOnly = false);
    bool CommitCompositionTimeline(std::span<IDCompositionAnimation* const> animations);
    bool ScheduleAnimationWake();
    bool ApplyFrame(double progress);
    bool ApplyOcclusion(const RECT& hostBounds, int cornerRadius);
    void LogPresentation(int requestedEffect,
        const wchar_t* fallbackStage);
    bool OnAnimationFrame(double nowMilliseconds);
    void RequestRestoreForAnimation();
    bool PrepareRestoredWindow(double nowMilliseconds);
    bool RestoreGeometryReady(RECT& frame) const;
    void Finish();
    void CompleteRestoreAfterRenderFailure();
    void ActivateRestoredWindowForHandoff();
    void ReleaseSourceCloak();

    HINSTANCE instance_ = nullptr;
    snowdesktop::UiAnimationScheduler* animationScheduler_ = nullptr;
    snowdesktop::UiScheduleToken animationToken_ = 0;
    HWND hwnd_ = nullptr;
    bool presenting_ = false;
    std::function<void(HWND)> presentationCallback_;
    std::function<std::vector<RECT>()> occlusionRectsProvider_;
    std::function<void(const wchar_t*)> diagnosticCallback_;
    RECT occlusionHostBounds_{};
    int occlusionCornerRadius_ = 0;
    std::vector<RECT> occlusionRects_;
    bool hasOcclusionRegion_ = false;
    const wchar_t* imageSource_ = L"none";
    HRESULT imageResult_ = S_OK;
    HWND sourceWindow_ = nullptr;
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice>
        compositionDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget>
        compositionTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual2>
        compositionVisual_;
    Microsoft::WRL::ComPtr<IDCompositionScaleTransform>
        compositionScaleTransform_;
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup>
        compositionEffect_;
    Microsoft::WRL::ComPtr<IDCompositionRectangleClip>
        compositionClip_;
    SIZE compositionImageSize_{};
    RECT compositionSourceRegion_{};
    bool compositionImageActive_ = false;
    bool compositionTimelineActive_ = false;
    std::vector<Microsoft::WRL::ComPtr<IDCompositionVisual3>> genieStrips_;
    snowdesktop::dock_thumbnail::SharedVisual compositionWindowImage_;
    std::vector<snowdesktop::dock_thumbnail::SharedVisual> genieWindowImages_;
    bool compositionSharedWindowActive_ = false;
    snowdesktop::dock_genie::Edge genieEdge_ =
        snowdesktop::dock_genie::Edge::Bottom;
    int effect_ = 1;
    double collapseFrom_ = 0.0;
    double collapseTo_ = 1.0;
    double lastCollapse_ = 0.0;
    double lastGenieCollapse_ = -1.0;
    DockWindowTransitionDirection direction_ =
        DockWindowTransitionDirection::Minimize;
    RECT fromRect_{};
    RECT toRect_{};
    RECT windowRect_{};
    RECT dockRect_{};
    std::vector<RECT> animationMonitorRects_;
    bool nativeFallbackRequested_ = false;
    bool externalMinimize_ = false;
    bool externalMinimizeObserved_ = false;
    DWORD externalMinimizeDeadline_ = 0;
    bool minimizeObserved_ = false;
    double minimizeCleanupDeadlineMs_ = 0.0;
    RECT imageHostRect_{};
    RECT lastFrameRect_{};
    BYTE lastFrameOpacity_ = 0;
    bool hasLastFrame_ = false;
    double animationStartTimeMs_ = 0.0;
    double animationDurationMs_ =
        static_cast<double>(kAnimationDurationMs);
    double restoreCleanupDeadlineMs_ = 0.0;
    double restoreVisibleTimeMs_ = 0.0;
    double restoreFadeStartTimeMs_ = 0.0;
    BYTE animationFromOpacity_ = 255;
    BYTE animationToOpacity_ = 0;
    bool awaitingRestoreVisibility_ = false;
    bool preparingRestore_ = false;
    bool restoreRequested_ = false;
    bool restoreActivated_ = false;
    double restoreGeometryStableTimeMs_ = 0.0;
    RECT restoreGeometryRect_{};
    bool sourceCloaked_ = false;
    HWND sourceCloakWindow_ = nullptr;
    RestoreCallback restoreCallback_;
    std::unordered_map<HWND, RECT> lastVisibleRects_;
};
