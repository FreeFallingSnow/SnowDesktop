#pragma once

#include "dock_settings.h"
#include "dock_window_pin.h"
#include "dock_window_peek.h"

#include <dwmapi.h>
#include <windows.h>

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct DockWindowPreviewItem
{
    HWND window = nullptr;
    std::wstring title;
};

struct DockWindowPreviewGrid
{
    int columns = 0;
    int rows = 0;
    int cardWidth = 0;
    int cardHeight = 0;
    int panelWidth = 0;
    int panelHeight = 0;
};

struct DockWindowPreviewZOrderPolicy
{
    HWND insertAfter = nullptr;
    UINT flags = 0;
};

DockWindowPreviewZOrderPolicy
ResolveDockWindowPreviewZOrderPolicy(
    bool useDockLayer, bool wasVisible);

DockWindowPreviewGrid CalculateDockWindowPreviewGrid(
    size_t itemCount, int maximumWidth, int maximumHeight, UINT dpi);
std::vector<RECT> CalculateDockWindowPreviewCardRects(
    size_t itemCount, const DockWindowPreviewGrid& grid, UINT dpi);
RECT CalculateDockWindowPreviewCloseButtonRect(
    const RECT& cardRect, UINT dpi);
RECT CalculateDockWindowPreviewPinButtonRect(
    const RECT& cardRect, UINT dpi);
bool IsPointInDockWindowPreviewCloseButton(
    POINT point, const RECT& cardRect, UINT dpi);
bool IsPointInDockPreviewTransitionRegion(
    POINT screenPoint, POINT transitionOriginScreen,
    const RECT& anchorScreen,
    const RECT& previewScreen, DockPosition dockPosition,
    int tolerance);

struct DockPreviewHoverTransition
{
    bool armTimer = false;
    bool cancelTimer = false;
    bool keepPreviewVisible = false;
    bool schedulePreviewHide = false;
};

/**
 * @brief 管理 Dock 预览的延迟悬停与点击后锁定状态。
 */
class DockPreviewHoverController
{
public:
    DockPreviewHoverTransition UpdateTarget(
        const std::wstring& targetToken,
        bool previewVisible,
        bool previewMatchesTarget);
    bool ConsumeTimer(const std::wstring& observedTargetToken);
    bool SuppressForActivation();
    void MarkPreviewShown(const std::wstring& targetToken);
    void Reset();

    const std::wstring& CurrentTarget() const { return currentTarget_; }
    const std::wstring& SuppressedTarget() const { return suppressedTarget_; }
    bool TimerArmed() const { return timerArmed_; }
    bool IsIdle() const
    {
        return currentTarget_.empty() &&
            pendingTarget_.empty() &&
            shownTarget_.empty() &&
            suppressedTarget_.empty() &&
            !timerArmed_;
    }

private:
    std::wstring currentTarget_;
    std::wstring pendingTarget_;
    std::wstring shownTarget_;
    std::wstring suppressedTarget_;
    bool timerArmed_ = false;
};

/**
 * @brief 以不抢焦点的独立弹窗显示一组实时 DWM 窗口缩略图。
 */
class DockWindowPreview
{
public:
    using ActivateCallback = std::function<void(HWND)>;
    using CloseCallback = std::function<void(HWND)>;

    explicit DockWindowPreview(snowdesktop::dock_peek::Api peekApi =
        snowdesktop::dock_peek::Api::Load()) : windowPeek_(peekApi) {}
    ~DockWindowPreview();

    DockWindowPreview(const DockWindowPreview&) = delete;
    DockWindowPreview& operator=(const DockWindowPreview&) = delete;

    bool Initialize(
        HINSTANCE instance,
        ActivateCallback activateCallback,
        CloseCallback closeCallback, std::function<void()> visibilityChanged = {});
    void Show(const std::vector<DockWindowPreviewItem>& items,
        RECT anchorScreen, DockPosition dockPosition, bool lightTheme,
        HWND dockLayerOwner = nullptr);
    void UpdateAnchor(RECT anchorScreen, DockPosition dockPosition);
    void SetPinBorderAppearance(const PersonalizationSettings& settings) { windowPins_.SetAppearance(settings); }
    void SetPeekActivateCallback(ActivateCallback callback)
    {
        if (!callback) { peekActivateCallback_ = {}; return; }
        peekActivateCallback_ = [this, callback = std::move(callback)](HWND window) {
            windowPeek_.PrepareActivation();
            callback(window);
        };
    }
    void Hide();
    void RemoveWindow(HWND window);
    void UpdateItems(const std::vector<DockWindowPreviewItem>& items);
    // One-shot input provenance survives card reflow and final-card Hide.
    HMONITOR TakePointerInteractionMonitor() { return std::exchange(pointerInteractionMonitor_, nullptr); }
    void ObserveForwardedPointer(UINT message, DWORD time);
    bool RetainsCloseForeground(HWND foreground);
    void ReleaseCloseRetention() { closeRetentionUntil_ = 0; closeReflowPointerKnown_ = false; }
    void ScheduleHide();
    void KeepVisible();
    bool IsVisible() const;
    bool IsCleared() const;
    bool IsShowingWindow(HWND window) const;
    bool ContainsInteractionPoint(POINT screenPoint) const;
    HWND GetWindow() const { return hwnd_; }
    const std::vector<HWND>& GetWindows() const { return windows_; }
    RECT GetBounds() const;

private:
    static constexpr UINT_PTR kHideTimerId = 1;
    static constexpr UINT_PTR kPeekTimerId = 2;
    static constexpr UINT kPeekTimerIntervalMs = 50;
    static constexpr UINT kTransitionHideDelayMs = 120;
    static constexpr UINT kPointerReconcileMessage = WM_APP + 1;

