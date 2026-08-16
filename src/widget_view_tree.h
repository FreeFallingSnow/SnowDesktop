#pragma once

#include "widget_interaction_region.h"
#include "widget_logical_slot.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget_runtime
{
enum class ViewNodeType
{
    Box,
    Row,
    Column,
    Grid,
    Flow,
    Stack,
    Scroll,
    List,
    GridList,
    VirtualList,
    VirtualGrid,
    ListItem,
    Text,
    StyledText,
    TextInput,
    TextArea,
    SearchBox,
    NumberInput,
    Select,
    Image,
    ReferenceIcon,
    Button,
    Link,
    Toggle,
    Checkbox,
    RadioGroup,
    Slider,
    Icon,
    IconButton,
    Shape,
    Badge,
    Divider,
    ProgressBar,
    ProgressRing,
    Meter,
    Sparkline,
    LineChart,
    BarChart,
    Waveform,
    Spectrum,
    MonthCalendar,
    SlotSurface,
    SlotItem,
    Spacer,
};

enum class ViewShapeKind
{
    Rectangle,
    RoundedRectangle,
    Circle,
    Ellipse,
};

enum class ViewIconFont
{
    FontAwesome,
    Fluent,
};

enum class ViewLengthKind
{
    Auto,
    Fill,
    Fixed,
};

struct ViewLength
{
    ViewLengthKind kind = ViewLengthKind::Auto;
    float value = 0.0f;
};

enum class ViewAlignment
{
    Auto,
    Start,
    Center,
    End,
    Stretch,
};

enum class ViewJustification
{
    Start,
    Center,
    End,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

enum class ViewFlexDirection
{
    Auto,
    Row,
    RowReverse,
    Column,
    ColumnReverse,
};

enum class ViewFlexWrap
{
    NoWrap,
    Wrap,
    WrapReverse,
};

enum class ViewContentAlignment
{
    Start,
    Center,
    End,
    Stretch,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

enum class ViewTextAlignment
{
    Start,
    Center,
    End,
};

enum class ViewTextWrap
{
    NoWrap,
    Wrap,
};

enum class ViewTextOverflow
{
    Clip,
    Ellipsis,
};

enum class ViewFontStyle
{
    Normal,
    Italic,
};

enum class ViewTextDirection
{
    Auto,
    LeftToRight,
    RightToLeft,
};

enum class ViewValidationState
{
    None,
    Info,
    Success,
    Warning,
    Error,
};

enum class ViewImageFit
{
    Fill,
    Contain,
    Cover,
    None,
};

enum class ViewImageAlignment
{
    Start,
    Center,
    End,
};

enum class ViewImageInterpolation
{
    Nearest,
    Linear,
};

enum class ViewOverflow
{
    Visible,
    Clip,
};

enum class ViewGridTrackKind
{
    Fixed,
    Auto,
    Fraction,
    MinMax,
};

enum class ViewOrientation
{
    Horizontal,
    Vertical,
};

enum class ViewSelectionMode
{
    None,
    Single,
    Multiple,
};

enum class ViewCollectionContent
{
    Items,
    Empty,
    Loading,
};

enum class ViewVisibility
{
    Visible,
    Hidden,
    Collapsed,
};

struct ViewRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool operator==(const ViewRect&) const = default;
};

struct ViewTextSelection
{
    // Zero-based half-open UTF-8 byte offsets in the controlled value.
    std::size_t start = 0;
    std::size_t finish = 0;

    bool operator==(const ViewTextSelection&) const = default;
};

struct ViewEdgeInsets
{
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;

    constexpr ViewEdgeInsets() = default;
    constexpr explicit ViewEdgeInsets(float uniform) noexcept
        : top(uniform), right(uniform), bottom(uniform), left(uniform)
    {
    }
    constexpr ViewEdgeInsets(float topValue, float rightValue,
        float bottomValue, float leftValue) noexcept
        : top(topValue), right(rightValue), bottom(bottomValue), left(leftValue)
    {
    }

    ViewEdgeInsets& operator=(float uniform) noexcept
    {
        top = right = bottom = left = uniform;
        return *this;
    }

    bool operator==(const ViewEdgeInsets&) const = default;
};

enum class ViewThemeColorToken
{
    WidgetBackground,
    Surface,
    SurfaceVariant,
    TextPrimary,
    TextSecondary,
    TextDisabled,
    Border,
    BorderStrong,
    SystemAccent,
    AccentText,
    Info,
    Success,
    Warning,
    Error,
};

struct ViewThemePalette
{
    std::uint32_t widgetBackground = 0x151A21;
    std::uint32_t surface = 0x23272D;
    std::uint32_t surfaceVariant = 0x32363B;
    std::uint32_t textPrimary = 0xFFFFFF;
    std::uint32_t textSecondary = 0xB9BBC0;
    std::uint32_t textDisabled = 0x777A80;
    std::uint32_t border = 0xFFFFFF;
    std::uint32_t borderStrong = 0xFFFFFF;
    std::uint32_t systemAccent = 0x0078D4;
    std::uint32_t accentText = 0xFFFFFF;
    std::uint32_t info = 0x72C7FF;
    std::uint32_t success = 0x55C271;
    std::uint32_t warning = 0xF2C94C;
    std::uint32_t error = 0xFF6B6B;

    bool operator==(const ViewThemePalette&) const = default;
};

struct ViewStyle
{
    std::optional<std::uint32_t> background;
    std::optional<ViewThemeColorToken> backgroundToken;
    std::optional<std::uint32_t> foreground;
    std::optional<ViewThemeColorToken> foregroundToken;
    std::optional<std::uint32_t> borderColor;
    std::optional<ViewThemeColorToken> borderColorToken;
    std::optional<float> borderWidth;
    std::optional<float> cornerRadius;
    std::optional<float> opacity;

    bool operator==(const ViewStyle&) const = default;
};

enum class ViewTransitionEasing
{
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
};

enum class ViewTransitionProperty
{
    Background,
    Foreground,
    BorderColor,
    Opacity,
    Transform,
    Layout,
};

struct ViewTransition
{
    std::uint32_t durationMilliseconds = 120;
    ViewTransitionEasing easing = ViewTransitionEasing::EaseOut;
    std::vector<ViewTransitionProperty> properties;

    bool operator==(const ViewTransition&) const = default;
};

struct ViewShadow
{
    std::uint32_t color = 0x000000;
    std::optional<ViewThemeColorToken> colorToken;
    float blur = 12.0f;
    float offsetX = 0.0f;
    float offsetY = 4.0f;
    float alpha = 0.25f;
};

struct ViewTransform
{
    float translateX = 0.0f;
    float translateY = 0.0f;
    float scale = 1.0f;
    float originX = 0.5f;
    float originY = 0.5f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float rotate = 0.0f;
    float skewX = 0.0f;
    float skewY = 0.0f;

    bool operator==(const ViewTransform&) const = default;
};

struct ViewPresenceTransition
{
    std::uint32_t durationMilliseconds = 120;
    ViewTransitionEasing easing = ViewTransitionEasing::EaseOut;
    std::optional<float> opacity;
    std::optional<ViewTransform> transform;

    bool operator==(const ViewPresenceTransition&) const = default;
};

struct ViewTransitionPresentation
{
    ViewStyle style;
    std::optional<ViewTransform> transform;
    std::optional<ViewRect> layoutFrame;

    bool operator==(const ViewTransitionPresentation&) const = default;
};

struct ViewResolvedTransform
{
    float m11 = 1.0f;
    float m12 = 0.0f;
    float m21 = 0.0f;
    float m22 = 1.0f;
    float dx = 0.0f;
    float dy = 0.0f;

    bool operator==(const ViewResolvedTransform&) const = default;
};

struct ViewGridTrack
{
    ViewGridTrackKind kind = ViewGridTrackKind::Auto;
    float value = 0.0f;
    float minimum = 0.0f;
    ViewGridTrackKind maximumKind = ViewGridTrackKind::Auto;
    float maximumValue = 0.0f;

    bool operator==(const ViewGridTrack&) const = default;
};

struct ViewChoiceOption
{
    std::string key;
    std::string value;
    std::string label;
    bool enabled = true;
};

struct ViewTextSpan
{
    std::string key;
    std::string text;
    std::optional<std::uint32_t> foreground;
    std::optional<ViewThemeColorToken> foregroundToken;
    std::optional<std::uint32_t> hoverForeground;
    std::optional<ViewThemeColorToken> hoverForegroundToken;
    std::optional<std::uint32_t> pressedForeground;
    std::optional<ViewThemeColorToken> pressedForegroundToken;
    std::optional<float> fontSize;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    std::string cursor;
    std::string tooltip;
    std::string accessibilityLabel;
    std::map<std::string, InteractionAction, std::less<>> events;
};

struct ViewMonthCalendarCell
{
    std::string date;
    int day = 0;
    bool currentMonth = false;
    bool selected = false;
    bool today = false;
    bool hasEvent = false;
};

struct ViewNode
{
    ViewNodeType type = ViewNodeType::Box;
    std::string key;
    std::string text;
    std::string inputValue;
    std::string placeholder;
    std::string imageResourceName;
    std::string itemReference;
    std::string fontResourceName;
    std::string alt;
    std::vector<ViewTextSpan> spans;
    ViewLength width{ ViewLengthKind::Fill, 0.0f };
    ViewLength height{};
    std::optional<float> minimumWidth;
    std::optional<float> maximumWidth;
    std::optional<float> minimumHeight;
    std::optional<float> maximumHeight;
    std::optional<float> aspectRatio;
    ViewEdgeInsets margin;
    ViewEdgeInsets padding;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    int zIndex = 0;
    bool clipChildren = false;
    ViewOverflow overflow = ViewOverflow::Visible;
    std::optional<ViewShadow> shadow;
    std::optional<ViewTransform> transform;
    std::optional<ViewTransition> transition;
    std::optional<ViewPresenceTransition> enterTransition;
    std::optional<ViewPresenceTransition> exitTransition;
    float gap = 0.0f;
    std::size_t columns = 1;
    std::vector<ViewGridTrack> columnTracks;
    std::vector<ViewGridTrack> rowTracks;
    std::size_t itemCount = 0;
    std::size_t firstIndex = 0;
    std::size_t overscan = 2;
    float itemExtent = 0.0f;
    ViewSelectionMode selectionMode = ViewSelectionMode::None;
    std::vector<std::string> selectedKeys;
    ViewSelectionMode inheritedSelectionMode = ViewSelectionMode::None;
    std::vector<std::string> inheritedSelectedKeys;
    std::optional<InteractionAction> inheritedSelectionChangeAction;
    std::optional<float> columnGap;
    std::optional<float> rowGap;
    std::optional<std::size_t> gridColumn;
    std::optional<std::size_t> gridRow;
    std::size_t columnSpan = 1;
    std::size_t rowSpan = 1;
    std::size_t resolvedGridColumn = 0;
    std::size_t resolvedGridRow = 0;
    ViewLength flexBasis{};
    float flexGrow = 0.0f;
    float flexShrink = 1.0f;
    ViewFlexDirection flexDirection = ViewFlexDirection::Auto;
    ViewFlexWrap flexWrap = ViewFlexWrap::NoWrap;
    ViewContentAlignment alignContent = ViewContentAlignment::Stretch;
    ViewAlignment alignItems = ViewAlignment::Stretch;
    ViewAlignment alignSelf = ViewAlignment::Auto;
    ViewJustification justifyContent = ViewJustification::Start;
    ViewTextAlignment textAlign = ViewTextAlignment::Start;
    ViewAlignment verticalAlign = ViewAlignment::Center;
    ViewTextWrap textWrap = ViewTextWrap::NoWrap;
    ViewTextOverflow overflowText = ViewTextOverflow::Ellipsis;
    std::size_t maximumLines = 0;
    ViewImageFit imageFit = ViewImageFit::Contain;
    ViewImageAlignment imageAlignment = ViewImageAlignment::Center;
    ViewImageInterpolation imageInterpolation =
        ViewImageInterpolation::Linear;
    std::optional<std::uint32_t> imageTint;
    std::optional<ViewThemeColorToken> imageTintToken;
    ViewOrientation orientation = ViewOrientation::Horizontal;
    ViewShapeKind shapeKind = ViewShapeKind::Rectangle;
    ViewIconFont iconFont = ViewIconFont::FontAwesome;
    float fontSize = 15.0f;
    std::size_t fontWeight = 0;
    ViewFontStyle fontStyle = ViewFontStyle::Normal;
    std::string locale;
    ViewTextDirection textDirection = ViewTextDirection::Auto;
    std::optional<float> lineHeight;
    float letterSpacing = 0.0f;
    float value = 0.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float step = 0.01f;
    float thickness = 4.0f;
    float trackOpacity = 1.0f;
    float fillOpacity = 1.0f;
    std::vector<float> values;
    std::optional<float> seriesMinimum;
    std::optional<float> seriesMaximum;
    bool bold = false;
    bool checked = false;
    bool indeterminate = false;
    bool selected = false;
    bool expanded = false;
    bool selectAll = false;
    std::optional<ViewTextSelection> textSelection;
    bool liveUpdate = true;
    bool readOnly = false;
    bool required = false;
    bool busy = false;
    ViewValidationState validationState = ViewValidationState::None;
    std::string validationMessage;
    std::size_t maximumUtf8Bytes = 0;
    bool showScrollbar = true;
    std::string selectedValue;
    std::vector<ViewChoiceOption> options;
    int calendarYear = 0;
    int calendarMonth = 0;
    int firstDayOfWeek = 1;
    std::string calendarSelectedDate;
    std::string calendarTodayDate;
    std::vector<std::string> calendarEventDates;
    std::array<std::string, 7> weekdayLabels{};
    bool showAdjacentDates = true;
    std::string logicalSlotId;
    LogicalSlotKind logicalSlotKind = LogicalSlotKind::Binding;
    std::uint64_t logicalSlotRevision = 0;
    std::string logicalSlotReference;
    bool visible = true;
    ViewVisibility visibility = ViewVisibility::Visible;
    bool enabled = true;
    std::optional<bool> focusable;
    std::optional<int> tabIndex;
    ViewCollectionContent collectionContent = ViewCollectionContent::Items;
    std::string cursor;
    std::string tooltip;
    std::string accessKey;
    std::string acceleratorText;
    std::string accessibilityRole;
    std::string accessibilityLabel;
    ViewStyle style;
    ViewStyle hoverStyle;
    ViewStyle pressedStyle;
    ViewStyle focusStyle;
    ViewStyle disabledStyle;
    ViewStyle validationStyle;
    ViewStyle checkedStyle;
    ViewStyle selectedStyle;
    ViewStyle todayStyle;
    ViewStyle adjacentStyle;
    ViewStyle eventStyle;
    std::map<std::string, InteractionAction, std::less<>> events;
    std::vector<ViewNode> children;
    ViewRect frame;
    // Parent-relative frame captured before scroll offsets are applied. This
    // keeps scrolling separate from opt-in layout transitions.
    ViewRect layoutTransitionFrame;
    std::optional<ViewRect> clipFrame;
    float scrollOffset = 0.0f;
    float scrollViewportExtent = 0.0f;
    float scrollContentExtent = 0.0f;
};

struct ViewScrollViewport
{
    std::string key;
    ViewRect frame;
    ViewOrientation orientation = ViewOrientation::Vertical;
    float viewportExtent = 0.0f;
    float contentExtent = 0.0f;
    float offset = 0.0f;
    float maximum = 0.0f;
};

struct ViewInputControl
{
    ViewNodeType type = ViewNodeType::TextInput;
    std::string key;
    std::string value;
    std::string placeholder;
    ViewRect frame;
    std::optional<ViewRect> clip;
    float fontSize = 15.0f;
    ViewEdgeInsets padding{ 8.0f };
    bool enabled = true;
    bool focusable = true;
    bool readOnly = false;
    bool selectAll = false;
    std::optional<ViewTextSelection> selection;
    bool liveUpdate = true;
    std::size_t maximumUtf8Bytes = 0;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float step = 0.01f;
    InteractionAction changeAction;
    InteractionAction selectionChangeAction;
    InteractionAction focusAction;
    InteractionAction blurAction;
    InteractionAction submitAction;
};

struct ViewVirtualRange
{
    std::size_t firstIndex = 0;
    std::size_t lastIndex = 0;
    float offset = 0.0f;
    float maximum = 0.0f;
    float viewportExtent = 0.0f;
    float contentExtent = 0.0f;
};

using ViewScrollOffsetResolver = std::function<float(
    std::string_view key, float maximum)>;

struct ViewExitTransitionFrame
{
    const ViewNode* node = nullptr;
    ViewResolvedTransform parentTransform;
    std::optional<ViewRect> parentClip;
    ViewTransitionPresentation presentation;
};

class ViewTransitionRuntime
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void BeginFrame() noexcept;
    ViewTransitionPresentation ResolvePresentation(std::string_view key,
        const ViewStyle& targetStyle,
        const std::optional<ViewTransform>& targetTransform,
        const std::optional<ViewRect>& targetLayoutFrame,
        const std::optional<ViewTransition>& transition,
        const std::optional<ViewPresenceTransition>& enterTransition,
        TimePoint now, bool reducedMotion);
    ViewStyle Resolve(std::string_view key, const ViewStyle& target,
        const std::optional<ViewTransition>& transition,
        TimePoint now, bool reducedMotion);
    void QueueExitTransitions(const ViewNode& previous,
        const ViewNode& current, TimePoint now, bool reducedMotion);
    std::vector<ViewExitTransitionFrame> ExitFrames(
        TimePoint now, bool reducedMotion);
    void EndFrame();
    bool Tick(TimePoint now) noexcept;
    bool HasActive() const noexcept;
    void Clear() noexcept;
    std::size_t Size() const noexcept;

private:
    struct Entry
    {
        ViewTransitionPresentation start;
        ViewTransitionPresentation target;
        ViewTransition configuredTransition;
        ViewTransition activeTransition;
        TimePoint started{};
        std::uint64_t generation = 0;
        bool active = false;
        bool entering = false;
    };

    struct ExitEntry
    {
        ViewNode node;
        ViewResolvedTransform parentTransform;
        std::optional<ViewRect> parentClip;
        ViewTransitionPresentation start;
        ViewTransitionPresentation target;
        ViewTransition transition;
        TimePoint started{};
        std::size_t nodeCount = 0;
    };

    static ViewTransitionPresentation CurrentPresentation(
        const Entry& entry, TimePoint now) noexcept;

    std::unordered_map<std::string, Entry> entries_;
    std::vector<ExitEntry> exits_;
    std::uint64_t generation_ = 0;
};

std::uint32_t ResolveViewThemeColor(ViewThemeColorToken token,
    const ViewThemePalette& palette) noexcept;
std::optional<std::uint32_t> ResolveViewThemeColor(
    const std::optional<std::uint32_t>& literal,
    const std::optional<ViewThemeColorToken>& token,
    const ViewThemePalette& palette) noexcept;
ViewStyle ResolveViewThemeStyle(const ViewStyle& style,
    const ViewThemePalette& palette) noexcept;

struct ViewTreeLimits
{
    static constexpr std::size_t MaximumNodes = 512;
    static constexpr std::size_t MaximumDepth = 32;
    static constexpr std::size_t MaximumTextBytes = 4096;
    static constexpr std::size_t MaximumTotalTextBytes = 64 * 1024;
    static constexpr std::size_t MaximumResources = 64;
    static constexpr std::size_t MaximumSeriesPoints = 512;
    static constexpr std::size_t MaximumTotalSeriesPoints = 4096;
    static constexpr std::size_t MaximumChoiceOptions = 64;
    static constexpr std::size_t MaximumTextSpans = 64;
    static constexpr std::size_t MaximumCalendarEventDates = 366;
    static constexpr std::size_t MaximumCollectionItems = 256;
    static constexpr std::size_t MaximumScrollContainers = 32;
    static constexpr std::size_t MaximumVirtualItemCount = 1'000'000;
    static constexpr std::size_t MaximumVirtualWindowItems = 128;
    static constexpr std::size_t MaximumVirtualOverscan = 16;
};

bool ValidateAndLayoutViewTree(ViewNode& root, float width, float height,
    std::string& error);
bool ValidateViewLogicalSlots(const ViewNode& root,
    const LogicalSlotModel& model, std::string& error);
bool CollectViewInteractionRegions(const ViewNode& root,
    std::vector<InteractionRegion>& regions, std::string& error);
bool CollectViewInputControls(const ViewNode& root,
    std::vector<ViewInputControl>& controls, std::string& error);
bool ApplyViewScrollOffsets(ViewNode& root,
    const ViewScrollOffsetResolver& resolver,
    std::vector<ViewScrollViewport>& viewports, std::string& error);
bool ComputeViewVirtualRange(std::size_t itemCount, float itemExtent,
    std::size_t columns, float rowGap, float viewportExtent,
    float requestedOffset, std::size_t overscan,
    ViewVirtualRange& range, std::string& error);
bool ComputeViewVirtualItemScrollOffset(std::size_t itemCount,
    float itemExtent, std::size_t columns, float rowGap,
    float viewportExtent, float currentOffset, std::size_t index,
    std::string_view alignment, float& offset, std::string& error);
ViewRect ViewNodeContentRect(const ViewNode& node) noexcept;
ViewResolvedTransform ResolveViewTransformForKey(
    const ViewNode& root, std::string_view key) noexcept;
ViewResolvedTransform ResolveViewLocalTransform(
    const ViewNode& node) noexcept;
ViewResolvedTransform ResolveViewLocalTransform(const ViewRect& frame,
    const std::optional<ViewTransform>& transform) noexcept;
ViewResolvedTransform ResolveViewPresentationTransform(
    const ViewRect& renderedFrame,
    const ViewRect& targetLayoutFrame,
    const ViewRect& presentedLayoutFrame,
    const std::optional<ViewTransform>& transform) noexcept;
std::optional<ViewRect> ResolveViewClipForKey(
    const ViewNode& root, std::string_view key,
    bool includeMatchedNode) noexcept;
ViewRect ApplyViewTransform(const ViewRect& rect,
    const ViewResolvedTransform& transform) noexcept;
void ApplyViewTransform(const ViewNode& root,
    InteractionRegion& region) noexcept;
std::vector<const ViewNode*> ViewChildrenInPaintOrder(const ViewNode& node);
ViewRect ViewRadioOptionFrame(
    const ViewNode& node, std::size_t optionIndex) noexcept;
ViewRect ViewSelectOptionFrame(const ViewNode& node,
    std::size_t optionIndex, float viewportHeight) noexcept;
ViewRect ViewMonthCalendarWeekdayFrame(
    const ViewNode& node, std::size_t weekdayIndex) noexcept;
ViewRect ViewMonthCalendarCellFrame(
    const ViewNode& node, std::size_t cellIndex) noexcept;
bool BuildViewMonthCalendarCells(const ViewNode& node,
    std::array<ViewMonthCalendarCell, 42>& cells, std::string& error);
const char* ViewNodeTypeName(ViewNodeType type) noexcept;
}
