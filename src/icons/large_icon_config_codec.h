#pragma once

#include "large_icon_config_types.h"

#include <string>

struct JsonValue;

namespace snowdesktop
{
bool DecodeLargeIconConfig(const JsonValue& value, LargeIconConfig& result);
std::string EncodeLargeIconConfig(const LargeIconConfig& c);

// The JSON codec and both settings frontends use the same field names.
template<class C, class F> void VisitLargeIconFields(C& c, F&& f)
{
#define LI_FIELD(name) f(#name, c.name)
    LI_FIELD(version); LI_FIELD(columns); LI_FIELD(rows); LI_FIELD(shape); LI_FIELD(flagDirection); LI_FIELD(regularHexagon);
    LI_FIELD(followComponentRadius); LI_FIELD(showOnHoverOnly); LI_FIELD(keepWhenDesktopHidden);
    LI_FIELD(contentScale); LI_FIELD(fillScale); LI_FIELD(radius); LI_FIELD(radiusPercent); LI_FIELD(content); LI_FIELD(fit);
    LI_FIELD(focusX); LI_FIELD(focusY); LI_FIELD(image); LI_FIELD(imageAssetId); LI_FIELD(cachedCover);
    LI_FIELD(autoColor); LI_FIELD(manualColor); LI_FIELD(colorMix);
    LI_FIELD(opacity); LI_FIELD(hoverOpacity); LI_FIELD(border);
    LI_FIELD(hoverOpacityLinked);
    LI_FIELD(borderColor); LI_FIELD(borderWidth); LI_FIELD(borderOpacity);
    LI_FIELD(shadow); LI_FIELD(shadowStrength); LI_FIELD(titleMode);
    LI_FIELD(titleSize); LI_FIELD(revealTitleSize); LI_FIELD(autoTitleColor); LI_FIELD(titleColor);
    LI_FIELD(hoverContent); LI_FIELD(hoverFrame); LI_FIELD(press);
    LI_FIELD(launch); LI_FIELD(coverHover); LI_FIELD(amplitude);
    LI_FIELD(delayMs); LI_FIELD(enterMs); LI_FIELD(exitMs);
    LI_FIELD(steamOrientation); LI_FIELD(localOnly);
    LI_FIELD(backgroundStyle); LI_FIELD(smartFill); LI_FIELD(themeColor); LI_FIELD(themeGradient);
    LI_FIELD(defaultBackground); LI_FIELD(defaultSolidColor); LI_FIELD(defaultSolidOpacity); LI_FIELD(defaultGradient);
    LI_FIELD(themeOpacity); LI_FIELD(themeAngle); LI_FIELD(foregroundContent); LI_FIELD(foregroundImage); LI_FIELD(foregroundImageAssetId);
    LI_FIELD(iconX); LI_FIELD(iconY); LI_FIELD(material); LI_FIELD(componentTheme); LI_FIELD(blurRadius);
    LI_FIELD(edgeHighlight); LI_FIELD(edgeWidth); LI_FIELD(edgeStrength); LI_FIELD(edgeLight); LI_FIELD(gradient); LI_FIELD(gradientOpacity);
    LI_FIELD(effect); LI_FIELD(titleDirection); LI_FIELD(autoTitleDirection); LI_FIELD(titleWeight);
    LI_FIELD(zoomAmount); LI_FIELD(glowStrength); LI_FIELD(shineStrength); LI_FIELD(shineDurationMs);
#undef LI_FIELD
}
}
