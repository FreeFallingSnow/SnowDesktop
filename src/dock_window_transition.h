#pragma once

#include <d2d1.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui_animation_scheduler.h"
#include "dock_genie_rules.h"

namespace snowdesktop::dock_snapshot_warmup
{
struct Request;
}

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

// Genie requires a deformable surface. CaptureSnapshot isolates our own
// overlaid windows, so a floating Dock no longer forces it to use DWM scale.
constexpr DockWindowTransitionCapturePolicy ResolveDockWindowCapturePolicy(
    int effect, DockWindowTransitionCapturePolicy requested) noexcept
{
    return effect == 3
        ? DockWindowTransitionCapturePolicy::SnapshotPreferred : requested;
}

constexpr bool PreferDockSnapshotEviction(
    bool candidateMinimized, ULONGLONG candidateLastUsed,
    bool oldestMinimized, ULONGLONG oldestLastUsed) noexcept
{
    return candidateMinimized != oldestMinimized
        ? !candidateMinimized : candidateLastUsed < oldestLastUsed;
}

// Coordinates are physical screen pixels; the returned region belongs to the
// caller and is local to hostBounds. This also handles negative monitor origins.
HRGN CreateDockWindowTransitionOcclusionRegion(
    const RECT& hostBounds, int cornerRadius,
    const std::vector<RECT>& occluders);

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
 * 动画期间由 GPU 变换静态位图。抓取期间临时排除自身上层窗口，避免把
 * Dock 写入快照；神奇效果优先快照，其余效果可使用目标 HWND 的 DWM
 * 缩略图。该窗口不抢焦点且鼠标穿透，呈现期由宿主维护 Dock 遮挡层级。
 * 神奇还原也可复用正常前台窗口的低频后台快照，覆盖应用自身最小化入口；
 * 后台捕获不隐藏、激活或重排窗口，缺少可靠画面时仍回退系统缩略图。
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
    bool PrimeMinimizeSnapshot(HWND sourceWindow);
    // Called by the existing maintenance timer, never by an animation frame.
    void UpdateSnapshotWarmup(HWND foregroundWindow, DWORD foregroundAge);
    bool StartMinimize(
        HWND sourceWindow, RECT dockRect,
        DockWindowTransitionCapturePolicy capturePolicy =
            DockWindowTransitionCapturePolicy::SnapshotPreferred,
        HWND keepBelowWindow = nullptr);
    bool StartRestore(
        HWND sourceWindow, RECT dockRect,
        RestoreCallback restoreCallback,
        HWND keepBelowWindow = nullptr);
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
    static constexpr ULONGLONG
        kPrimedSnapshotLifetimeMs = 500;
    static constexpr ULONGLONG kRestoreCleanupTimeoutMs = 240;
    static constexpr ULONGLONG
        kRestorePresentationDelayMs = 16;
    static constexpr ULONGLONG
        kRestoreSnapshotFadeDurationMs = 56;
    static constexpr std::size_t kMaximumCachedSnapshots = 16;
    static constexpr std::size_t
        kMaximumCachedSnapshotBytes =
            96ULL * 1024ULL * 1024ULL;

    struct CachedSnapshot
    {
        DWORD processId = 0;
        DWORD threadId = 0;
        SIZE pixelSize{};
        RECT sourceRect{};
        WINDOWPLACEMENT placement{};
        bool background = false;
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
        CachedSnapshot& snapshot);
    const CachedSnapshot* PrepareSnapshot(
        HWND window, const RECT& sourceRect,
        DockWindowTransitionDirection direction,
        bool allowFreshMinimizeSnapshot);
    void PurgeSnapshotCache();
    void CollectSnapshotWarmup();
    const CachedSnapshot* StoreSnapshot(HWND window, CachedSnapshot snapshot);
    bool CreateCompositionSnapshot(
        const CachedSnapshot& snapshot);
    bool CreateGenieStrips();
    bool ApplyGenieFrame(double progress, BYTE opacity);
    void ClearGenieStrips();
    bool StartCompositionTimeline();
    bool ScheduleAnimationWake();
    bool ApplyFrame(double progress);
    bool ApplyOcclusion(const RECT& hostBounds, int cornerRadius);
    void LogPresentation(int requestedEffect,
        DockWindowTransitionCapturePolicy requestedPolicy,
        DockWindowTransitionCapturePolicy actualPolicy,
        const wchar_t* fallbackStage);
    bool OnAnimationFrame(double nowMilliseconds);
    void Finish();
    void CompleteRestoreAfterRenderFailure();
    void ActivateRestoredWindowForHandoff();
    void SetNativeTransitionsDisabled(bool disabled);
    void UnregisterThumbnail();

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
    const wchar_t* snapshotSource_ = L"none";
    HRESULT snapshotResult_ = S_OK;
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
    std::vector<Microsoft::WRL::ComPtr<IDCompositionVisual2>> genieStrips_;
    snowdesktop::dock_genie::Edge genieEdge_ =
        snowdesktop::dock_genie::Edge::Bottom;
    int effect_ = 1;
    double collapseFrom_ = 0.0;
    double collapseTo_ = 1.0;
    double lastCollapse_ = 0.0;
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
    double restoreVisibleTimeMs_ = 0.0;
    double restoreFadeStartTimeMs_ = 0.0;
    BYTE animationFromOpacity_ = 255;
    BYTE animationToOpacity_ = 0;
    bool awaitingRestoreVisibility_ = false;
    bool nativeTransitionsDisabled_ = false;
    RestoreCallback restoreCallback_;
    std::unordered_map<HWND, RECT> lastVisibleRects_;
    std::unordered_map<HWND, CachedSnapshot>
        snapshotCache_;
    std::shared_ptr<snowdesktop::dock_snapshot_warmup::Request> snapshotWarmup_;
    ULONGLONG lastSnapshotWarmupAttempt_ = 0;
};
