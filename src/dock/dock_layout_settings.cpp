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

DockLayoutSettings ApplyDockMonitorOverride(DockLayoutSettings base, const DockMonitorOverride& value)
{
    base.position = value.position.value_or(base.position);
    base.edgeAttached = value.edgeAttached.value_or(base.edgeAttached);
    base.edgeCentered = value.edgeCentered.value_or(base.edgeCentered);
    base.thicknessScale = ClampDockScale(value.thicknessScale.value_or(base.thicknessScale));
    base.showOnlyWhenSummoned = value.showOnlyWhenSummoned.value_or(base.showOnlyWhenSummoned);
    base.allowDesktopContentOverlap = value.allowDesktopContentOverlap.value_or(base.allowDesktopContentOverlap);
    base.reserveScreenSpace = value.reserveScreenSpace.value_or(base.reserveScreenSpace);
    base.showFrequentItems = value.showFrequentItems.value_or(base.showFrequentItems);
    base.keepWhenDesktopHidden = value.keepWhenDesktopHidden.value_or(base.keepWhenDesktopHidden);
    base.showRunningArea = value.showRunningArea.value_or(base.showRunningArea);
    return base;
}
