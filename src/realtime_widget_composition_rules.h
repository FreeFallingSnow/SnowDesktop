#pragma once

namespace snowdesktop::realtime_widget_composition_rules
{
struct SceneState
{
    bool compositionActive = false;
    bool desktopSurfaceVisible = false;
    bool dragActive = false;
    bool externalDragActive = false;
    bool widgetPreviewActive = false;
    bool desktopMarqueeActive = false;
    bool popupActive = false;
    bool widgetPanelActive = false;
};

inline bool ShouldUseIndependentSurface(const SceneState& state)
{
    return state.compositionActive &&
        state.desktopSurfaceVisible &&
        !state.dragActive &&
        !state.externalDragActive &&
        !state.widgetPreviewActive &&
        !state.desktopMarqueeActive &&
        !state.popupActive &&
        !state.widgetPanelActive;
}
}
