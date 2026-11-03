#include "dock_magnification.h"

#include <algorithm>
#include <cmath>

namespace snowdesktop::dock_magnification
{
float ResolveFocusScale(
    int effect, float configuredScale, bool animationsEnabled) noexcept
{
    if (!animationsEnabled || effect == 0)
        return 1.0f;
    if (effect == 1)
        return kSingleFocusScale;
    return std::isfinite(configuredScale)
        ? std::clamp(configuredScale, 1.0f, 2.0f)
        : kFocusScale;
}

float ScaleGrowthMultiplier(float focusScale) noexcept
{
    return (ResolveFocusScale(2, focusScale, true) - 1.0f) /
        (kFocusScale - 1.0f);
}

int FocusSwitchHysteresisPixels(int itemPitch)
{
    return std::clamp(
        std::max(1, itemPitch) / 16,
        kMinimumFocusSwitchHysteresisPixels,
        kMaximumFocusSwitchHysteresisPixels);
}

bool HasCrossedFocusSwitchBoundary(
    int previousCenter, int nextCenter,
    int pointerAxis, int itemPitch)
{
    if (previousCenter == nextCenter)
        return true;
    const int midpoint =
        previousCenter + (nextCenter - previousCenter) / 2;
    const int hysteresis =
        FocusSwitchHysteresisPixels(itemPitch);
    return nextCenter > previousCenter
        ? pointerAxis >= midpoint + hysteresis
        : pointerAxis <= midpoint - hysteresis;
}

RECT ExpandFocusRetentionBounds(RECT visualBounds)
{
    InflateRect(
        &visualBounds,
        kFocusExitHysteresisPixels,
        kFocusExitHysteresisPixels);
    return visualBounds;
}

RECT ExpandHoverPresentationBounds(RECT interactionBounds)
{
    return ExpandFocusRetentionBounds(interactionBounds);
}

bool ShouldTrackHoverPresentation(
    RECT interactionBounds, POINT previous, POINT current)
{
    const RECT presentationBounds =
        ExpandHoverPresentationBounds(interactionBounds);
    return PtInRect(&presentationBounds, previous) != FALSE ||
        PtInRect(&presentationBounds, current) != FALSE;
}

float SmoothStep(float progress)
{
    progress = std::clamp(progress, 0.0f, 1.0f);
    return progress * progress *
        (3.0f - 2.0f * progress);
}

float InterpolateScale(
    float from, float to, float progress)
{
    const float eased = SmoothStep(progress);
    return from + (to - from) * eased;
}

int GrowthForScale(float scale, int baseIconSize)
{
    return std::max(0, static_cast<int>(std::round(
        std::max(1, baseIconSize) * (std::max(1.0f, scale) - 1.0f))));
}

SingleFocusGeometry ResolveSingleFocusGeometry(
    const std::vector<RECT>& candidates, bool vertical,
    int pointerAxis, int baseIconSize, float focusScale)
{
    SingleFocusGeometry result;
    result.vertical = vertical;
    for (const RECT& candidate : candidates)
    {
        if (IsRectEmpty(&candidate)) continue;
        const int center = result.Center(candidate);
        if (center <= pointerAxis)
        {
            if (IsRectEmpty(&result.leading) || center > result.Center(result.leading))
                result.leading = candidate;
        }
        else if (IsRectEmpty(&result.trailing) || center < result.Center(result.trailing))
            result.trailing = candidate;
    }
    const int growth = GrowthForScale(focusScale, baseIconSize);
    if (IsRectEmpty(&result.leading))
    {
        result.leading = result.trailing;
        result.trailing = {};
    }
    else if (!IsRectEmpty(&result.trailing))
    {
        const float progress = static_cast<float>(pointerAxis - result.Center(result.leading)) /
            static_cast<float>(result.Center(result.trailing) - result.Center(result.leading));
        result.trailingGrowth = static_cast<int>(std::lround(
            static_cast<float>(growth) * SmoothStep(progress)));
    }
    if (!IsRectEmpty(&result.leading)) result.leadingGrowth = growth - result.trailingGrowth;
    return result;
}

float ScaleForAxisDistance(
    float centerDistance, int itemPitch, float focusScale)
{
    focusScale = ResolveFocusScale(2, focusScale, true);
    const float multiplier = ScaleGrowthMultiplier(focusScale);
    const float firstNeighborScale =
        1.0f + (kFirstNeighborScale - 1.0f) * multiplier;
    const float secondNeighborScale =
        1.0f + (kSecondNeighborScale - 1.0f) * multiplier;
    const int pitch = std::max(1, itemPitch);
    const float distanceInItems =
        static_cast<float>(std::abs(centerDistance)) /
        static_cast<float>(pitch);
    if (distanceInItems < 1.0f)
        return InterpolateScale(
            focusScale, firstNeighborScale,
            distanceInItems);
    if (distanceInItems < 2.0f)
        return InterpolateScale(
            firstNeighborScale,
            secondNeighborScale,
            distanceInItems - 1.0f);
    if (distanceInItems < kInfluenceRadiusInItems)
        return InterpolateScale(
            secondNeighborScale, 1.0f,
            distanceInItems - 2.0f);
    return 1.0f;
}

float ScaleForEffect(
    int effect, bool focused, float centerDistance, int itemPitch,
    float focusScale)
{
    if (effect == 0)
        return 1.0f;
    if (effect == 1)
        return focused ? (std::isfinite(focusScale) ?
            std::clamp(focusScale, 1.0f, kSingleFocusScale) : kSingleFocusScale) : 1.0f;
    return ScaleForAxisDistance(centerDistance, itemPitch, focusScale);
}

double IntegratedGrowthInItems(
    double distanceInItems, float focusScale)
{
    const double distance = std::clamp(
        distanceInItems, 0.0,
        static_cast<double>(
            kInfluenceRadiusInItems));
    constexpr double growth[] = {
        static_cast<double>(kFocusScale - 1.0f),
        static_cast<double>(
            kFirstNeighborScale - 1.0f),
        static_cast<double>(
            kSecondNeighborScale - 1.0f),
        0.0
    };

    double integral = 0.0;
    const int wholeSegments =
        std::min(3, static_cast<int>(
            std::floor(distance)));
    for (int segment = 0;
        segment < wholeSegments; ++segment)
    {
        integral +=
            (growth[segment] +
                growth[segment + 1]) *
            0.5;
    }

    if (wholeSegments < 3 &&
        distance > wholeSegments)
    {
        const double progress =
            distance - wholeSegments;
        const double smoothStepIntegral =
            progress * progress * progress -
            0.5 * progress * progress *
                progress * progress;
        integral += growth[wholeSegments] *
            progress +
            (growth[wholeSegments + 1] -
                growth[wholeSegments]) *
            smoothStepIntegral;
    }
    return integral * ScaleGrowthMultiplier(focusScale);
}

int AxisShiftForDistance(
    int centerDistance, int itemPitch, int baseIconSize,
    float focusScale)
{
    if (centerDistance == 0)
        return 0;

    const double distanceInItems =
        static_cast<double>(
            std::abs(centerDistance)) /
        static_cast<double>(
            std::max(1, itemPitch));
    const int magnitude = static_cast<int>(
        std::lround(
            std::max(1, baseIconSize) *
            IntegratedGrowthInItems(
                distanceInItems, focusScale)));
    return centerDistance < 0 ? -magnitude : magnitude;
}

int MaximumAxisShift(
    int baseIconSize, float focusScale)
{
    return AxisShiftForDistance(3, 1, baseIconSize, focusScale);
}

int SingleFocusAxisShift(
    int centerDistanceFromFocus, int baseIconSize, float focusScale)
{
    if (centerDistanceFromFocus == 0)
        return 0;
    const int growth = GrowthForScale(focusScale, baseIconSize);
    return centerDistanceFromFocus < 0
        ? -(growth / 2) : growth - growth / 2;
}

int IslandAxisShift(
    int effect, int baseCenter, int focusCenter, int pointerAxis,
    int itemPitch, int baseIconSize, float focusScale)
{
    if (effect == 0)
        return 0;
    if (effect == 1)
        return SingleFocusAxisShift(
            baseCenter - focusCenter, baseIconSize, focusScale);
    return AxisShiftForDistance(
        baseCenter - pointerAxis, itemPitch, baseIconSize, focusScale);
}

int PackedAxisShift(
    const std::vector<float>& scales, size_t index,
    int baseIconSize, bool towardPositiveAxis)
{
    if (index >= scales.size())
        return 0;

    const auto growthAt = [&](size_t candidate) {
        return GrowthForScale(scales[candidate], baseIconSize);
    };
    const int currentGrowth = growthAt(index);
    if (towardPositiveAxis)
    {
        int previousGrowth = 0;
        for (size_t candidate = 0; candidate < index; ++candidate)
            previousGrowth += growthAt(candidate);
        return previousGrowth + currentGrowth / 2;
    }

    int followingGrowth = 0;
    for (size_t candidate = index + 1;
        candidate < scales.size(); ++candidate)
    {
        followingGrowth += growthAt(candidate);
    }
    return -(followingGrowth +
        (currentGrowth - currentGrowth / 2));
}

RECT MagnifyRect(
    RECT base, DockPosition position, float scale, int baseIconSize,
    int axisShift, bool centered)
{
    const bool vertical = position == DockPosition::Left ||
        position == DockPosition::Right;
    OffsetRect(&base, vertical ? 0 : axisShift,
        vertical ? axisShift : 0);

    const int growth = GrowthForScale(scale, baseIconSize);
    if (growth == 0)
        return base;

    const int leadingGrowth = growth / 2;
    const int trailingGrowth = growth - leadingGrowth;
    if (centered)
    {
        base.left -= leadingGrowth; base.right += trailingGrowth;
        base.top -= leadingGrowth; base.bottom += trailingGrowth;
        return base;
    }
    switch (position)
    {
    case DockPosition::Top:
        base.left -= leadingGrowth;
        base.right += trailingGrowth;
        base.bottom += growth;
        break;
    case DockPosition::Left:
        base.top -= leadingGrowth;
        base.bottom += trailingGrowth;
        base.right += growth;
        break;
    case DockPosition::Right:
        base.top -= leadingGrowth;
        base.bottom += trailingGrowth;
        base.left -= growth;
        break;
    case DockPosition::Bottom:
    default:
        base.left -= leadingGrowth;
        base.right += trailingGrowth;
        base.top -= growth;
        break;
    }
    return base;
}

int IconSizeForVisualRect(const RECT& base, const RECT& visual, int baseIconSize)
{
    const LONG growth = std::max(0L, std::min(
        (visual.right - visual.left) - (base.right - base.left),
        (visual.bottom - visual.top) - (base.bottom - base.top)));
    return std::max(1, baseIconSize + static_cast<int>(growth));
}

RECT AnchorTooltipBounds(
    const RECT& visualBounds, DockPosition position,
    int tooltipWidth, int tooltipHeight, int gap)
{
    tooltipWidth = std::max(0, tooltipWidth);
    tooltipHeight = std::max(0, tooltipHeight);
    gap = std::max(0, gap);

    RECT tooltip{};
    switch (position)
    {
    case DockPosition::Top:
        tooltip.left =
            (visualBounds.left + visualBounds.right -
                tooltipWidth) / 2;
        tooltip.top = visualBounds.bottom + gap;
        break;
    case DockPosition::Left:
        tooltip.left = visualBounds.right + gap;
        tooltip.top =
            (visualBounds.top + visualBounds.bottom -
                tooltipHeight) / 2;
        break;
    case DockPosition::Right:
        tooltip.left =
            visualBounds.left - gap - tooltipWidth;
        tooltip.top =
            (visualBounds.top + visualBounds.bottom -
                tooltipHeight) / 2;
        break;
    case DockPosition::Bottom:
    default:
        tooltip.left =
            (visualBounds.left + visualBounds.right -
                tooltipWidth) / 2;
        tooltip.top =
            visualBounds.top - gap - tooltipHeight;
        break;
    }
    tooltip.right = tooltip.left + tooltipWidth;
    tooltip.bottom = tooltip.top + tooltipHeight;
    return tooltip;
}

RECT ExpandInteractionBounds(
    RECT bounds, DockPosition position, int baseIconSize,
    float focusScale, bool centered)
{
    const int growth = GrowthForScale(focusScale, baseIconSize);
    if (growth == 0)
        return bounds;
    const int axisPadding = std::max(1,
        MaximumAxisShift(baseIconSize, focusScale) +
        (growth + 1) / 2);
    if (centered)
    {
        const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
        bounds.left -= vertical ? growth / 2 : axisPadding;
        bounds.right += vertical ? growth - growth / 2 : axisPadding;
        bounds.top -= vertical ? axisPadding : growth / 2;
        bounds.bottom += vertical ? axisPadding : growth - growth / 2;
        return bounds;
    }
    switch (position)
    {
    case DockPosition::Top:
        bounds.left -= axisPadding;
        bounds.right += axisPadding;
        bounds.bottom += growth;
        break;
    case DockPosition::Left:
        bounds.top -= axisPadding;
        bounds.bottom += axisPadding;
        bounds.right += growth;
        break;
    case DockPosition::Right:
        bounds.top -= axisPadding;
        bounds.bottom += axisPadding;
        bounds.left -= growth;
        break;
    case DockPosition::Bottom:
    default:
        bounds.left -= axisPadding;
        bounds.right += axisPadding;
        bounds.top -= growth;
        break;
    }
    return bounds;
}

RECT ExpandPerpendicularBounds(
    RECT bounds, DockPosition position, int baseIconSize,
    float focusScale, bool centered)
{
    const int growth = GrowthForScale(focusScale, baseIconSize);
    if (centered)
    {
        const bool vertical = position == DockPosition::Left || position == DockPosition::Right;
        if (vertical) { bounds.left -= growth / 2; bounds.right += growth - growth / 2; }
        else { bounds.top -= growth / 2; bounds.bottom += growth - growth / 2; }
        return bounds;
    }
    switch (position)
    {
    case DockPosition::Top:
        bounds.bottom += growth;
        break;
    case DockPosition::Left:
        bounds.right += growth;
        break;
    case DockPosition::Right:
        bounds.left -= growth;
        break;
    case DockPosition::Bottom:
    default:
        bounds.top -= growth;
        break;
    }
    return bounds;
}

RECT ExpandSeparatorHoverBounds(
    RECT bounds, DockPosition position, int baseIconSize,
    float focusScale)
{
    return ExpandPerpendicularBounds(
        bounds, position, baseIconSize, focusScale);
}

RECT ResolveFocusInteractionBounds(
    RECT bounds, DockPosition position, int baseIconSize,
    bool magnificationActive, float focusScale)
{
    return magnificationActive
        ? ExpandSeparatorHoverBounds(
            bounds, position, baseIconSize, focusScale)
        : bounds;
}

RECT FitOverflowViewportToFixedVisuals(
    RECT viewport, DockPosition position,
    const RECT& leadingVisual, const RECT& trailingVisual,
    int separatorGap)
{
    const int gap = std::max(0, separatorGap);
    const bool vertical = position == DockPosition::Left ||
        position == DockPosition::Right;
    if (vertical)
    {
        if (!IsRectEmpty(&leadingVisual))
            viewport.top = std::max(
                viewport.top, leadingVisual.bottom + gap);
        if (!IsRectEmpty(&trailingVisual))
            viewport.bottom = std::min(
                viewport.bottom, trailingVisual.top - gap);
        viewport.bottom = std::max(
            viewport.top, viewport.bottom);
    }
    else
    {
        if (!IsRectEmpty(&leadingVisual))
            viewport.left = std::max(
                viewport.left, leadingVisual.right + gap);
        if (!IsRectEmpty(&trailingVisual))
            viewport.right = std::min(
                viewport.right, trailingVisual.left - gap);
        viewport.right = std::max(
            viewport.left, viewport.right);
    }
    return viewport;
}

RECT MoveOverflowViewportWithScrollableVisuals(
    RECT viewport, DockPosition position,
    const RECT& firstBase, const RECT& firstVisual,
    const RECT& lastBase, const RECT& lastVisual)
{
    if (IsRectEmpty(&firstBase) ||
        IsRectEmpty(&firstVisual) ||
        IsRectEmpty(&lastBase) ||
        IsRectEmpty(&lastVisual))
    {
        return viewport;
    }

    const bool vertical = position == DockPosition::Left ||
        position == DockPosition::Right;
    if (vertical)
    {
        viewport.top += firstVisual.top - firstBase.top;
        viewport.bottom += lastVisual.bottom - lastBase.bottom;
        viewport.bottom = std::max(
            viewport.top, viewport.bottom);
    }
    else
    {
        viewport.left += firstVisual.left - firstBase.left;
        viewport.right += lastVisual.right - lastBase.right;
        viewport.right = std::max(
            viewport.left, viewport.right);
    }
    return viewport;
}

RECT ClipPanelToWorkArea(RECT panel, RECT workArea)
{
    RECT clipped{};
    return IntersectRect(&clipped, &panel, &workArea) ? clipped : panel;
}

RECT ExtendPanelAlongDockAxis(
    RECT panel, const RECT& visualElement, DockPosition position,
    int axisMargin)
{
    const int margin = std::max(0, axisMargin);
    const bool vertical = position == DockPosition::Left ||
        position == DockPosition::Right;
    if (vertical)
    {
        panel.top = std::min(panel.top, visualElement.top - margin);
        panel.bottom = std::max(panel.bottom, visualElement.bottom + margin);
    }
    else
    {
        panel.left = std::min(panel.left, visualElement.left - margin);
        panel.right = std::max(panel.right, visualElement.right + margin);
    }
    return panel;
}

void HoverEntryAnimation::SetHovered(bool hovered, double now, double durationScale)
{
    if (!hovered)
    {
        *this = {};
        return;
    }
    if (hovered_)
        return;
    hovered_ = true;
    started_ = now;
    duration_ = kDurationMilliseconds * std::max(0.01, durationScale);
    progress_ = 0.0f;
}

bool HoverEntryAnimation::IsAnimating() const
{ return hovered_ && progress_ < 1.0f; }

void HoverEntryAnimation::Advance(double now)
{
    if (IsAnimating())
        progress_ = std::max(progress_, static_cast<float>(
            std::clamp((now - started_) / duration_, 0.0, 1.0)));
}

float HoverEntryAnimation::FocusScale(float maximumScale) const
{
    return InterpolateScale(1.0f, maximumScale, progress_);
}

void SingleFocusAnimation::SetTarget(RECT target, double now, double durationScale, int pointerAxis)
{
    if (!IsRectEmpty(&target)) pointerAxis_ = pointerAxis;
    if (EqualRect(&target, &requested_)) return;
    Advance(now);
    requested_ = target;
    const double speed = std::isfinite(durationScale) ? std::max(0.01, durationScale) : 1.0;
    if (!IsRectEmpty(&target))
    {
        current_ = target;
        if (animating_ && to_ == 1.0f) return;
    }
    Begin(IsRectEmpty(&target) ? 0.0f : 1.0f, now, kDurationMilliseconds * speed);
}

void SingleFocusAnimation::Advance(double now)
{
    if (!animating_) return;
    const float progress = static_cast<float>(std::clamp((now - started_) / duration_, 0.0, 1.0));
    amount_ = InterpolateScale(from_, to_, progress);
    if (progress < 1.0f) return;
    animating_ = false;
    if (to_ == 0.0f)
        current_ = requested_;
}

bool SingleFocusAnimation::IsAnimating() const
{ return animating_; }

const RECT& SingleFocusAnimation::CurrentRect() const
{ return current_; }

int SingleFocusAnimation::PointerAxis() const
{ return pointerAxis_; }

float SingleFocusAnimation::Scale() const
{ return 1.0f + (kSingleFocusScale - 1.0f) * amount_; }

void SingleFocusAnimation::Begin(float to, double now, double duration)
{
    from_ = amount_; to_ = to; started_ = now; duration_ = duration;
    animating_ = from_ != to_;
}

int SingleFocusGeometry::Center(const RECT& rect) const
{ return vertical ? (rect.top + rect.bottom) / 2 : (rect.left + rect.right) / 2; }

int SingleFocusGeometry::GrowthFor(const RECT& rect) const
{
    if (!IsRectEmpty(&leading) && EqualRect(&rect, &leading)) return leadingGrowth;
    if (!IsRectEmpty(&trailing) && EqualRect(&rect, &trailing)) return trailingGrowth;
    return 0;
}

float SingleFocusGeometry::ScaleFor(const RECT& rect, int baseIconSize) const
{ return 1.0f + static_cast<float>(GrowthFor(rect)) / std::max(1, baseIconSize); }

int SingleFocusGeometry::AxisShiftFor(const RECT& rect) const
{
    int precedingGrowth = 0;
    const int center = Center(rect);
    if (!IsRectEmpty(&leading) && center > Center(leading)) precedingGrowth += leadingGrowth;
    if (!IsRectEmpty(&trailing) && center > Center(trailing)) precedingGrowth += trailingGrowth;
    return precedingGrowth + GrowthFor(rect) / 2 - (leadingGrowth + trailingGrowth) / 2;
}
}
