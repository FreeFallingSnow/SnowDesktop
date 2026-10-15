#pragma once

#include <windows.h>
#include <oleidl.h>

namespace snowdesktop::ole_drag_rules
{
inline DWORD ChooseDropEffect(DWORD keyState, DWORD allowed) noexcept
{
    const DWORD available = allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    DWORD requested = DROPEFFECT_NONE;
    if (keyState & MK_ALT) requested = DROPEFFECT_LINK;
    else if (keyState & MK_SHIFT) requested = DROPEFFECT_MOVE;
    else if (keyState & MK_CONTROL) requested = DROPEFFECT_COPY;
    if (requested != DROPEFFECT_NONE) return requested & available;
    if (available & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    if (available & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    return available & DROPEFFECT_LINK;
}

enum class DwellTargetRefreshRoute
{
    NativePointer,
    SelfOleDragOver,
    ExternalLocalFeedback,
};

enum class QueryContinueDragAction
{
    ContinueOle,
    Drop,
    Cancel,
    ResumeNative,
};

enum class SelfOleUnwindAction
{
    FinishOle,
    RestartOle,
    ResumeNativeHeld,
    ReleaseNative,
};

constexpr bool IsExternalDropSurface(
    bool hasHitWindow,
    bool isDesktopInteractionSurface,
    bool rootWindowVisible) noexcept
{
    return hasHitWindow &&
        !isDesktopInteractionSurface &&
        rootWindowVisible;
}

constexpr DwellTargetRefreshRoute SelectDwellTargetRefreshRoute(
    bool selfOleDragActive,
    bool externalOleDragActive) noexcept
{
    if (selfOleDragActive)
        return DwellTargetRefreshRoute::SelfOleDragOver;
    if (externalOleDragActive)
        return DwellTargetRefreshRoute::ExternalLocalFeedback;
    return DwellTargetRefreshRoute::NativePointer;
}

constexpr QueryContinueDragAction SelectQueryContinueDragAction(
    bool escapePressed,
    bool primaryButtonDown,
    bool selfOleDragActive,
    bool selfDragReturned,
    bool pointerOnDesktopSurface) noexcept
{
    if (escapePressed)
        return QueryContinueDragAction::Cancel;
    if (selfOleDragActive &&
        selfDragReturned && pointerOnDesktopSurface)
    {
        return QueryContinueDragAction::ResumeNative;
    }
    if (!primaryButtonDown)
        return QueryContinueDragAction::Drop;
    return QueryContinueDragAction::ContinueOle;
}

constexpr SelfOleUnwindAction SelectSelfOleUnwindAction(
    bool nativeResumeRequested,
    bool pointerOnDesktopSurface,
    bool primaryButtonDown,
    bool gestureActive) noexcept
{
    if (!nativeResumeRequested || !gestureActive)
        return SelfOleUnwindAction::FinishOle;
    if (!pointerOnDesktopSurface)
    {
        return primaryButtonDown
            ? SelfOleUnwindAction::RestartOle
            : SelfOleUnwindAction::FinishOle;
    }
    return primaryButtonDown
        ? SelfOleUnwindAction::ResumeNativeHeld
        : SelfOleUnwindAction::ReleaseNative;
}

} // namespace snowdesktop::ole_drag_rules
