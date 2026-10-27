#pragma once

#include "dock/floating_dock_rules.h"
#include <optional>

namespace snowdesktop
{
inline bool ReserveIndependentStatusBarSpace(bool merged, bool requested, bool summonOnly)
{
    return requested && (merged || !summonOnly);
}

inline bool HideIndependentStatusBar(bool fullscreenHidden, bool summonOnly, bool revealed, bool interacting)
{
    return fullscreenHidden || (summonOnly && !revealed && !interacting);
}

// One lifetime per monitor. Native polling supplies geometry and interaction
// holds; gesture thresholds and edge latching stay shared with Dock.
class StatusBarRevealState
{
public:
    bool Update(POINT pointer, RECT monitor, RECT bar, DockPosition position,
        DWORD tick, UINT dpi, int gesture, bool interacting)
    {
        const bool before = revealed_;
        if (!revealed_)
        {
            if (edge_.Update(pointer, monitor, position, tick,
                floating_dock_rules::ScaleEdgeSwipeDip(floating_dock_rules::kEdgeSwipeBandDip, dpi),
                floating_dock_rules::ScaleEdgeSwipeDip(floating_dock_rules::kEdgeSwipeTravelDip, dpi),
                floating_dock_rules::kEdgeSwipeMaximumDurationMs, gesture == 1)) Reveal();
        }
        if (revealed_)
        {
            if (interacting || PtInRect(&bar, pointer)) leaveTick_.reset();
            else if (!leaveTick_) leaveTick_ = tick;
            else if (tick - *leaveTick_ >= floating_dock_rules::kPassiveDragLeaveDelayMs) Dismiss();
        }
        return before != revealed_;
    }
    bool Revealed() const { return revealed_; }
    void Reveal() { revealed_ = true; leaveTick_.reset(); }
    void Dismiss() { revealed_ = false; leaveTick_.reset(); edge_.SuppressUntilEdgeLeave(); }
    void Reset() { revealed_ = false; leaveTick_.reset(); edge_.Reset(); }
private:
    floating_dock_rules::EdgeSwipeDetector edge_;
    bool revealed_ = false;
    std::optional<DWORD> leaveTick_;
};
}
