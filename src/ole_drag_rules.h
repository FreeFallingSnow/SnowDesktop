#pragma once

namespace snowdesktop::ole_drag_rules
{

enum class DwellTargetRefreshRoute
{
    NativePointer,
    SelfOleDragOver,
    AwaitExternalOleCallback,
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

} // namespace snowdesktop::ole_drag_rules
