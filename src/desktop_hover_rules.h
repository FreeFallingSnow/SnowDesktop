#pragma once

#include <cstdint>

namespace snowdesktop::desktop_hover_rules
{
inline constexpr std::uint32_t kActivationSettleMs = 150;

enum class ReconcileMode
{
    DeactivateOnly,
    AllowImmediateActivation,
    AllowActivationAfterForegroundSettle,
};

constexpr ReconcileMode ShellPopupCloseReconcileMode()
{
    // A Shell verb may return before its asynchronous confirmation dialog is
    // created. The menu's final cursor position therefore cannot prove that
    // the pointer has returned to the desktop interaction surface.
    return ReconcileMode::DeactivateOnly;
}

constexpr bool HasForegroundSettled(
    bool foregroundChangeKnown,
    std::uint32_t elapsedMs)
{
    return !foregroundChangeKnown ||
        elapsedMs >= kActivationSettleMs;
}

constexpr bool ShouldReconcileFromSurfaceSample(
    bool nativeShellPopupLayerActive,
    bool shellDialogOwnerAvailable,
    bool shellDialogOwnerEnabled)
{
    const bool nativeShellModalSessionActive =
        nativeShellPopupLayerActive ||
        (shellDialogOwnerAvailable && !shellDialogOwnerEnabled);
    return !nativeShellModalSessionActive;
}

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
    ReconcileMode mode,
    bool foregroundSettled)
{
    const bool activationAllowed =
        mode == ReconcileMode::AllowImmediateActivation ||
        (mode == ReconcileMode::AllowActivationAfterForegroundSettle &&
            foregroundSettled);
    return pointerOnDesktopSurface &&
        passiveHoverCleared &&
        activationAllowed;
}
}
