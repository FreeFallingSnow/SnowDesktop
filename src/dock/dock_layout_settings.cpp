#include "dock_layout_settings.h"

#include <algorithm>
#include <cmath>

float ClampDockEdgeDistanceScale(float scale)
{
    return std::isfinite(scale)
        ? std::clamp(scale, kDockMinimumEdgeDistanceScale, kDockMaximumEdgeDistanceScale)
        : 1.0f;
}

int ScaleDockEdgeDistance(int baseMargin, float scale)
{
    if (baseMargin <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(
        baseMargin * ClampDockEdgeDistanceScale(scale))));
}

float ClampMergedDockIconScale(float scale)
{
    return std::clamp(scale, kDockMinimumScale, 1.00f);
}

float ClampDockScale(float scale)
{
    return std::clamp(scale, kDockMinimumScale, kDockMaximumScale);
}

float ResolveDockScale(const DockLayoutSettings& settings, bool merged)
{
    return merged ? ClampMergedDockIconScale(settings.mergedIconScale) : ClampDockScale(settings.thicknessScale);
}