    static LRESULT CALLBACK WindowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureWindow();
    bool EnsureWindows(size_t count);
    void Layout(RECT monitorWorkArea, UINT dpi);
    void RegisterThumbnails();
    void UnregisterThumbnails();
    void Paint(HWND window);
    void InvalidatePopups();
    POINT ToLayoutPoint(HWND window, POINT point) const;
    void OnMouseMove(HWND window, POINT point);
    void OnMouseLeave(HWND window);
    void OnLeftButtonUp(POINT point);
    void OnMiddleButtonUp(POINT point);
    void CloseWindowAtIndex(int index);
    void RecordPointerInteraction(UINT message);
    void RefreshPointerTracking(bool layoutChanged = false);
    HWND PointerWindowAt(POINT screenPoint) const;
    void ArmPointerTracking(HWND window, POINT screenPoint);
    void UpdatePeekTarget(HWND target);
    void OnPeekTimer();
    void CancelPeek(bool suppressUntilLeave = false);
    void HideIfPointerOutside();
    bool IsPointerInTransitionRegion(POINT screenPoint) const;
    bool RetainsReflowPointer(POINT screenPoint) const;
    int CardIndexAtPoint(POINT point) const;
    int CloseButtonIndexAtPoint(POINT point) const;
    int PinButtonIndexAtPoint(POINT point) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    std::vector<HWND> windows_;
    ActivateCallback activateCallback_;
    ActivateCallback peekActivateCallback_;
    CloseCallback closeCallback_;
    DockWindowPin windowPins_;
    snowdesktop::dock_peek::Session windowPeek_;
    snowdesktop::dock_peek::WindowIdentity peekCandidate_;
    HWND peekOwner_ = nullptr;
    HWND peekTimerWindow_ = nullptr;
    HWND peekSuppressedWindow_ = nullptr;
    HWND peekPendingForeground_ = nullptr;
    ULONGLONG peekDue_ = 0;
    ULONGLONG peekLeaveDue_ = 0;
    bool peekSurfacesReady_ = false;
    bool peekCommitting_ = false;
    ULONGLONG peekCommitDeadline_ = 0;
    std::function<void()> visibilityChanged_;
    std::vector<DockWindowPreviewItem> items_;
    std::vector<RECT> cardRects_;
    std::vector<RECT> thumbnailRects_;
    std::vector<HTHUMBNAIL> thumbnails_;
    std::vector<HTHUMBNAIL> measuredThumbnails_;
    SIZE panelSize_{};
    RECT anchorScreen_{};
    HMONITOR pointerInteractionMonitor_ = nullptr;
    struct PointerReceipt
    {
        UINT message = 0;
        DWORD time = 0;
        HMONITOR monitor = nullptr;
        ULONGLONG recordedAt = 0;
    };
    std::array<PointerReceipt, 8> pointerReceipts_{};
    size_t nextPointerReceipt_ = 0;
    snowdesktop::dock_peek::WindowIdentity closingWindow_;
    HWND closeForeground_ = nullptr;
    HWND closeSuccessor_ = nullptr;
    ULONGLONG closeRetentionUntil_ = 0;
    POINT closeReflowPointer_{};
    bool closeReflowPointerKnown_ = false;
    DockPosition dockPosition_ = DockPosition::Bottom;
    bool lightTheme_ = false;
    bool trackingMouse_ = false;
    HWND trackingWindow_ = nullptr;
    HWND retainedLeaveWindow_ = nullptr;
    POINT retainedLeavePoint_{};
    bool retainedLeaveRearmed_ = false;
    bool pointerReconcilePending_ = false;
    int hoveredIndex_ = -1;
    int hoveredCloseIndex_ = -1;
    int hoveredPinIndex_ = -1;
    UINT dpi_ = 96;
    POINT transitionOriginScreen_{};
    bool hasTransitionOrigin_ = false;
    bool hideTimerArmed_ = false;
};
