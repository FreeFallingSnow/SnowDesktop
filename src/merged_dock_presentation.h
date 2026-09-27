#pragma once

#include "quick_navigation_animation_rules.h"
#include <windows.h>

namespace snowdesktop
{
inline bool ReserveStatusBarSpace(bool merged, bool summonOnly)
{
    return !merged || !summonOnly;
}

// One Dock-owned frame drives both native surfaces. No layout or visibility
// policy may be inferred from this intermediate animation frame.
struct StatusBarDockPresentation
{
    bool visible = false, inputEnabled = false, topmost = false;
    HWND insertAfter = HWND_NOTOPMOST;
    float opacity = 0;
};

inline StatusBarDockPresentation MergedDockPresentationFrame(
    const quick_navigation_animation_rules::State& animation)
{
    const auto visual = animation.GetVisual();
    StatusBarDockPresentation frame;
    frame.visible = visual.visible;
    frame.inputEnabled = animation.IsInteractive() && !animation.IsAnimating();
    frame.opacity = visual.opacity;
    return frame;
}
}
