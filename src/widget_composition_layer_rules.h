#pragma once

namespace snowdesktop::widget_composition_layer_rules
{
enum class DesktopLayer
{
    Background,
    Widget,
    Foreground,
    AnimationOverlay,
};

constexpr int ZOrder(DesktopLayer layer)
{
    return static_cast<int>(layer);
}

constexpr bool IsAbove(DesktopLayer candidate, DesktopLayer reference)
{
    return ZOrder(candidate) > ZOrder(reference);
}

constexpr bool ShouldPresentWidgetSurface(
    bool desktopSurfaceVisible,
    bool previewSourceHidden)
{
    return desktopSurfaceVisible && !previewSourceHidden;
}
}
