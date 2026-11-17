#pragma once

#include <windows.h>
#include <string>

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

struct DockPreviewHoverTransition
{
    bool armTimer = false;
    bool cancelTimer = false;
    bool keepPreviewVisible = false;
};

class DockPreviewHoverController
{
public:
    DockPreviewHoverTransition UpdateTarget(
        const std::wstring& targetToken,
        bool previewVisible,
        bool previewMatchesTarget);
    void TrackTimer(ULONGLONG dueTick);
    UINT TimerDelayRemaining(ULONGLONG now) const;
    bool ConsumeTimer(const std::wstring& observedTargetToken);
    bool SuppressForActivation();
    void MarkPreviewShown(const std::wstring& targetToken);
    void Reset();

    const std::wstring& CurrentTarget() const;
    const std::wstring& SuppressedTarget() const;
    bool TimerArmed() const;
    bool IsIdle() const;

private:
    std::wstring currentTarget_;
    std::wstring shownTarget_;
    std::wstring suppressedTarget_;
    ULONGLONG timerDueTick_ = 0;
};
