#pragma once

#include "navigation/quick_navigation_animation_rules.h"
#include "dock_layout_settings.h"
#include <windows.h>

namespace snowdesktop
{
// Small fused bars spend at most one eighth of their height on each inset.
// Keep the normal Dock spacing along the bar independent of this padding.
inline int MergedDockVerticalPadding(int height, int normalSpacing)
{
    return std::clamp(height / 8, 1, std::max(1, normalSpacing));
}

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
    float hiddenFraction = 1;
    float offsetX = 0, offsetY = 0;
    float hiddenOffsetX = 0, hiddenOffsetY = 0;
    bool compositorSlide = false;
};

inline StatusBarDockPresentation MergedDockPresentationFrame(
    const quick_navigation_animation_rules::State& animation)
{
    const auto visual = animation.GetVisual();
    StatusBarDockPresentation frame;
    frame.visible = visual.visible;
    frame.inputEnabled = animation.IsInteractive() && !animation.IsAnimating();
    frame.opacity = visual.visible ? 1.f : 0.f;
    frame.hiddenFraction = 1.f - visual.opacity;
    return frame;
}

inline void PositionDockRevealFrame(StatusBarDockPresentation& frame,
    DockPosition edge, const RECT& bounds, const RECT& screen)
{
    frame.hiddenOffsetX = frame.hiddenOffsetY = 0;
    switch (edge)
    {
    case DockPosition::Left: frame.hiddenOffsetX = -static_cast<float>(std::max<LONG>(0, bounds.right - screen.left)); break;
    case DockPosition::Right: frame.hiddenOffsetX = static_cast<float>(std::max<LONG>(0, screen.right - bounds.left)); break;
    case DockPosition::Top: frame.hiddenOffsetY = -static_cast<float>(std::max<LONG>(0, bounds.bottom - screen.top)); break;
    case DockPosition::Bottom: frame.hiddenOffsetY = static_cast<float>(std::max<LONG>(0, screen.bottom - bounds.top)); break;
    }
    frame.offsetX = frame.hiddenOffsetX * frame.hiddenFraction;
    frame.offsetY = frame.hiddenOffsetY * frame.hiddenFraction;
}
}
