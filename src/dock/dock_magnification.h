#pragma once

#include "dock_layout_settings.h"

#include <cstddef>
#include <vector>
#include <windows.h>

namespace snowdesktop::dock_magnification
{
constexpr float kFocusScale = 1.28f;
constexpr float kSingleFocusScale = 1.12f;
constexpr float kFirstNeighborScale = 1.14f;
constexpr float kSecondNeighborScale = 1.05f;
constexpr float kInfluenceRadiusInItems = 3.0f;
constexpr int kMinimumFocusSwitchHysteresisPixels = 3;
constexpr int kMaximumFocusSwitchHysteresisPixels = 8;
constexpr int kFocusExitHysteresisPixels = 5;

float ResolveFocusScale(
    int effect, float configuredScale, bool animationsEnabled) noexcept;

float ScaleGrowthMultiplier(float focusScale) noexcept;

inline constexpr bool ShouldSuppressMagnification(
    bool widgetResizeActive,
    bool inputAvailable = true)
{
    // Item and component drags retain the passive pointer hover timeline.
    // Titles have a separate drag gate so they do not obscure drop feedback.
    return !inputAvailable || widgetResizeActive;
}

// Shared full-width chrome does not pin the Dock's magnification to an edge.
// Its icons use the same centered growth and displacement as an island Dock.
inline constexpr bool UsesEdgeAnchoredMagnification(
    bool edgeAttached, bool mergedWithStatusBar)
{
    return edgeAttached && !mergedWithStatusBar;
}

int FocusSwitchHysteresisPixels(int itemPitch);

/**
 * @brief 判断指针是否明确越过相邻元素的切换边界。
 *
 * midpoint 两侧形成一个小型 Schmitt 区间：当前目标不同，越界方向也
 * 不同，从而避免边界上的 1px 抖动让 focus 来回翻转。
 */
bool HasCrossedFocusSwitchBoundary(
    int previousCenter, int nextCenter,
    int pointerAxis, int itemPitch);

RECT ExpandFocusRetentionBounds(RECT visualBounds);

/**
 * @brief Expand the passive-hover presentation gate through focus retention.
 *
 * The interaction bounds already cover the largest magnified wave. The focus
 * resolver can keep that wave alive for a few more pixels, so pointer-driven
 * presentation must keep running through the same exit margin. Otherwise the
 * first point outside the interaction bounds can render a retained focus and
 * the following point has no Dock hover owner left to submit its clearing
 * frame.
 */
RECT ExpandHoverPresentationBounds(RECT interactionBounds);

bool ShouldTrackHoverPresentation(
    RECT interactionBounds, POINT previous, POINT current);

float SmoothStep(float progress);

float InterpolateScale(
    float from, float to, float progress);

// Only the entry amplitude is animated. The wave center continues to follow
// each pointer sample immediately, including while the entry is in progress.
class HoverEntryAnimation
{
public:
    static constexpr double kDurationMilliseconds = 160.0;

    void SetHovered(bool hovered, double now, double durationScale);

    bool IsAnimating() const;

    void Advance(double now);

    float FocusScale(float maximumScale) const;

private:
    bool hovered_ = false;
    double started_ = 0.0;
    double duration_ = kDurationMilliseconds;
    float progress_ = 0.0f;
};

// Animate only entry/exit amplitude. Pointer movement transfers that amplitude
// between adjacent icons without shrinking the Dock at every semantic switch.
class SingleFocusAnimation
{
public:
    static constexpr double kDurationMilliseconds = 80.0;
    void SetTarget(RECT target, double now, double durationScale, int pointerAxis);

    void Advance(double now);

    bool IsAnimating() const;
    const RECT& CurrentRect() const;
    int PointerAxis() const;
    float Scale() const;

private:
    void Begin(float to, double now, double duration);
    RECT current_{}, requested_{};
    float amount_ = 0.0f, from_ = 0.0f, to_ = 0.0f;
    double started_ = 0.0, duration_ = kDurationMilliseconds;
    int pointerAxis_ = 0;
    bool animating_ = false;
};

int GrowthForScale(float scale, int baseIconSize);

// The two icons surrounding the pointer share one fixed growth budget. Packing
// with that same integer budget keeps gaps and both Dock ends stable, including
// odd-pixel growth and unequal distances across separators.
struct SingleFocusGeometry
{
    RECT leading{}, trailing{};
    int leadingGrowth = 0, trailingGrowth = 0;
    bool vertical = false;

