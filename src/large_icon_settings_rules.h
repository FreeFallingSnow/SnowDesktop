#pragma once
#include "large_icon_render_rules.h"

namespace snowdesktop::large_icon_settings_rules
{
enum class Field
{
    Always, Default, Smart, ThemeOptions, ThemeGradient, Fill, FillImage, Crop, Steam,
    Custom, Solid, Blur, Border, Edge, Foreground, ForegroundImage, ForegroundPosition,
    Title, ManualTitle, Tilt
};
inline bool Visible(Field field, const LargeIconConfig& c, bool hasEdge = false)
{
    const bool fill = IsLargeIconFill(c), custom = c.backgroundStyle == 9, automatic = c.backgroundStyle == -3;
    switch (field)
    {
    case Field::Default: return automatic;
    case Field::Smart: return automatic && hasEdge;
    case Field::ThemeOptions: return automatic && c.themeColor;
    case Field::ThemeGradient: return automatic && c.themeColor && c.themeGradient;
    case Field::Fill: return fill;
    case Field::FillImage: return fill && c.content == 1;
    case Field::Crop: return fill && c.fit == 1;
    case Field::Steam: return fill && c.content == 2;
    case Field::Custom: return custom;
    case Field::Solid: return custom && !c.gradient.enabled;
    case Field::Blur: return custom && c.material != 0;
    case Field::Border: return custom && c.border;
    case Field::Edge: return custom && c.edgeHighlight;
    case Field::Foreground: case Field::ForegroundPosition: return !fill;
    case Field::ForegroundImage: return !fill && c.foregroundContent == 1;
    case Field::Title: return c.effect == 2;
    case Field::ManualTitle: return c.effect == 2 && !c.autoTitleColor;
    case Field::Tilt: return c.effect == 1;
    default: return true;
    }
}
inline bool Enabled(Field field, const LargeIconConfig& c, bool hasEdge, bool hasTheme)
{
    if (field == Field::ThemeOptions || field == Field::ThemeGradient) return hasTheme && !(hasEdge && c.smartFill);
    if (field == Field::ForegroundPosition) return c.effect != 2;
    return true;
}
}
