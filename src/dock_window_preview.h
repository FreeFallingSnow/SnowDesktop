#pragma once

#include "dock_settings.h"

#include <dwmapi.h>
#include <windows.h>

#include <cstddef>
#include <functional>
#include <string>
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
 * @brief 以不抢焦点的置顶弹窗显示一组实时 DWM 窗口缩略图。
 */
class DockWindowPreview
{
public:
    using ActivateCallback = std::function<void(HWND)>;
    using CloseCallback = std::function<void(HWND)>;

    DockWindowPreview() = default;
    ~DockWindowPreview();

    DockWindowPreview(const DockWindowPreview&) = delete;
    DockWindowPreview& operator=(const DockWindowPreview&) = delete;

    bool Initialize(
        HINSTANCE instance,
        ActivateCallback activateCallback,
        CloseCallback closeCallback);
    void Show(const std::vector<DockWindowPreviewItem>& items,
        RECT anchorScreen, DockPosition dockPosition, bool lightTheme,
        HWND dockLayerOwner = nullptr);
    void UpdateAnchor(RECT anchorScreen, DockPosition dockPosition);
    void Hide();
    void ScheduleHide();
    void KeepVisible();
    bool IsVisible() const;
    bool IsCleared() const;
    bool IsShowingWindow(HWND window) const;
    bool ContainsInteractionPoint(POINT screenPoint) const;
    HWND GetWindow() const { return hwnd_; }

private:
    static constexpr UINT_PTR kHideTimerId = 1;
    static constexpr UINT kTransitionHideDelayMs = 120;

    static LRESULT CALLBACK WindowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureWindow();
    void Layout(RECT monitorWorkArea, UINT dpi);
    void RegisterThumbnails();
    void UnregisterThumbnails();
    void Paint();
    void OnMouseMove(POINT point);
    void OnMouseLeave();
    void OnLeftButtonUp(POINT point);
    void HideIfPointerOutside();
    bool IsPointerInTransitionRegion(POINT screenPoint) const;
    int CardIndexAtPoint(POINT point) const;
    int CloseButtonIndexAtPoint(POINT point) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ActivateCallback activateCallback_;
    CloseCallback closeCallback_;
    std::vector<DockWindowPreviewItem> items_;
    std::vector<RECT> cardRects_;
    std::vector<RECT> thumbnailRects_;
    std::vector<HTHUMBNAIL> thumbnails_;
    SIZE panelSize_{};
    RECT anchorScreen_{};
    DockPosition dockPosition_ = DockPosition::Bottom;
    bool lightTheme_ = false;
    bool trackingMouse_ = false;
    int hoveredIndex_ = -1;
    int hoveredCloseIndex_ = -1;
    UINT dpi_ = 96;
    POINT transitionOriginScreen_{};
    bool hasTransitionOrigin_ = false;
    bool hideTimerArmed_ = false;
};