    int Center(const RECT& rect) const;

    int GrowthFor(const RECT& rect) const;

    float ScaleFor(const RECT& rect, int baseIconSize) const;

    int AxisShiftFor(const RECT& rect) const;
};

SingleFocusGeometry ResolveSingleFocusGeometry(
    const std::vector<RECT>& candidates, bool vertical,
    int pointerAxis, int baseIconSize, float focusScale);

float ScaleForAxisDistance(
    float centerDistance, int itemPitch, float focusScale = kFocusScale);

float ScaleForEffect(
    int effect, bool focused, float centerDistance, int itemPitch,
    float focusScale);

double IntegratedGrowthInItems(
    double distanceInItems, float focusScale = kFocusScale);

int AxisShiftForDistance(
    int centerDistance, int itemPitch, int baseIconSize,
    float focusScale = kFocusScale);

int MaximumAxisShift(
    int baseIconSize, float focusScale = kFocusScale);

int SingleFocusAxisShift(
    int centerDistanceFromFocus, int baseIconSize, float focusScale);

int IslandAxisShift(
    int effect, int baseCenter, int focusCenter, int pointerAxis,
    int itemPitch, int baseIconSize, float focusScale);

int PackedAxisShift(
    const std::vector<float>& scales, size_t index,
    int baseIconSize, bool towardPositiveAxis);

RECT MagnifyRect(
    RECT base, DockPosition position, float scale, int baseIconSize,
    int axisShift = 0, bool centered = false);

// The cell can be shorter than icon+spacing in a compact fused bar. Preserve
// its logical icon size and apply only the growth of the actual visual frame;
// subtracting spacing from the cell's short side shrinks entries but not controls.
int IconSizeForVisualRect(const RECT& base, const RECT& visual, int baseIconSize);

RECT AnchorTooltipBounds(
    const RECT& visualBounds, DockPosition position,
    int tooltipWidth, int tooltipHeight, int gap);

RECT ExpandInteractionBounds(
    RECT bounds, DockPosition position, int baseIconSize,
    float focusScale = kFocusScale, bool centered = false);

RECT ExpandPerpendicularBounds(
    RECT bounds, DockPosition position, int baseIconSize,
    float focusScale = kFocusScale, bool centered = false);

/**
 * @brief 扩展分隔区的 hover 走廊，使其覆盖图标向桌面侧放大的高度。
 *
 * 分割线本身没有可命中的元素矩形；指针从相邻图标斜向经过分割线上方时，
 * 需要继续由最近的图标接管 focus，避免放大波形短暂归零。
 */
RECT ExpandSeparatorHoverBounds(
    RECT bounds, DockPosition position, int baseIconSize,
    float focusScale = kFocusScale);

RECT ResolveFocusInteractionBounds(
    RECT bounds, DockPosition position, int baseIconSize,
    bool magnificationActive, float focusScale = kFocusScale);

RECT FitOverflowViewportToFixedVisuals(
    RECT viewport, DockPosition position,
    const RECT& leadingVisual, const RECT& trailingVisual,
    int separatorGap);

RECT MoveOverflowViewportWithScrollableVisuals(
    RECT viewport, DockPosition position,
    const RECT& firstBase, const RECT& firstVisual,
    const RECT& lastBase, const RECT& lastVisual);

// A perpendicular status bar keeps its reservation during magnification.
// Clipping the expanded background prevents two translucent panels overlapping
// and creating a new material seam at the expanded edge.
RECT ClipPanelToWorkArea(RECT panel, RECT workArea);

RECT ExtendPanelAlongDockAxis(
    RECT panel, const RECT& visualElement, DockPosition position,
    int axisMargin = 0);
}
