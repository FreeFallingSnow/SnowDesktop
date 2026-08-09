#pragma once

#include <d2d1.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "ui_animation_scheduler.h"

enum class DockWindowTransitionDirection
{
    Minimize,
    Restore,
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
RECT ResolveDockWindowSnapshotHostRect(
    const RECT& from, const RECT& to) noexcept;

inline constexpr LONG kDockWindowSnapshotMaxWidth = 4096;
inline constexpr LONG kDockWindowSnapshotMaxHeight = 4096;
inline constexpr FLOAT kDockWindowSnapshotRenderDpi = 96.0f;
inline constexpr bool kDockWindowSnapshotUsesComposition = true;
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

SIZE ConstrainDockWindowSnapshotSize(
    SIZE source,
    LONG maximumWidth = kDockWindowSnapshotMaxWidth,
    LONG maximumHeight = kDockWindowSnapshotMaxHeight) noexcept;

enum class DockWindowTransitionSurface
{
    None,
    Snapshot,
    LiveThumbnail,
};

enum class DockWindowTransitionCapturePolicy
{
    SnapshotPreferred,
    LiveThumbnailOnly,
};

constexpr DockWindowTransitionSurface ResolveDockWindowTransitionSurface(
    bool snapshotAvailable,
    bool liveThumbnailAvailable,
    DockWindowTransitionCapturePolicy capturePolicy =
        DockWindowTransitionCapturePolicy::SnapshotPreferred) noexcept
{
    if (capturePolicy ==
        DockWindowTransitionCapturePolicy::LiveThumbnailOnly)
    {
        return liveThumbnailAvailable
            ? DockWindowTransitionSurface::LiveThumbnail
            : DockWindowTransitionSurface::None;
    }
    return snapshotAvailable
        ? DockWindowTransitionSurface::Snapshot
        : (liveThumbnailAvailable
            ? DockWindowTransitionSurface::LiveThumbnail
            : DockWindowTransitionSurface::None);
}

/**
 * @brief 使用静态窗口快照在应用窗口与 Dock 图标之间播放过渡。
 *
 * 普通桌面 Dock 最小化前仅捕获一次窗口帧，恢复时优先复用缓存帧，
 * 动画期间由 GPU 缩放静态位图。悬浮 Dock 可要求仅使用目标 HWND 的
 * DWM 缩略图，避免把顶层 Dock 写入屏幕快照；注册失败时由调用方决定
 * 是否关闭 Dock 后重试静态快照。该窗口不抢焦点且鼠标穿透。
 */
class DockWindowTransition
{
public:
    using RestoreCallback = std::function<void(HWND)>;

    DockWindowTransition() = default;
    ~DockWindowTransition();

    DockWindowTransition(const DockWindowTransition&) = delete;
    DockWindowTransition& operator=(const DockWindowTransition&) = delete;

    bool Initialize(
        HINSTANCE instance,
        snowdesktop::UiAnimationScheduler* animationScheduler,
        ID2D1Device* d2dDevice,
        IDCompositionDesktopDevice* compositionDevice);
    bool PrimeMinimizeSnapshot(HWND sourceWindow);
    bool StartMinimize(
        HWND sourceWindow, RECT dockRect,
        DockWindowTransitionCapturePolicy capturePolicy =
            DockWindowTransitionCapturePolicy::SnapshotPreferred,
        HWND keepBelowWindow = nullptr);
    bool StartRestore(
        HWND sourceWindow, RECT dockRect,
        RestoreCallback restoreCallback,
        HWND keepBelowWindow = nullptr);
    void Cancel();
    bool IsActive() const;
    bool IsActiveFor(HWND window) const;
    DockWindowTransitionDirection GetDirection() const;

private:
    static constexpr ULONGLONG kAnimationDurationMs = 240;
    static constexpr ULONGLONG
        kMinimumReverseDurationMs = 80;
    static constexpr ULONGLONG
        kPrimedSnapshotLifetimeMs = 500;
    static constexpr ULONGLONG kRestoreCleanupTimeoutMs = 240;
    static constexpr std::size_t kMaximumCachedSnapshots = 3;
    static constexpr std::size_t
        kMaximumCachedSnapshotBytes =
            96ULL * 1024ULL * 1024ULL;

    struct CachedSnapshot
    {
        DWORD processId = 0;
        SIZE pixelSize{};
        RECT sourceRect{};
        ULONGLONG capturedTick = 0;
        ULONGLONG lastUsedTick = 0;
        std::vector<std::uint32_t> pixels;
    };

    static LRESULT CALLBACK WindowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureWindow();
    bool Start(
        HWND sourceWindow, RECT dockRect,
        DockWindowTransitionDirection direction,
        RestoreCallback restoreCallback,
        DockWindowTransitionCapturePolicy capturePolicy,
        HWND keepBelowWindow);
    bool Reverse(
        DockWindowTransitionDirection direction,
        RestoreCallback restoreCallback);
    bool ResolveVisibleWindowRect(HWND window, RECT& rect) const;
    bool ResolveRestoreWindowRect(HWND window, RECT& rect) const;
    bool CaptureSnapshot(
        HWND window, const RECT& sourceRect,
        CachedSnapshot& snapshot) const;
    const CachedSnapshot* PrepareSnapshot(
        HWND window, const RECT& sourceRect,
        DockWindowTransitionDirection direction,
        bool allowFreshMinimizeSnapshot);
    void PurgeSnapshotCache();
    bool CreateCompositionSnapshot(
        const CachedSnapshot& snapshot);
    bool StartCompositionTimeline();
    bool ScheduleAnimationWake();
    bool ApplyFrame(double progress);
    bool OnAnimationFrame(double nowMilliseconds);
    void Finish();
    void CompleteRestoreAfterRenderFailure();
    void SetNativeTransitionsDisabled(bool disabled);
    void UnregisterThumbnail();

    HINSTANCE instance_ = nullptr;
    snowdesktop::UiAnimationScheduler* animationScheduler_ = nullptr;
    snowdesktop::UiScheduleToken animationToken_ = 0;
    HWND hwnd_ = nullptr;
    HWND sourceWindow_ = nullptr;
    HTHUMBNAIL thumbnail_ = nullptr;
    DockWindowTransitionSurface surface_ =
        DockWindowTransitionSurface::None;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
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
    Microsoft::WRL::ComPtr<IDCompositionSurface>
        compositionSurface_;
    SIZE compositionSnapshotSize_{};
    bool compositionSnapshotActive_ = false;
    bool compositionTimelineActive_ = false;
    DockWindowTransitionDirection direction_ =
        DockWindowTransitionDirection::Minimize;
    RECT fromRect_{};
    RECT toRect_{};
    RECT windowRect_{};
    RECT dockRect_{};
    RECT snapshotHostRect_{};
    RECT lastFrameRect_{};
    BYTE lastFrameOpacity_ = 0;
    bool hasLastFrame_ = false;
    double animationStartTimeMs_ = 0.0;
    double animationDurationMs_ =
        static_cast<double>(kAnimationDurationMs);
    double restoreCleanupDeadlineMs_ = 0.0;
    BYTE animationFromOpacity_ = 255;
    BYTE animationToOpacity_ = 0;
    bool awaitingRestoreVisibility_ = false;
    bool nativeTransitionsDisabled_ = false;
    RestoreCallback restoreCallback_;
    std::unordered_map<HWND, RECT> lastVisibleRects_;
    std::unordered_map<HWND, CachedSnapshot>
        snapshotCache_;
};
