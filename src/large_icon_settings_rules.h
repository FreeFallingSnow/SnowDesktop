#pragma once
#include "large_icon_render_rules.h"

namespace snowdesktop::large_icon_settings_rules
{
enum class Field
{
    Always, Fill, FillImage, Crop, Steam,
    Custom, Gradient, Solid, Blur, Border, Edge, Foreground, ForegroundImage, ForegroundPosition,
    Title, ManualTitle, Tilt, Zoom, Glow, Shine, Radius
};
inline bool Visible(Field field, const LargeIconConfig& c, bool hasEdge = false)
{
    (void)hasEdge;
    const bool fill = IsLargeIconFill(c), custom = c.backgroundStyle == 9;
    switch (field)
    {
    case Field::Radius: return !c.followComponentRadius;
    case Field::Fill: return fill;
    case Field::FillImage: return fill && c.content == 1;
    case Field::Crop: return fill && c.fit == 1;
    case Field::Steam: return fill && c.content == 2;
    case Field::Custom: return custom;
    case Field::Gradient: return custom && c.gradient.enabled;
    case Field::Solid: return custom && !c.gradient.enabled;
    case Field::Blur: return custom && c.material != 0;
    case Field::Border: return custom && c.border;
    case Field::Edge: return custom && c.edgeHighlight;
    case Field::Foreground: case Field::ForegroundPosition: return !fill;
    case Field::ForegroundImage: return !fill && c.foregroundContent == 1;
    case Field::Title: return !fill && c.effect == 2;
    case Field::ManualTitle: return !fill && c.effect == 2 && !c.autoTitleColor;
    case Field::Tilt: return c.effect == 1;
    case Field::Zoom: return c.effect == 3;
    case Field::Glow: return c.effect == 4;
    case Field::Shine: return c.effect == 5;
    default: return true;
    }
}
inline bool Enabled(Field field, const LargeIconConfig& c, bool hasEdge, bool hasTheme)
{
    (void)hasEdge;
    (void)hasTheme;
    if (field == Field::ForegroundPosition) return c.effect != 2;
    return true;
}
}
