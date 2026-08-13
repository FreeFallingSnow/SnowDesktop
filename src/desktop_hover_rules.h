#pragma once

namespace snowdesktop::desktop_hover_rules
{
enum class ReconcileMode
{
    DeactivateOnly,
    AllowActivation,
};

template<typename Handle>
constexpr bool OwnsInteractionCapture(
    Handle captureWindow,
    Handle desktopWindow,
    Handle floatingDockWindow)
{
    return captureWindow != Handle{} &&
        (captureWindow == desktopWindow ||
            captureWindow == floatingDockWindow);
}

constexpr bool CanClearPassiveHover(
    bool ownsInteractionCapture,
    bool mouseDown,
    bool dragActive,
    bool widgetInteractionActive)
{
    return !ownsInteractionCapture &&
        !mouseDown &&
        !dragActive &&
        !widgetInteractionActive;
}

constexpr bool ShouldPresentSynchronously(
    bool hoverTargetChanged,
    bool continuousPointerSurface)
{
    return hoverTargetChanged ||
        continuousPointerSurface;
}

constexpr bool ShouldActivateFromSurfaceSample(
    bool pointerOnDesktopSurface,
    bool passiveHoverCleared,
    ReconcileMode mode)
{
    return pointerOnDesktopSurface &&
        passiveHoverCleared &&
        mode == ReconcileMode::AllowActivation;
}
}
