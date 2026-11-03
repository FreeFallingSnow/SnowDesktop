#pragma once

#include "app/app.h"
#include "desktop/desktop_hover_types.h"
#include "dock/floating_dock_rules.h"
#include "navigation/quick_navigation_animation_rules.h"
#include "system/status_bar/status_bar.h"
#include "ui/render/inline_tooltip_backdrop.h"
#include "ui/render/ui_animation_scheduler.h"
#include "app/render/desktop_backdrop_compositor.h"

// Each monitor owns its persistent window, input promotion and composition
// resources. The combination root only stores owners/references to these hosts.
struct DesktopApp::PersistentDockHost
{
    DesktopApp* owner = nullptr;
    DockContainer* container = nullptr;
    HMONITOR monitor = nullptr;
    HWND hwnd = nullptr;
    RECT sourceRect{};
    RECT dockRect{};
    RECT animationVisualRect{};
    RECT popupRect{};
    RECT tooltipRect{};
    snowdesktop::InlineTooltipPaintDamage tooltipPaintDamage;
    RECT guideOcclusionRect{};
    bool active = false;
    snowdesktop::desktop_hover_rules::RetainedDockLeaveTracking retainedLeaveTracking;
    // Promotion is per monitor. Selection only identifies the Host that
    // currently owns pointer/keyboard-associated actions.
    bool promoted = false;
    bool explicitDismissAvailable = false;
    RECT explicitDismissRect{};
    bool promotionInputVisible = false;
    bool promotionInputBlocked = false;
    HWND promotionInputForeground = nullptr;
    DWORD promotionInputProcess = 0;
    std::optional<bool> promotionInputCapabilityMissing;
    bool promotionInputBorrowedFocus = false;
    ULONGLONG promotionInputFocusPendingUntil = 0;
    std::optional<DWORD> promotionInputRequestInputTime;
    bool promotionInputPointerRetryUsed = false;
    snowdesktop::floating_dock_rules::WindowActionPromotion windowActionPromotion;
    // Passive drag-edge reveal is intentionally separate from manual
    // promotion. It may borrow the floating Z band without starting a
    // keyboard session, and its leave timer must never collapse a Host
    // that the user summoned explicitly.
    bool passivelyRevealed = false;
    bool edgeHoverRequested = false;
    ULONGLONG passiveRevealTick = 0;
    ULONGLONG passiveLeaveStartTick = 0;
    bool revealPending = false;
    bool frameReady = false;
    snowdesktop::quick_navigation_animation_rules::State mergedAnimation;
    snowdesktop::UiScheduleToken mergedAnimationToken = 0;
    snowdesktop::StatusBarDockPresentation mergedPresentation;
    // Shared visibility timeline for merged bars and summon-only Docks.
    bool mergedPresentationActive = false, updatingMergedPresentation = false;
    bool presentationMerged = false;
    bool mergedInteractionHeld = false, mergedCloseAfterInteraction = false;
    ComPtr<IDCompositionEffectGroup> mergedOpacity;
    bool compositionRenderRecoveryPending = false;
    bool compositionPaintInProgress = false;
    bool immediatePaintDeferred = false;
    bool windowRegionPending = false;
    bool backdropRecoveryPending = false;
    bool desktopLayerRestorePending = false;
    ULONGLONG desktopLayerRestoreRetryTick = 0;
    HWND desktopLayerForegroundWindow = nullptr;
    std::uint64_t presentationRevision = 0;
    bool dropTargetRegistered = false;
    DesktopBackdropCompositor backdrop;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual2> dcompVisual;
    ComPtr<IDCompositionSurface> dcompSurface;
    UINT compWidth = 0;
    UINT compHeight = 0;
};
