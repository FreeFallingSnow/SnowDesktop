#pragma once
#include "large_icon_render_rules.h"
#include <array>

namespace snowdesktop::large_icon_preset_rules
{
struct Option { int value; const char* label; };
inline constexpr std::array backgrounds{
    Option{-5, "largeIcon.default"}, Option{-4, "largeIcon.platePreset"},
    Option{-2, "largeIcon.fill"}, Option{-1, "largeIcon.follow"},
    Option{0, "app.settings.dark"}, Option{1, "app.settings.light"},
    Option{6, "app.settings.dark_glass"}, Option{7, "app.settings.light_glass"},
    Option{10, "app.settings.dark_acrylic"}, Option{11, "app.settings.light_acrylic"},
    Option{9, "app.settings.custom"}};
inline constexpr std::array effects{
    Option{0, "largeIcon.noEffect"}, Option{1, "largeIcon.tilt"}, Option{2, "largeIcon.dynamicTitle"},
    Option{3, "largeIcon.zoom"}, Option{4, "largeIcon.edgeGlow"}, Option{5, "largeIcon.shine"}};
inline int Background(const LargeIconConfig& c)
{
    if (c.backgroundStyle != -3) return c.backgroundStyle;
    return c.defaultBackground == 0 && c.smartFill ? -4 : 9;
}
inline int Effect(const LargeIconConfig& c)
{ return IsLargeIconFill(c) && c.effect == 2 ? 0 : c.effect; }
// Creation and explicit reset use the current content mode. Layout decoding
// retains its historical defaults and never opts existing items into animation.
inline int DefaultEffect(const LargeIconConfig& c)
{ return IsLargeIconFill(c) ? 3 : 2; }
inline bool BackgroundVisible(int value, bool hasEdge, const LargeIconConfig& c)
{ return value != -4 || hasEdge || Background(c) == -4; }
// Convert only an editing copy. Loading a layout, losing entitlement or opening
// a menu never rewrites legacy appearance or the stored custom appearance.
inline void PrepareForEditing(LargeIconConfig& c, unsigned accent, bool hasEdge, unsigned edge)
{
    if (c.backgroundStyle != -3) return;
    const auto background = large_icon_render_rules::DefaultBackground(c, accent, hasEdge, edge);
    if (Background(c) == -4 && c.themeOpacity == .65) { c.backgroundStyle = -4; return; }
    if (c.defaultBackground == 0)
    { c.material = 0; c.border = false; c.edgeHighlight = false; }
    c.backgroundStyle = 9; c.manualColor = background.color; c.opacity = background.opacity;
    c.gradient = background.gradient; c.gradientOpacity = 1;
}
inline bool ApplyBackground(LargeIconConfig& c, int value, bool editable, bool hasEdge,
    unsigned accent = 0, unsigned edge = 0)
{
    if (!editable || std::none_of(backgrounds.begin(), backgrounds.end(),
        [value](auto option) { return option.value == value; }) ||
        (value == -4 && !hasEdge && Background(c) != -4)) return false;
    PrepareForEditing(c, accent, hasEdge, edge);
    c.backgroundStyle = value;
    if (IsLargeIconFill(c) && c.effect == 2) c.effect = DefaultEffect(c);
    return true;
}
inline bool ApplyEffect(LargeIconConfig& c, int value, bool editable)
{
    if (!editable || std::none_of(effects.begin(), effects.end(),
            [value](auto option) { return option.value == value; }) || (value == 2 && IsLargeIconFill(c))) return false;
    c.effect = value;
    return true;
}
}
