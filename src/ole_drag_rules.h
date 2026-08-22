#pragma once

namespace snowdesktop::ole_drag_rules
{

enum class DwellTargetRefreshRoute
{
    NativePointer,
    SelfOleDragOver,
    AwaitExternalOleCallback,
};

enum class QueryContinueDragAction
{
    ContinueOle,
    Drop,
    Cancel,
    ResumeNative,
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
        return DwellTargetRefreshRoute::AwaitExternalOleCallback;
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

} // namespace snowdesktop::ole_drag_rules
