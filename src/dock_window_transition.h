#pragma once

#include <dwmapi.h>
#include <windows.h>

#include <functional>
#include <unordered_map>

enum class DockWindowTransitionDirection
{
    Minimize,
    Restore,
};

double EaseDockWindowTransition(double progress) noexcept;
RECT InterpolateDockWindowTransitionRect(
    const RECT& from, const RECT& to, double progress) noexcept;

/**
 * @brief 使用 DWM 实时缩略图在应用窗口与 Dock 图标之间播放过渡。
 *
 * 该窗口不抢焦点且鼠标穿透。若 DWM 缩略图或禁用目标窗口原生过渡失败，
 * StartMinimize/StartRestore 会返回 false，由调用方继续使用系统动画。
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
    bool StartMinimize(HWND sourceWindow, RECT dockRect);
    bool StartRestore(
        HWND sourceWindow, RECT dockRect,
        RestoreCallback restoreCallback);
    void Cancel();
    bool IsActive() const;

private:
    static constexpr UINT_PTR kAnimationTimerId = 1;
    static constexpr UINT kAnimationTimerIntervalMs = 16;
    static constexpr ULONGLONG kAnimationDurationMs = 240;
    static constexpr ULONGLONG kRestoreCleanupTimeoutMs = 240;

    static LRESULT CALLBACK WindowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureWindow();
    bool Start(
        HWND sourceWindow, RECT dockRect,
        DockWindowTransitionDirection direction,
        RestoreCallback restoreCallback);
    bool ResolveVisibleWindowRect(HWND window, RECT& rect) const;
    bool ResolveRestoreWindowRect(HWND window, RECT& rect) const;
    bool ApplyFrame(double progress);
    void OnTimer();
    void Finish();
    void SetNativeTransitionsDisabled(bool disabled);
    void UnregisterThumbnail();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND sourceWindow_ = nullptr;
    HTHUMBNAIL thumbnail_ = nullptr;
    DockWindowTransitionDirection direction_ =
        DockWindowTransitionDirection::Minimize;
    RECT fromRect_{};
    RECT toRect_{};
    RECT lastFrameRect_{};
    BYTE lastFrameOpacity_ = 0;
    bool hasLastFrame_ = false;
    ULONGLONG animationStartTick_ = 0;
    ULONGLONG restoreCleanupDeadline_ = 0;
    bool awaitingRestoreVisibility_ = false;
    bool nativeTransitionsDisabled_ = false;
    RestoreCallback restoreCallback_;
    std::unordered_map<HWND, RECT> lastVisibleRects_;
};
