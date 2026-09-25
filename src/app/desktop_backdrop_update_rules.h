#pragma once

#include <windows.h>

#include <cstdint>
#include <algorithm>

namespace snowdesktop::desktop_backdrop_update_rules
{

// A retained widget can be painted before its backdrop target exists, or
// outlive a target reset. Reconcile every visible request even when that
// widget is outside the current paint rectangle and its surface is reused.
template<class Compositor, class Widget>
bool KeepOrRestoreWidgetPanel(Compositor& compositor, const Widget& widget)
{
    if (!widget.visible || !widget.backdropRequested)
        return false;
    if (compositor.KeepPanel(widget.bounds))
        return true;
    return compositor.AddPanel(widget.bounds,
        static_cast<float>(widget.backdropCornerRadius),
        static_cast<float>(widget.backdropBlurRadius));
}

// Call after the final panel collection, so a remove/add pair in one frame
// can reuse its factory. Brushes and backdrop sources remain panel-owned.
template<class Factories, class Panels>
void PruneUnusedBlurFactories(Factories& factories, const Panels& panels)
{
    for (auto factory = factories.begin(); factory != factories.end();)
    {
        const bool used = std::any_of(panels.begin(), panels.end(),
            [&](const auto& panel) { return panel.blurRadius == factory->first; });
        if (used)
            ++factory;
        else
            factory = factories.erase(factory);
    }
}

inline bool PanelIdentityMatches(
    std::uintptr_t existingOwnerKey,
    const RECT& existingFrame,
    std::uintptr_t requestedOwnerKey,
    const RECT& requestedFrame) noexcept
{
    if (existingOwnerKey != 0 || requestedOwnerKey != 0)
    {
        return existingOwnerKey != 0 &&
            existingOwnerKey == requestedOwnerKey;
    }
    return EqualRect(&existingFrame, &requestedFrame) != FALSE;
}

inline bool CoversClientArea(
    const RECT* updateRect,
    const RECT& clientRect) noexcept
{
    if (!updateRect)
        return true;

    return updateRect->left <= clientRect.left &&
        updateRect->top <= clientRect.top &&
        updateRect->right >= clientRect.right &&
        updateRect->bottom >= clientRect.bottom;
}

inline bool ShouldCollectAllPanels(
    bool forceCompleteCollection,
    bool dragSessionActive,
    bool widgetPreviewActive,
    bool desktopMarqueeActive,
    const RECT* updateRect,
    const RECT& clientRect) noexcept
{
    return forceCompleteCollection ||
        (!dragSessionActive &&
        !widgetPreviewActive &&
        !desktopMarqueeActive &&
        CoversClientArea(updateRect, clientRect));
}

} // namespace snowdesktop::desktop_backdrop_update_rules
