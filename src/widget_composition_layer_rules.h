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

enum class PointerVisualLayer
{
    None,
    Background,
    Widget,
    Foreground,
};

constexpr int ZOrder(DesktopLayer layer)
{
    return static_cast<int>(layer);
}

constexpr bool IsAbove(DesktopLayer candidate, DesktopLayer reference)
{
    return ZOrder(candidate) > ZOrder(reference);
}

// DirectComposition reverses the intuitive meaning of insertAbove when no
// reference visual is supplied: FALSE inserts above every existing sibling.
constexpr bool NullReferenceInsertsAboveAll(bool insertAbove)
{
    return !insertAbove;
}

constexpr bool ShouldPresentWidgetSurface(
    bool desktopSurfaceVisible,
    bool previewSourceHidden)
{
    return desktopSurfaceVisible && !previewSourceHidden;
}

constexpr bool NeedsWidgetSurfaceRefresh(PointerVisualLayer layer)
{
    return layer == PointerVisualLayer::Widget;
}

constexpr bool NeedsDesktopPaint(PointerVisualLayer layer)
{
    return layer == PointerVisualLayer::Background ||
        layer == PointerVisualLayer::Foreground;
}

constexpr bool NeedsBackgroundPaint(PointerVisualLayer layer)
{
    return layer == PointerVisualLayer::Background;
}

constexpr bool NeedsForegroundPaint(PointerVisualLayer layer)
{
    return layer == PointerVisualLayer::Foreground;
}
}
