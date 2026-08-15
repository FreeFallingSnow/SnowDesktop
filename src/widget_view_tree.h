#pragma once

#include "widget_interaction_region.h"
#include "widget_logical_slot.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
};

enum class ViewTextAlignment
{
    Start,
    Center,
    End,
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

enum class ViewOrientation
{
    Horizontal,
    Vertical,
};

struct ViewRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool operator==(const ViewRect&) const = default;
};

struct ViewStyle
{
    std::optional<std::uint32_t> background;
    std::optional<std::uint32_t> foreground;
    std::optional<std::uint32_t> borderColor;
    std::optional<float> borderWidth;
    std::optional<float> cornerRadius;
    std::optional<float> opacity;
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
    std::string text;
    std::optional<std::uint32_t> foreground;
    std::optional<float> fontSize;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
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
    float padding = 0.0f;
    float gap = 0.0f;
    std::size_t columns = 1;
    std::size_t itemCount = 0;
    std::size_t firstIndex = 0;
    std::size_t overscan = 2;
    float itemExtent = 0.0f;
    std::optional<float> columnGap;
    std::optional<float> rowGap;
    float flexGrow = 0.0f;
    ViewAlignment alignItems = ViewAlignment::Stretch;
    ViewAlignment alignSelf = ViewAlignment::Auto;
    ViewJustification justifyContent = ViewJustification::Start;
    ViewTextAlignment textAlign = ViewTextAlignment::Start;
    ViewImageFit imageFit = ViewImageFit::Contain;
    ViewImageAlignment imageAlignment = ViewImageAlignment::Center;
    ViewImageInterpolation imageInterpolation =
        ViewImageInterpolation::Linear;
    ViewOrientation orientation = ViewOrientation::Horizontal;
    ViewShapeKind shapeKind = ViewShapeKind::Rectangle;
    ViewIconFont iconFont = ViewIconFont::FontAwesome;
    float fontSize = 15.0f;
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
    bool expanded = false;
    bool selectAll = false;
    bool liveUpdate = true;
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
    bool enabled = true;
    std::string cursor;
    std::string accessibilityRole;
    std::string accessibilityLabel;
    ViewStyle style;
    ViewStyle hoverStyle;
    ViewStyle pressedStyle;
    ViewStyle checkedStyle;
    ViewStyle selectedStyle;
    ViewStyle todayStyle;
    ViewStyle adjacentStyle;
    ViewStyle eventStyle;
    std::map<std::string, InteractionAction, std::less<>> events;
    std::vector<ViewNode> children;
    ViewRect frame;
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
    float padding = 8.0f;
    bool enabled = true;
    bool selectAll = false;
    bool liveUpdate = true;
    std::size_t maximumUtf8Bytes = 0;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float step = 0.01f;
    InteractionAction changeAction;
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
