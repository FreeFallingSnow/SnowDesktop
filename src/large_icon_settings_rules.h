#pragma once
#include "large_icon_render_rules.h"

namespace snowdesktop::large_icon_settings_rules
{
enum class Field
{
    Always, Default, Smart, ThemeOptions, ThemeGradient, Fill, FillImage, Crop, Steam,
    Custom, EditableBackground, DefaultSolid, Gradient, Solid, Blur, Border, Edge, Foreground, ForegroundImage, ForegroundPosition,
    Title, ManualTitle, Tilt
};
inline bool Visible(Field field, const LargeIconConfig& c, bool hasEdge = false)
{
    const bool fill = IsLargeIconFill(c), custom = c.backgroundStyle == 9, automatic = c.backgroundStyle == -3;
    const bool editable = custom || (automatic && c.defaultBackground != 0);
    switch (field)
    {
    case Field::Default: return automatic;
    case Field::Smart: return automatic && c.defaultBackground == 0 && hasEdge;
    case Field::ThemeOptions: return automatic && c.defaultBackground == 0;
    case Field::ThemeGradient: return automatic && c.defaultBackground == 1;
    case Field::Fill: return fill;
    case Field::FillImage: return fill && c.content == 1;
    case Field::Crop: return fill && c.fit == 1;
    case Field::Steam: return fill && c.content == 2;
    case Field::Custom: return custom;
    case Field::EditableBackground: return editable;
    case Field::DefaultSolid: return automatic && c.defaultBackground == 2;
    case Field::Gradient: return custom || (automatic && c.defaultBackground == 1);
    case Field::Solid: return custom && !c.gradient.enabled;
    case Field::Blur: return editable && c.material != 0;
    case Field::Border: return editable && c.border;
    case Field::Edge: return editable && c.edgeHighlight;
    case Field::Foreground: case Field::ForegroundPosition: return !fill;
    case Field::ForegroundImage: return !fill && c.foregroundContent == 1;
    case Field::Title: return !fill && c.effect == 2;
    case Field::ManualTitle: return !fill && c.effect == 2 && !c.autoTitleColor;
    case Field::Tilt: return c.effect == 1;
    default: return true;
    }
}
inline bool Enabled(Field field, const LargeIconConfig& c, bool hasEdge, bool hasTheme)
{
    if (field == Field::ThemeOptions) return !(hasEdge && c.smartFill && c.defaultBackground == 0);
    (void)hasTheme;
    if (field == Field::ForegroundPosition) return c.effect != 2;
    return true;
}
}
