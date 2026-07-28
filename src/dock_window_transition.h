#pragma once

#include <d2d1.h>
#include <dwmapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

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
};

constexpr DockWindowTransitionStartAction
ResolveDockWindowTransitionStartAction(
    bool active,
    bool sameWindow,
    bool sameDirection) noexcept
{
    if (!active || !sameWindow)
        return DockWindowTransitionStartAction::StartNew;
    return sameDirection
        ? DockWindowTransitionStartAction::ContinueActive
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
inline constexpr D2D1_PRESENT_OPTIONS
    kDockWindowSnapshotPresentOptions =
        D2D1_PRESENT_OPTIONS_NONE;
inline constexpr DWORD kDockWindowTransitionExStyle =
    WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
    WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
    WS_EX_LAYERED;
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

constexpr DockWindowTransitionSurface ResolveDockWindowTransitionSurface(
    bool snapshotAvailable,
    bool liveThumbnailAvailable) noexcept
{
    return snapshotAvailable
        ? DockWindowTransitionSurface::Snapshot
        : (liveThumbnailAvailable
            ? DockWindowTransitionSurface::LiveThumbnail
            : DockWindowTransitionSurface::None);
}

/**
 * @brief 使用静态窗口快照在应用窗口与 Dock 图标之间播放过渡。
 *
 * 最小化前仅捕获一次窗口帧，恢复时优先复用缓存帧，动画期间由 GPU
 * 缩放静态位图，避免持续重采样实时 DWM 表面。没有可用快照时仍保留
 * DWM 缩略图回退。该窗口不抢焦点且鼠标穿透。
 */
class DockWindowTransition
{
public:
    using RestoreCallback = std::function<void(HWND)>;

    DockWindowTransition() = default;
    ~DockWindowTransition();

    DockWindowTransition(const DockWindowTransition&) = delete;
    DockWindowTransition& operator=(const DockWindowTransition&) = delete;

    bool Initialize(HINSTANCE instance);
    bool PrimeMinimizeSnapshot(HWND sourceWindow);
    bool StartMinimize(HWND sourceWindow, RECT dockRect);
    bool StartRestore(
        HWND sourceWindow, RECT dockRect,
        RestoreCallback restoreCallback);
    void Cancel();
    bool IsActive() const;
    bool IsActiveFor(HWND window) const;
    DockWindowTransitionDirection GetDirection() const;

private:
    static constexpr UINT_PTR kAnimationTimerId = 1;
    static constexpr UINT kAnimationTimerIntervalMs = 8;
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
        RestoreCallback restoreCallback);
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
    bool EnsureSnapshotRenderer();
    bool CreateActiveSnapshotBitmap(
        const CachedSnapshot& snapshot);
    bool DrawSnapshotFrame(
        const RECT& destinationRect);
    bool ApplyFrame(double progress);
    void OnTimer();
    void Finish();
    void CompleteRestoreAfterRenderFailure();
    void SetNativeTransitionsDisabled(bool disabled);
    void UnregisterThumbnail();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND sourceWindow_ = nullptr;
    HTHUMBNAIL thumbnail_ = nullptr;
    DockWindowTransitionSurface surface_ =
        DockWindowTransitionSurface::None;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget>
        snapshotRenderTarget_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap>
        activeSnapshotBitmap_;
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
