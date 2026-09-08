#pragma once
#include "large_icon_render_rules.h"

namespace snowdesktop::large_icon_settings_rules
{
enum class Field
{
    Always, Original, Image, Imported, Crop, Steam, ManualBackground, AutomaticBackground,
    HoverBackground, ManualHoverOpacity, BorderAppearance, BorderOpacity, ShadowStrength,
    ManualTitle, FloatingTitle, RevealTitle, OriginalMotion, CoverMotion, AnimationStrength
};
inline bool Visible(Field field, const LargeIconConfig& c)
{
    switch (field)
    {
    case Field::Original: return c.content == 0;
    case Field::Image: return c.content != 0;
    case Field::Imported: return c.content == 1;
    case Field::Crop: return c.content != 0 && c.fit == 1;
    case Field::Steam: return c.content == 2;
    case Field::ManualBackground: return !c.autoColor;
    case Field::AutomaticBackground: return c.autoColor;
    case Field::HoverBackground: return c.hoverFrame == 1;
    case Field::ManualHoverOpacity: return c.hoverFrame == 1 && !c.hoverOpacityLinked;
    case Field::BorderAppearance: return c.border || c.hoverFrame == 2;
    case Field::BorderOpacity: return c.border;
    case Field::ShadowStrength: return c.shadow;
    case Field::ManualTitle: return !c.autoTitleColor;
    case Field::FloatingTitle: return c.content != 0 || c.titleMode != 1;
    case Field::RevealTitle: return c.content == 0 && c.titleMode == 1;
    case Field::OriginalMotion: return c.content == 0 && c.titleMode != 1;
    case Field::CoverMotion: return c.content != 0;
    case Field::AnimationStrength: return c.launch != 0 || c.hoverFrame == 2 || c.hoverFrame == 3 ||
        (c.content == 0 ? c.hoverContent >= 2 && c.titleMode == 0 : c.coverHover == 1);
    default: return true;
    }
}
inline bool CanSelectLeftTitle(const LargeIconConfig& c, double width, double height,
    double sourceWidth, double sourceHeight, double scale, bool animations)
{
    auto candidate = c; candidate.titleMode = 1;
    return large_icon_render_rules::ResolveContent(candidate, width, height,
        sourceWidth, sourceHeight, scale, c.content == 0, animations, 0, false, 0).leftReveal;
}
}
