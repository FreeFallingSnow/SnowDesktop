#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace snowdesktop::widget_composition_layer_rules
{
// The maximum four-pixel dimensional border includes a 1.35 px antialiased
// outer pass. Reserve the centered half-width on each side so compact
// DirectComposition surfaces never clip the configured overdraw.
inline constexpr long kWidgetSurfaceBorderOverdraw = 3;

constexpr long WidgetSurfaceOrigin(long logicalOrigin)
{
    return logicalOrigin - kWidgetSurfaceBorderOverdraw;
}

constexpr long WidgetSurfaceExtent(long logicalExtent)
{
    return logicalExtent + kWidgetSurfaceBorderOverdraw * 2;
}

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

enum class CompositionHost
{
    Desktop,
    FloatingPopup,
};

struct WidgetDragFeedbackState
{
    bool active = false;
    bool resize = false;
    std::wstring pageId;
    int column = 0;
    int row = 0;
    int columns = 1;
    int rows = 1;
    bool dockTarget = false;
    std::uintptr_t dockOwner = 0;
    std::size_t dockInsertIndex = 0;
    std::size_t groupTargetIndex = static_cast<std::size_t>(-1);
    std::size_t groupInsertIndex = static_cast<std::size_t>(-1);
    int navigationSide = 0;

    bool operator==(const WidgetDragFeedbackState&) const = default;
};

inline bool NeedsWidgetDragFeedbackPresent(
    const WidgetDragFeedbackState& presented,
    const WidgetDragFeedbackState& current)
{
    return current.active && presented != current;
}

constexpr bool BelongsToCompositionRoot(
    CompositionHost visualHost,
    CompositionHost rootHost)
{
    return visualHost == rootHost;
}

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

constexpr bool ShouldDeferWidgetSurfaceDraw(
    bool desktopPaintInProgress,
    bool floatingDockPaintInProgress,
    bool floatingPopupPaintInProgress)
{
    return desktopPaintInProgress || floatingDockPaintInProgress ||
        floatingPopupPaintInProgress;
}

constexpr bool SurfaceIncludesDesktop(std::string_view surface)
{
    return surface.empty() || surface == "desktop";
}

constexpr bool SurfaceIncludesAuxiliary(std::string_view surface)
{
    return surface.empty() || surface == "panel" ||
        surface == "dialog" || surface == "popover";
}
}
