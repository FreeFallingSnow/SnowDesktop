#include "widget_view_contract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr auto kContracts = std::to_array<ViewNodeContract>({
    { ViewNodeType::Box, "box", "layout", "view.tree.core", "group", "Group", ViewChildPolicy::Any },
    { ViewNodeType::Row, "row", "layout", "view.tree.core", "group", "Group", ViewChildPolicy::Any },
    { ViewNodeType::Column, "column", "layout", "view.tree.core", "group", "Group", ViewChildPolicy::Any },
    { ViewNodeType::Grid, "grid", "layout", "view.grid.uniform", "group", "Group", ViewChildPolicy::Any, ViewAccessibilityPattern::Grid },
    { ViewNodeType::Flow, "flow", "layout", "view.flow.wrap", "group", "Group", ViewChildPolicy::Any },
    { ViewNodeType::Stack, "stack", "layout", "view.tree.core", "group", "Group", ViewChildPolicy::Any },
    { ViewNodeType::Scroll, "scroll", "layout", "view.scroll", "group", "Pane", ViewChildPolicy::Single, ViewAccessibilityPattern::Scroll },
    { ViewNodeType::List, "list", "collection", "view.collection.basic", "list", "List", ViewChildPolicy::Collection, ViewAccessibilityPattern::Selection },
    { ViewNodeType::GridList, "gridList", "collection", "view.collection.basic", "grid", "DataGrid", ViewChildPolicy::Collection, ViewAccessibilityPattern::Grid | ViewAccessibilityPattern::Selection },
    { ViewNodeType::VirtualList, "virtualList", "collection", "view.collection.virtual", "list", "List", ViewChildPolicy::Collection, ViewAccessibilityPattern::Scroll | ViewAccessibilityPattern::Selection },
    { ViewNodeType::VirtualGrid, "virtualGrid", "collection", "view.collection.virtual", "grid", "DataGrid", ViewChildPolicy::Collection, ViewAccessibilityPattern::Scroll | ViewAccessibilityPattern::Grid | ViewAccessibilityPattern::Selection },
    { ViewNodeType::ListItem, "listItem", "collection", "view.collection.basic", "listitem", "ListItem", ViewChildPolicy::Single, ViewAccessibilityPattern::Invoke | ViewAccessibilityPattern::SelectionItem, true },
    { ViewNodeType::Text, "text", "content", "view.tree.core", "text", "Text", ViewChildPolicy::None },
    { ViewNodeType::StyledText, "styledText", "content", "view.styledText.basic", "text", "Text", ViewChildPolicy::None },
    { ViewNodeType::TextInput, "textInput", "input", "view.inputControls", "textbox", "Edit", ViewChildPolicy::None, ViewAccessibilityPattern::Value, true },
    { ViewNodeType::TextArea, "textArea", "input", "view.inputControls", "textbox", "Edit", ViewChildPolicy::None, ViewAccessibilityPattern::Value, true },
    { ViewNodeType::SearchBox, "searchBox", "input", "view.inputControls", "searchbox", "Edit", ViewChildPolicy::None, ViewAccessibilityPattern::Value, true },
    { ViewNodeType::NumberInput, "numberInput", "input", "view.inputControls", "spinbutton", "Spinner", ViewChildPolicy::None, ViewAccessibilityPattern::Value | ViewAccessibilityPattern::RangeValue, true },
    { ViewNodeType::Select, "select", "input", "view.inputControls", "combobox", "ComboBox", ViewChildPolicy::None, ViewAccessibilityPattern::ExpandCollapse | ViewAccessibilityPattern::Selection, true },
    { ViewNodeType::Image, "image", "content", "view.image", "img", "Image", ViewChildPolicy::None },
    { ViewNodeType::ReferenceIcon, "referenceIcon", "content", "view.referenceIcon", "img", "Image", ViewChildPolicy::None },
    { ViewNodeType::Button, "button", "action", "view.tree.core", "button", "Button", ViewChildPolicy::None, ViewAccessibilityPattern::Invoke, true },
    { ViewNodeType::Link, "link", "action", "view.actionControls", "link", "Hyperlink", ViewChildPolicy::None, ViewAccessibilityPattern::Invoke, true },
    { ViewNodeType::Toggle, "toggle", "action", "view.selectionControls", "switch", "Button", ViewChildPolicy::None, ViewAccessibilityPattern::Toggle, true },
    { ViewNodeType::Checkbox, "checkbox", "action", "view.selectionControls", "checkbox", "CheckBox", ViewChildPolicy::None, ViewAccessibilityPattern::Toggle, true },
    { ViewNodeType::RadioGroup, "radioGroup", "action", "view.actionControls", "radiogroup", "Group", ViewChildPolicy::None, ViewAccessibilityPattern::Selection, true },
    { ViewNodeType::Slider, "slider", "action", "view.actionControls", "slider", "Slider", ViewChildPolicy::None, ViewAccessibilityPattern::RangeValue, true },
    { ViewNodeType::Icon, "icon", "content", "view.tree.core", "img", "Image", ViewChildPolicy::None },
    { ViewNodeType::IconButton, "iconButton", "action", "view.tree.core", "button", "Button", ViewChildPolicy::None, ViewAccessibilityPattern::Invoke, true },
    { ViewNodeType::Shape, "shape", "content", "view.tree.core", "img", "Image", ViewChildPolicy::None },
    { ViewNodeType::Badge, "badge", "status", "view.statusVisuals", "status", "Text", ViewChildPolicy::None },
    { ViewNodeType::Divider, "divider", "layout", "view.statusVisuals", "separator", "Separator", ViewChildPolicy::None },
    { ViewNodeType::ProgressBar, "progressBar", "status", "view.tree.core", "progressbar", "ProgressBar", ViewChildPolicy::None, ViewAccessibilityPattern::RangeValue },
    { ViewNodeType::ProgressRing, "progressRing", "status", "view.tree.core", "progressbar", "ProgressBar", ViewChildPolicy::None, ViewAccessibilityPattern::RangeValue },
    { ViewNodeType::Meter, "meter", "status", "view.statusVisuals", "meter", "ProgressBar", ViewChildPolicy::None, ViewAccessibilityPattern::RangeValue },
    { ViewNodeType::Sparkline, "sparkline", "data", "view.dataSeries", "img", "Image", ViewChildPolicy::None },
    { ViewNodeType::LineChart, "lineChart", "data", "view.dataSeries", "img", "Image", ViewChildPolicy::None },
    { ViewNodeType::BarChart, "barChart", "data", "view.dataSeries", "img", "Image", ViewChildPolicy::None },
    { ViewNodeType::Waveform, "waveform", "data", "view.dataSeries", "img", "Image", ViewChildPolicy::None },
    { ViewNodeType::Spectrum, "spectrum", "data", "view.dataSeries", "img", "Image", ViewChildPolicy::None },
    { ViewNodeType::MonthCalendar, "monthCalendar", "date", "view.monthCalendar", "grid", "Calendar", ViewChildPolicy::None, ViewAccessibilityPattern::Grid | ViewAccessibilityPattern::Selection, true },
    { ViewNodeType::SlotSurface, "slotSurface", "slot", "view.logicalSlots", "group", "Group", ViewChildPolicy::LogicalSlot, ViewAccessibilityPattern::Selection },
    { ViewNodeType::SlotItem, "slotItem", "slot", "view.logicalSlots", "listitem", "ListItem", ViewChildPolicy::Single, ViewAccessibilityPattern::Invoke | ViewAccessibilityPattern::SelectionItem, true },
    { ViewNodeType::Spacer, "spacer", "layout", "view.tree.core", "", "", ViewChildPolicy::None },
});

constexpr auto kValidationDiagnostics =
    std::to_array<ViewValidationDiagnosticContract>({
        { "view.lifecycle", "lifecycle" },
        { "view.evaluate", "callback" },
        { "view.parse", "parse" },
        { "view.layout", "validation-layout" },
        { "view.slots", "logical-slots" },
        { "view.scroll", "scroll-state" },
        { "view.interaction", "interaction-regions" },
        { "view.styledText", "styled-text-hit-testing" },
        { "view.input", "host-input-controls" },
        { "view.hostControls", "host-control-commit" },
    });

constexpr auto NodeTypeNames() noexcept
{
    std::array<std::string_view, kContracts.size()> result{};
    for (std::size_t index = 0; index < kContracts.size(); ++index)
        result[index] = kContracts[index].name;
    return result;
}

constexpr auto kNodeTypeNames = NodeTypeNames();
constexpr auto kIconFontValues = std::to_array<std::string_view>({
    "fa", "fluent", "fluent-regular" });
constexpr auto kImageFitValues = std::to_array<std::string_view>({
    "contain", "fill", "cover", "none" });
constexpr auto kImageAlignmentValues = std::to_array<std::string_view>({
    "center", "start", "end" });
constexpr auto kImageInterpolationValues = std::to_array<std::string_view>({
    "linear", "nearest" });
constexpr auto kShapeValues = std::to_array<std::string_view>({
    "rectangle", "roundedRectangle", "circle", "ellipse" });
constexpr auto kOrientationValues = std::to_array<std::string_view>({
    "horizontal", "vertical" });
constexpr auto kValidationStateValues = std::to_array<std::string_view>({
    "none", "info", "success", "warning", "error" });
constexpr auto kOverflowValues = std::to_array<std::string_view>({
    "visible", "clip" });
constexpr auto kSelectionModeValues = std::to_array<std::string_view>({
    "none", "single", "multiple" });
constexpr auto kFlexDirectionValues = std::to_array<std::string_view>({
    "row", "rowReverse", "column", "columnReverse" });
constexpr auto kFlexWrapValues = std::to_array<std::string_view>({
    "noWrap", "wrap", "wrapReverse" });
constexpr auto kContentAlignmentValues = std::to_array<std::string_view>({
    "start", "center", "end", "stretch", "spaceBetween",
    "spaceAround", "spaceEvenly" });
constexpr auto kFontStyleValues = std::to_array<std::string_view>({
    "normal", "italic" });
constexpr auto kTextDirectionValues = std::to_array<std::string_view>({
    "auto", "ltr", "rtl" });
constexpr auto kVisibilityValues = std::to_array<std::string_view>({
    "visible", "hidden", "collapsed" });
constexpr auto kAlignmentValues = std::to_array<std::string_view>({
    "start", "center", "end", "stretch" });
constexpr auto kSelfAlignmentValues = std::to_array<std::string_view>({
    "auto", "start", "center", "end", "stretch" });
constexpr auto kJustificationValues = std::to_array<std::string_view>({
    "start", "center", "end", "spaceBetween", "spaceAround",
    "spaceEvenly" });
constexpr auto kTextAlignmentValues = std::to_array<std::string_view>({
    "start", "center", "end" });
constexpr auto kVerticalAlignmentValues = std::to_array<std::string_view>({
    "start", "center", "end" });
constexpr auto kTextWrapValues = std::to_array<std::string_view>({
    "noWrap", "wrap" });
constexpr auto kTextOverflowValues = std::to_array<std::string_view>({
    "ellipsis", "clip" });

constexpr bool IsNamed(std::string_view name,
    std::initializer_list<std::string_view> values) noexcept
{
    return std::find(values.begin(), values.end(), name) != values.end();
}

constexpr ViewPropertyEffect PropertyEffects(std::string_view name,
    ViewPropertyValueKind kind) noexcept
{
    ViewPropertyEffect effects = ViewPropertyEffect::None;
    const auto add = [&effects](ViewPropertyEffect value) {
        effects = effects | value;
    };
    switch (kind)
    {
    case ViewPropertyValueKind::Length:
    case ViewPropertyValueKind::EdgeInsets:
    case ViewPropertyValueKind::Offset:
    case ViewPropertyValueKind::GridTracks:
        add(ViewPropertyEffect::Layout | ViewPropertyEffect::Paint |
            ViewPropertyEffect::HitTest);
        break;
    case ViewPropertyValueKind::Resource:
        add(ViewPropertyEffect::Layout | ViewPropertyEffect::Paint |
            ViewPropertyEffect::Resource);
        break;
    case ViewPropertyValueKind::Color:
    case ViewPropertyValueKind::Style:
    case ViewPropertyValueKind::Shadow:
    case ViewPropertyValueKind::Transition:
    case ViewPropertyValueKind::PresenceTransition:
        add(ViewPropertyEffect::Paint);
        break;
    case ViewPropertyValueKind::Transform:
        add(ViewPropertyEffect::Paint | ViewPropertyEffect::HitTest |
            ViewPropertyEffect::Accessibility);
        break;
    case ViewPropertyValueKind::Node:
    case ViewPropertyValueKind::NodeArray:
        add(ViewPropertyEffect::Tree | ViewPropertyEffect::Layout |
            ViewPropertyEffect::Paint | ViewPropertyEffect::HitTest |
            ViewPropertyEffect::Accessibility);
        break;
    case ViewPropertyValueKind::Spans:
    case ViewPropertyValueKind::ChoiceOptions:
        add(ViewPropertyEffect::Layout | ViewPropertyEffect::Paint |
            ViewPropertyEffect::HitTest | ViewPropertyEffect::Input |
            ViewPropertyEffect::Accessibility);
        break;
    case ViewPropertyValueKind::TextSelection:
        add(ViewPropertyEffect::Paint | ViewPropertyEffect::Input |
            ViewPropertyEffect::Accessibility);
        break;
    case ViewPropertyValueKind::Tooltip:
        add(ViewPropertyEffect::HitTest |
            ViewPropertyEffect::Accessibility);
        break;
    case ViewPropertyValueKind::Accessibility:
        add(ViewPropertyEffect::Accessibility);
        break;
    case ViewPropertyValueKind::Events:
    case ViewPropertyValueKind::Action:
        add(ViewPropertyEffect::HitTest | ViewPropertyEffect::Input);
        break;
    case ViewPropertyValueKind::NumberArray:
        add(ViewPropertyEffect::Paint |
            ViewPropertyEffect::Accessibility);
        break;
    case ViewPropertyValueKind::StringArray:
        add(ViewPropertyEffect::Layout | ViewPropertyEffect::Paint |
            ViewPropertyEffect::Accessibility);
        break;
    case ViewPropertyValueKind::IndexArray:
        add(ViewPropertyEffect::Tree | ViewPropertyEffect::Layout |
            ViewPropertyEffect::Paint);
        break;
    default:
        break;
    }

    if (IsNamed(name, { "type", "key", "binding", "collection",
            "revision", "reference", "layoutRevision", "firstIndex",
            "overscan", "initialScrollKey", "initialScrollIndex" }))
        add(ViewPropertyEffect::Tree);
    if (IsNamed(name, { "text", "label", "glyph", "iconFont", "fit",
            "alignment", "interpolation", "shape", "orientation",
            "value", "min", "max", "step", "selectedValue",
            "placeholder", "expanded", "validationMessage", "year",
            "month", "firstDayOfWeek", "selectedDate", "todayDate",
            "showAdjacentDates", "reference", "thickness",
            "trackOpacity", "fillOpacity", "minWidth", "maxWidth",
            "minHeight", "maxHeight", "aspectRatio", "zIndex", "clip",
            "overflow", "gap", "columnGap", "rowGap", "gridColumn",
            "gridRow", "columnSpan", "rowSpan", "itemCount",
            "itemExtent", "estimatedItemSize", "layoutRevision",
            "stickyHeaderIndex", "firstIndex", "overscan",
            "initialScrollKey", "initialScrollIndex", "selectionMode",
            "flexGrow", "flexShrink", "flexDirection", "flexWrap",
            "alignContent", "fontSize", "fontWeight", "fontStyle",
            "lineHeight", "letterSpacing", "locale", "textDirection",
            "bold", "sticky", "visible", "visibility", "alignItems",
            "showScrollbar", "alignSelf", "justifyContent", "textAlign",
            "verticalAlign", "textWrap", "maxLines", "overflowText" }))
        add(ViewPropertyEffect::Layout);
    if (IsNamed(name, { "text", "label", "glyph", "iconFont", "fit",
            "alignment", "interpolation", "shape", "orientation",
            "value", "min", "max", "step", "selectedValue",
            "placeholder", "expanded", "readOnly", "required", "busy",
            "validationState", "validationMessage", "year", "month",
            "firstDayOfWeek", "selectedDate", "todayDate",
            "showAdjacentDates", "reference", "thickness",
            "trackOpacity", "fillOpacity", "clip", "overflow", "zIndex",
            "gap", "columnGap", "rowGap", "itemExtent", "firstIndex",
            "selectionMode", "fontSize", "fontWeight", "fontStyle",
            "lineHeight", "letterSpacing", "textDirection", "bold",
            "checked", "indeterminate", "selected", "sticky", "visible",
            "visibility", "enabled", "focusable", "cursor",
            "showScrollbar", "textAlign", "verticalAlign", "textWrap",
            "maxLines", "overflowText" }))
        add(ViewPropertyEffect::Paint);
    if (IsNamed(name, { "value", "min", "max", "step", "selectedValue",
            "expanded", "selectAll", "liveUpdate", "readOnly", "required",
            "maxBytes", "checked", "indeterminate", "selected", "enabled",
            "focusable", "tabIndex", "cursor", "capturePointer",
            "accessKey", "selectionMode", "initialScrollKey",
            "initialScrollIndex" }))
        add(ViewPropertyEffect::Input);
    if (IsNamed(name, { "expanded", "readOnly", "visible", "visibility",
            "enabled", "focusable", "tabIndex", "cursor",
            "capturePointer", "accessKey", "zIndex", "clip", "overflow",
            "showScrollbar" }))
        add(ViewPropertyEffect::HitTest);
    if (IsNamed(name, { "debugName", "testId", "text", "label", "glyph",
            "alt", "value", "min", "max", "step", "selectedValue",
            "placeholder", "expanded", "readOnly", "required", "busy",
            "validationState", "validationMessage", "year", "month",
            "selectedDate", "todayDate", "showAdjacentDates", "reference",
            "selectionMode", "fontSize", "locale", "textDirection", "bold",
            "checked", "indeterminate", "selected", "sticky", "visible",
            "visibility", "enabled", "focusable", "tabIndex", "accessKey",
            "acceleratorText" }))
        add(ViewPropertyEffect::Accessibility);
    if (name == "reference") add(ViewPropertyEffect::Resource);
    return effects;
}

constexpr ViewPropertyContract Property(
    std::string_view name, ViewPropertyValueKind valueKind) noexcept
{
    const ViewPropertyEffect effects = PropertyEffects(name, valueKind);
    ViewPropertyTransitionEffect transitionEffects =
        ViewPropertyTransitionEffect::None;
    if (HasViewPropertyEffect(effects, ViewPropertyEffect::Layout) &&
        !IsNamed(name, { "type", "key", "debugName", "testId" }))
    {
        transitionEffects = transitionEffects |
            ViewPropertyTransitionEffect::Layout;
    }
    if (name == "transform")
    {
        transitionEffects = transitionEffects |
            ViewPropertyTransitionEffect::Transform;
    }
    if (IsNamed(name, { "style", "hoverStyle", "pressedStyle",
            "focusStyle", "disabledStyle", "validationStyle",
            "checkedStyle", "selectedStyle", "selected", "checked",
            "indeterminate", "enabled", "validationState" }))
    {
        transitionEffects = transitionEffects |
            ViewPropertyTransitionEffect::Visual;
    }
    return { name, valueKind, ViewPropertyEnumSet::None,
        effects, transitionEffects };
}

constexpr ViewPropertyContract EnumProperty(std::string_view name,
    ViewPropertyEnumSet enumSet) noexcept
{
    ViewPropertyContract contract =
        Property(name, ViewPropertyValueKind::Enum);
    contract.enumSet = enumSet;
    return contract;
}

constexpr ViewPropertyContract RangedProperty(std::string_view name,
    ViewPropertyValueKind valueKind, double minimum,
    double maximum) noexcept
{
    ViewPropertyContract contract = Property(name, valueKind);
    contract.hasNumericRange = true;
    contract.numericMinimum = minimum;
    contract.numericMaximum = maximum;
    return contract;
}

constexpr auto kProperties = std::to_array<ViewPropertyContract>({
    EnumProperty("type", ViewPropertyEnumSet::NodeType),
    Property("key", ViewPropertyValueKind::String),
    Property("debugName", ViewPropertyValueKind::String),
    Property("testId", ViewPropertyValueKind::String),
    Property("text", ViewPropertyValueKind::String),
    Property("spans", ViewPropertyValueKind::Spans),
    Property("label", ViewPropertyValueKind::String),
    Property("glyph", ViewPropertyValueKind::String),
    EnumProperty("iconFont", ViewPropertyEnumSet::IconFont),
    Property("source", ViewPropertyValueKind::Resource),
    Property("font", ViewPropertyValueKind::Resource),
    EnumProperty("fit", ViewPropertyEnumSet::ImageFit),
    EnumProperty("alignment", ViewPropertyEnumSet::ImageAlignment),
    EnumProperty("interpolation", ViewPropertyEnumSet::ImageInterpolation),
    Property("alt", ViewPropertyValueKind::String),
    EnumProperty("shape", ViewPropertyEnumSet::Shape),
    EnumProperty("orientation", ViewPropertyEnumSet::Orientation),
    Property("value", ViewPropertyValueKind::StringOrNumber),
    Property("values", ViewPropertyValueKind::NumberArray),
    Property("min", ViewPropertyValueKind::Number),
    Property("max", ViewPropertyValueKind::Number),
    RangedProperty("step", ViewPropertyValueKind::Number,
        0.000001, 1.0e9),
    Property("options", ViewPropertyValueKind::ChoiceOptions),
    Property("selectedValue", ViewPropertyValueKind::String),
    Property("placeholder", ViewPropertyValueKind::String),
    Property("expanded", ViewPropertyValueKind::Boolean),
    Property("selectAll", ViewPropertyValueKind::Boolean),
    Property("selection", ViewPropertyValueKind::TextSelection),
    Property("liveUpdate", ViewPropertyValueKind::Boolean),
    Property("readOnly", ViewPropertyValueKind::Boolean),
    Property("required", ViewPropertyValueKind::Boolean),
    Property("busy", ViewPropertyValueKind::Boolean),
    EnumProperty("validationState", ViewPropertyEnumSet::ValidationState),
    Property("validationMessage", ViewPropertyValueKind::String),
    RangedProperty("maxBytes", ViewPropertyValueKind::Integer,
        0.0, 65536.0),
    RangedProperty("year", ViewPropertyValueKind::Integer,
        1.0, 9999.0),
    RangedProperty("month", ViewPropertyValueKind::Integer,
        1.0, 12.0),
    RangedProperty("firstDayOfWeek", ViewPropertyValueKind::Integer,
        1.0, 7.0),
    Property("selectedDate", ViewPropertyValueKind::String),
    Property("todayDate", ViewPropertyValueKind::String),
    Property("eventDates", ViewPropertyValueKind::StringArray),
    Property("weekdayLabels", ViewPropertyValueKind::StringArray),
    Property("showAdjacentDates", ViewPropertyValueKind::Boolean),
    Property("binding", ViewPropertyValueKind::String),
    Property("collection", ViewPropertyValueKind::String),
    Property("revision", ViewPropertyValueKind::Integer),
    Property("reference", ViewPropertyValueKind::String),
    Property("child", ViewPropertyValueKind::Node),
    RangedProperty("thickness", ViewPropertyValueKind::Number,
        0.5, 4096.0),
    RangedProperty("trackOpacity", ViewPropertyValueKind::Number,
        0.0, 1.0),
    RangedProperty("fillOpacity", ViewPropertyValueKind::Number,
        0.0, 1.0),
    Property("width", ViewPropertyValueKind::Length),
    Property("height", ViewPropertyValueKind::Length),
    RangedProperty("minWidth", ViewPropertyValueKind::Number,
        0.0, 100000.0),
    RangedProperty("maxWidth", ViewPropertyValueKind::Number,
        0.0, 100000.0),
    RangedProperty("minHeight", ViewPropertyValueKind::Number,
        0.0, 100000.0),
    RangedProperty("maxHeight", ViewPropertyValueKind::Number,
        0.0, 100000.0),
    RangedProperty("aspectRatio", ViewPropertyValueKind::Number,
        0.01, 100.0),
    Property("margin", ViewPropertyValueKind::EdgeInsets),
    Property("padding", ViewPropertyValueKind::EdgeInsets),
    Property("offset", ViewPropertyValueKind::Offset),
    RangedProperty("zIndex", ViewPropertyValueKind::Integer,
        -1024.0, 1024.0),
    Property("clip", ViewPropertyValueKind::Boolean),
    EnumProperty("overflow", ViewPropertyEnumSet::Overflow),
    Property("shadow", ViewPropertyValueKind::Shadow),
    Property("transform", ViewPropertyValueKind::Transform),
    Property("transition", ViewPropertyValueKind::Transition),
    Property("enterTransition", ViewPropertyValueKind::PresenceTransition),
    Property("exitTransition", ViewPropertyValueKind::PresenceTransition),
    RangedProperty("gap", ViewPropertyValueKind::Number,
        0.0, 4096.0),
    Property("columns", ViewPropertyValueKind::GridTracks),
    Property("rows", ViewPropertyValueKind::GridTracks),
    RangedProperty("columnGap", ViewPropertyValueKind::Number,
        0.0, 4096.0),
    RangedProperty("rowGap", ViewPropertyValueKind::Number,
        0.0, 4096.0),
    RangedProperty("gridColumn", ViewPropertyValueKind::Integer,
        1.0, 64.0),
    RangedProperty("gridRow", ViewPropertyValueKind::Integer,
        1.0, 64.0),
    RangedProperty("columnSpan", ViewPropertyValueKind::Integer,
        1.0, 64.0),
    RangedProperty("rowSpan", ViewPropertyValueKind::Integer,
        1.0, 64.0),
    Property("itemCount", ViewPropertyValueKind::Integer),
    RangedProperty("itemExtent", ViewPropertyValueKind::Number,
        0.000001, 1000000.0),
    RangedProperty("estimatedItemSize", ViewPropertyValueKind::Number,
        0.000001, 1000000.0),
    Property("layoutRevision", ViewPropertyValueKind::Integer),
    Property("sectionHeaderIndices", ViewPropertyValueKind::IndexArray),
    Property("stickyHeaderIndex", ViewPropertyValueKind::Integer),
    Property("firstIndex", ViewPropertyValueKind::Integer),
    RangedProperty("overscan", ViewPropertyValueKind::Integer,
        0.0, 16.0),
    Property("initialScrollKey", ViewPropertyValueKind::String),
    Property("initialScrollIndex", ViewPropertyValueKind::Integer),
    EnumProperty("selectionMode", ViewPropertyEnumSet::SelectionMode),
    Property("selectedKeys", ViewPropertyValueKind::StringArray),
    Property("emptyContent", ViewPropertyValueKind::Node),
    Property("loadingContent", ViewPropertyValueKind::Node),
    Property("flexBasis", ViewPropertyValueKind::Length),
    RangedProperty("flexGrow", ViewPropertyValueKind::Number,
        0.0, 1000.0),
    RangedProperty("flexShrink", ViewPropertyValueKind::Number,
        0.0, 1000.0),
    EnumProperty("flexDirection", ViewPropertyEnumSet::FlexDirection),
    EnumProperty("flexWrap", ViewPropertyEnumSet::FlexWrap),
    EnumProperty("alignContent", ViewPropertyEnumSet::ContentAlignment),
    RangedProperty("fontSize", ViewPropertyValueKind::Number,
        1.0, 512.0),
    RangedProperty("fontWeight", ViewPropertyValueKind::Integer,
        0.0, 900.0),
    EnumProperty("fontStyle", ViewPropertyEnumSet::FontStyle),
    RangedProperty("lineHeight", ViewPropertyValueKind::Number,
        1.0, 1024.0),
    RangedProperty("letterSpacing", ViewPropertyValueKind::Number,
        -64.0, 256.0),
    Property("locale", ViewPropertyValueKind::String),
    EnumProperty("textDirection", ViewPropertyEnumSet::TextDirection),
    Property("bold", ViewPropertyValueKind::Boolean),
    Property("checked", ViewPropertyValueKind::Boolean),
    Property("indeterminate", ViewPropertyValueKind::Boolean),
    Property("selected", ViewPropertyValueKind::Boolean),
    Property("sticky", ViewPropertyValueKind::Boolean),
    Property("visible", ViewPropertyValueKind::Boolean),
    EnumProperty("visibility", ViewPropertyEnumSet::Visibility),
    Property("enabled", ViewPropertyValueKind::Boolean),
    Property("focusable", ViewPropertyValueKind::Boolean),
    RangedProperty("tabIndex", ViewPropertyValueKind::Integer,
        -1.0, 32767.0),
    Property("cursor", ViewPropertyValueKind::String),
    Property("tooltip", ViewPropertyValueKind::Tooltip),
    Property("capturePointer", ViewPropertyValueKind::Boolean),
    Property("accessKey", ViewPropertyValueKind::String),
    Property("acceleratorText", ViewPropertyValueKind::String),
    EnumProperty("alignItems", ViewPropertyEnumSet::Alignment),
    Property("showScrollbar", ViewPropertyValueKind::Boolean),
    EnumProperty("alignSelf", ViewPropertyEnumSet::SelfAlignment),
    EnumProperty("justifyContent", ViewPropertyEnumSet::Justification),
    EnumProperty("textAlign", ViewPropertyEnumSet::TextAlignment),
    EnumProperty("verticalAlign", ViewPropertyEnumSet::VerticalAlignment),
    EnumProperty("textWrap", ViewPropertyEnumSet::TextWrap),
    RangedProperty("maxLines", ViewPropertyValueKind::Integer,
        0.0, 64.0),
    EnumProperty("overflowText", ViewPropertyEnumSet::TextOverflow),
    Property("style", ViewPropertyValueKind::Style),
    Property("hoverStyle", ViewPropertyValueKind::Style),
    Property("pressedStyle", ViewPropertyValueKind::Style),
    Property("focusStyle", ViewPropertyValueKind::Style),
    Property("disabledStyle", ViewPropertyValueKind::Style),
    Property("validationStyle", ViewPropertyValueKind::Style),
    Property("checkedStyle", ViewPropertyValueKind::Style),
    Property("tint", ViewPropertyValueKind::Color),
    Property("selectedStyle", ViewPropertyValueKind::Style),
    Property("dropStyle", ViewPropertyValueKind::Style),
    Property("todayStyle", ViewPropertyValueKind::Style),
    Property("adjacentStyle", ViewPropertyValueKind::Style),
    Property("eventStyle", ViewPropertyValueKind::Style),
    Property("accessibility", ViewPropertyValueKind::Accessibility),
    Property("events", ViewPropertyValueKind::Events),
    Property("action", ViewPropertyValueKind::Action),
    Property("children", ViewPropertyValueKind::NodeArray),
});

constexpr auto PropertyNames() noexcept
{
    std::array<std::string_view, kProperties.size()> result{};
    for (std::size_t index = 0; index < kProperties.size(); ++index)
        result[index] = kProperties[index].name;
    return result;
}

constexpr auto kPropertyNames = PropertyNames();

constexpr auto kCommonProperties = std::to_array<std::string_view>({
    "type", "key", "debugName", "testId", "width", "height", "minWidth", "maxWidth",
    "minHeight", "maxHeight", "aspectRatio", "margin", "padding",
    "offset", "zIndex", "clip", "overflow", "shadow", "transform",
    "transition", "enterTransition", "exitTransition", "gap",
    "gridColumn", "gridRow", "columnSpan", "rowSpan",
    "flexBasis", "flexGrow", "flexShrink",
    "fontSize", "fontWeight", "fontStyle", "lineHeight", "letterSpacing",
    "bold", "visible", "visibility", "enabled", "busy", "focusable", "tabIndex", "cursor", "tooltip", "alignItems",
    "alignSelf", "justifyContent", "textAlign", "verticalAlign",
    "textWrap", "maxLines", "overflowText", "style", "hoverStyle",
    "pressedStyle", "focusStyle", "disabledStyle", "selectedStyle", "selected", "accessibility",
    "events", "children",
});

constexpr auto kEventContracts = std::to_array<ViewEventContract>({
    { "pointerEnter", ViewEventPayloadKind::Pointer },
    { "pointerLeave", ViewEventPayloadKind::Pointer },
    { "pointerDown", ViewEventPayloadKind::Pointer },
    { "pointerMove", ViewEventPayloadKind::Pointer },
    { "pointerUp", ViewEventPayloadKind::Pointer },
    { "click", ViewEventPayloadKind::Action },
    { "doubleClick", ViewEventPayloadKind::Action },
    { "wheel", ViewEventPayloadKind::Wheel },
    { "contextMenu", ViewEventPayloadKind::Action },
    { "keyDown", ViewEventPayloadKind::Key },
    { "keyUp", ViewEventPayloadKind::Key },
    { "change", ViewEventPayloadKind::Change },
    { "selectionChange", ViewEventPayloadKind::SelectionChange },
    { "focus", ViewEventPayloadKind::Focus },
    { "blur", ViewEventPayloadKind::Focus },
    { "submit", ViewEventPayloadKind::Submit },
    { "scrollEnd", ViewEventPayloadKind::ScrollEnd },
});

template <std::size_t Size>
constexpr bool Contains(const std::array<std::string_view, Size>& values,
    std::string_view value) noexcept
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

constexpr bool IsType(ViewNodeType type,
    std::initializer_list<ViewNodeType> values) noexcept
{
    return std::find(values.begin(), values.end(), type) != values.end();
}

constexpr bool IsInput(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::TextInput, ViewNodeType::TextArea,
        ViewNodeType::SearchBox, ViewNodeType::NumberInput });
}

constexpr bool IsChoice(ViewNodeType type) noexcept
{
    return type == ViewNodeType::RadioGroup || type == ViewNodeType::Select;
}

constexpr bool IsCheck(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Toggle || type == ViewNodeType::Checkbox;
}

constexpr bool IsProgress(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::ProgressBar,
        ViewNodeType::ProgressRing, ViewNodeType::Meter });
}

constexpr bool IsSeries(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::Sparkline, ViewNodeType::LineChart,
        ViewNodeType::BarChart, ViewNodeType::Waveform,
        ViewNodeType::Spectrum });
}

constexpr bool IsStrokedSeries(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::Sparkline,
        ViewNodeType::LineChart, ViewNodeType::Waveform });
}

constexpr bool IsTrackedSeries(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::LineChart,
        ViewNodeType::BarChart, ViewNodeType::Waveform });
}

constexpr bool IsVirtual(ViewNodeType type) noexcept
{
    return type == ViewNodeType::VirtualList ||
        type == ViewNodeType::VirtualGrid;
}

constexpr bool IsGrid(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::Grid, ViewNodeType::GridList,
        ViewNodeType::VirtualGrid });
}

constexpr bool IsLabelNode(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::Button, ViewNodeType::Link,
        ViewNodeType::Toggle, ViewNodeType::Checkbox });
}

constexpr bool IsTextResourceNode(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::Text, ViewNodeType::StyledText,
        ViewNodeType::Badge, ViewNodeType::Button, ViewNodeType::Link,
        ViewNodeType::Toggle, ViewNodeType::Checkbox,
        ViewNodeType::RadioGroup, ViewNodeType::MonthCalendar });
}

constexpr bool IsTextualNode(ViewNodeType type) noexcept
{
    return (IsTextResourceNode(type) &&
            type != ViewNodeType::MonthCalendar) || IsInput(type) ||
        type == ViewNodeType::Select;
}

constexpr bool IsTypographyNode(ViewNodeType type) noexcept
{
    return IsTextualNode(type) ||
        type == ViewNodeType::Icon ||
        type == ViewNodeType::IconButton ||
        type == ViewNodeType::MonthCalendar;
}

constexpr bool IsAdvancedTypographyNode(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::Text, ViewNodeType::StyledText,
        ViewNodeType::Badge, ViewNodeType::Button, ViewNodeType::Link,
        ViewNodeType::Toggle, ViewNodeType::Checkbox,
        ViewNodeType::Icon, ViewNodeType::IconButton });
}

constexpr bool IsBoldTypographyNode(ViewNodeType type) noexcept
{
    return IsAdvancedTypographyNode(type) ||
        type == ViewNodeType::RadioGroup ||
        type == ViewNodeType::MonthCalendar;
}

constexpr bool IsTextAlignmentNode(ViewNodeType type) noexcept
{
    return IsAdvancedTypographyNode(type) || IsInput(type) ||
        type == ViewNodeType::Select;
}

constexpr bool IsGapNode(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::Row, ViewNodeType::Column,
        ViewNodeType::Grid, ViewNodeType::Flow, ViewNodeType::List,
        ViewNodeType::GridList, ViewNodeType::VirtualList,
        ViewNodeType::VirtualGrid, ViewNodeType::RadioGroup });
}

constexpr bool IsAlignedLayoutNode(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::Row, ViewNodeType::Column,
        ViewNodeType::Grid, ViewNodeType::Flow, ViewNodeType::List,
        ViewNodeType::GridList });
}

constexpr bool IsActionNode(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::Button, ViewNodeType::IconButton,
        ViewNodeType::Link, ViewNodeType::Toggle, ViewNodeType::Checkbox,
        ViewNodeType::RadioGroup, ViewNodeType::Select, ViewNodeType::Slider,
        ViewNodeType::TextInput, ViewNodeType::TextArea,
        ViewNodeType::SearchBox, ViewNodeType::NumberInput,
        ViewNodeType::MonthCalendar, ViewNodeType::ListItem,
        ViewNodeType::SlotItem });
}

constexpr bool IsControlledNode(ViewNodeType type) noexcept
{
    return IsCheck(type) || IsChoice(type) || IsInput(type) ||
        type == ViewNodeType::Slider ||
        type == ViewNodeType::MonthCalendar;
}

constexpr bool IsCollectionNode(ViewNodeType type) noexcept
{
    return IsType(type, { ViewNodeType::List, ViewNodeType::GridList,
        ViewNodeType::VirtualList, ViewNodeType::VirtualGrid });
}
}

std::span<const ViewNodeContract> ViewNodeContracts() noexcept
{
    return kContracts;
}

const ViewNodeContract* FindViewNodeContract(ViewNodeType type) noexcept
{
    const auto found = std::find_if(kContracts.begin(), kContracts.end(),
        [type](const ViewNodeContract& contract) {
            return contract.type == type;
        });
    return found == kContracts.end() ? nullptr : &*found;
}

const ViewNodeContract* FindViewNodeContract(std::string_view name) noexcept
{
    const auto found = std::find_if(kContracts.begin(), kContracts.end(),
        [name](const ViewNodeContract& contract) {
            return contract.name == name;
        });
    return found == kContracts.end() ? nullptr : &*found;
}

std::optional<ViewNodeType> FindViewNodeType(std::string_view name) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(name);
    return contract ? std::optional<ViewNodeType>(contract->type) : std::nullopt;
}

std::span<const ViewPropertyContract> ViewPropertyContracts() noexcept
{
    return kProperties;
}

const ViewPropertyContract* FindViewPropertyContract(
    std::string_view name) noexcept
{
    const auto found = std::find_if(kProperties.begin(), kProperties.end(),
        [name](const ViewPropertyContract& contract) {
            return contract.name == name;
        });
    return found == kProperties.end() ? nullptr : &*found;
}

std::string_view ViewPropertyValueKindName(
    ViewPropertyValueKind kind) noexcept
{
    switch (kind)
    {
    case ViewPropertyValueKind::String: return "string";
    case ViewPropertyValueKind::Boolean: return "boolean";
    case ViewPropertyValueKind::Number: return "number";
    case ViewPropertyValueKind::Integer: return "integer";
    case ViewPropertyValueKind::StringOrNumber: return "string-or-number";
    case ViewPropertyValueKind::Length: return "length";
    case ViewPropertyValueKind::EdgeInsets: return "edge-insets";
    case ViewPropertyValueKind::Offset: return "offset";
    case ViewPropertyValueKind::Resource: return "resource";
    case ViewPropertyValueKind::Color: return "color";
    case ViewPropertyValueKind::StringArray: return "string-array";
    case ViewPropertyValueKind::NumberArray: return "number-array";
    case ViewPropertyValueKind::IndexArray: return "index-array";
    case ViewPropertyValueKind::Node: return "node";
    case ViewPropertyValueKind::NodeArray: return "node-array";
    case ViewPropertyValueKind::Enum: return "enum";
    case ViewPropertyValueKind::Spans: return "styled-text-spans";
    case ViewPropertyValueKind::ChoiceOptions: return "choice-options";
    case ViewPropertyValueKind::TextSelection: return "text-selection";
    case ViewPropertyValueKind::Style: return "style";
    case ViewPropertyValueKind::Shadow: return "shadow";
    case ViewPropertyValueKind::Transform: return "transform";
    case ViewPropertyValueKind::Transition: return "transition";
    case ViewPropertyValueKind::PresenceTransition:
        return "presence-transition";
    case ViewPropertyValueKind::GridTracks: return "grid-tracks";
    case ViewPropertyValueKind::Tooltip: return "tooltip";
    case ViewPropertyValueKind::Accessibility: return "accessibility";
    case ViewPropertyValueKind::Events: return "events";
    case ViewPropertyValueKind::Action: return "action";
    }
    return {};
}

std::span<const std::string_view> ViewPropertyEnumValues(
    const ViewPropertyContract& contract) noexcept
{
    switch (contract.enumSet)
    {
    case ViewPropertyEnumSet::None: return {};
    case ViewPropertyEnumSet::NodeType: return kNodeTypeNames;
    case ViewPropertyEnumSet::IconFont: return kIconFontValues;
    case ViewPropertyEnumSet::ImageFit: return kImageFitValues;
    case ViewPropertyEnumSet::ImageAlignment:
        return kImageAlignmentValues;
    case ViewPropertyEnumSet::ImageInterpolation:
        return kImageInterpolationValues;
    case ViewPropertyEnumSet::Shape: return kShapeValues;
    case ViewPropertyEnumSet::Orientation: return kOrientationValues;
    case ViewPropertyEnumSet::ValidationState:
        return kValidationStateValues;
    case ViewPropertyEnumSet::Overflow: return kOverflowValues;
    case ViewPropertyEnumSet::SelectionMode:
        return kSelectionModeValues;
    case ViewPropertyEnumSet::FlexDirection:
        return kFlexDirectionValues;
    case ViewPropertyEnumSet::FlexWrap: return kFlexWrapValues;
    case ViewPropertyEnumSet::ContentAlignment:
        return kContentAlignmentValues;
    case ViewPropertyEnumSet::FontStyle: return kFontStyleValues;
    case ViewPropertyEnumSet::TextDirection:
        return kTextDirectionValues;
    case ViewPropertyEnumSet::Visibility: return kVisibilityValues;
    case ViewPropertyEnumSet::Alignment: return kAlignmentValues;
    case ViewPropertyEnumSet::SelfAlignment:
        return kSelfAlignmentValues;
    case ViewPropertyEnumSet::Justification:
        return kJustificationValues;
    case ViewPropertyEnumSet::TextAlignment:
        return kTextAlignmentValues;
    case ViewPropertyEnumSet::VerticalAlignment:
        return kVerticalAlignmentValues;
    case ViewPropertyEnumSet::TextWrap: return kTextWrapValues;
    case ViewPropertyEnumSet::TextOverflow: return kTextOverflowValues;
    }
    return {};
}

bool ViewPropertyAllowsEnumValue(const ViewPropertyContract& contract,
    std::string_view value) noexcept
{
    const auto values = ViewPropertyEnumValues(contract);
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool ViewPropertyNumericValueInRange(
    const ViewPropertyContract& contract, double value) noexcept
{
    return std::isfinite(value) && (!contract.hasNumericRange ||
        (value >= contract.numericMinimum &&
            value <= contract.numericMaximum));
}

std::span<const std::string_view> ViewNodePropertyNames() noexcept
{
    return kPropertyNames;
}

bool IsKnownViewNodeProperty(std::string_view property) noexcept
{
    return FindViewPropertyContract(property) != nullptr;
}

bool ViewNodeAllowsProperty(
    ViewNodeType type, std::string_view property) noexcept
{
    if (property == "children")
    {
        const ViewNodeContract* contract = FindViewNodeContract(type);
        return contract && contract->childPolicy != ViewChildPolicy::None;
    }
    if (property == "clip" || property == "overflow")
    {
        const ViewNodeContract* contract = FindViewNodeContract(type);
        return contract && contract->childPolicy != ViewChildPolicy::None;
    }
    if (property == "gap") return IsGapNode(type);
    if (property == "alignItems" || property == "justifyContent")
        return IsAlignedLayoutNode(type);
    if (property == "fontSize") return IsTypographyNode(type);
    if (property == "bold") return IsBoldTypographyNode(type);
    if (property == "textAlign") return IsTextAlignmentNode(type);
    if (property == "fontWeight" || property == "fontStyle" ||
        property == "lineHeight" || property == "letterSpacing" ||
        property == "verticalAlign" || property == "textWrap" ||
        property == "maxLines" || property == "overflowText")
        return IsAdvancedTypographyNode(type);
    if (Contains(kCommonProperties, property)) return true;
    if (property == "text")
        return type == ViewNodeType::Text || type == ViewNodeType::Badge;
    if (property == "spans") return type == ViewNodeType::StyledText;
    if (property == "label") return IsLabelNode(type);
    if (property == "glyph" || property == "iconFont")
        return type == ViewNodeType::Icon || type == ViewNodeType::IconButton;
    if (property == "source") return type == ViewNodeType::Image;
    if (property == "font") return IsTextResourceNode(type);
    if (property == "locale" || property == "textDirection")
        return IsTextualNode(type);
    if (property == "fit" || property == "alignment" ||
        property == "interpolation" || property == "alt")
        return type == ViewNodeType::Image ||
            type == ViewNodeType::ReferenceIcon;
    if (property == "tint") return type == ViewNodeType::Image;
    if (property == "shape") return type == ViewNodeType::Shape;
    if (property == "orientation")
        return IsType(type, { ViewNodeType::Divider,
            ViewNodeType::RadioGroup, ViewNodeType::Slider,
            ViewNodeType::Scroll, ViewNodeType::List,
            ViewNodeType::VirtualList });
    if (property == "value")
        return IsProgress(type) || type == ViewNodeType::Slider ||
            IsInput(type);
    if (property == "values") return IsSeries(type);
    if (property == "min" || property == "max")
        return IsSeries(type) || type == ViewNodeType::Slider ||
            type == ViewNodeType::NumberInput;
    if (property == "step")
        return type == ViewNodeType::Slider ||
            type == ViewNodeType::NumberInput;
    if (property == "options" || property == "selectedValue")
        return IsChoice(type);
    if (property == "placeholder")
        return IsInput(type) || type == ViewNodeType::Select;
    if (property == "expanded") return type == ViewNodeType::Select;
    if (property == "selectAll" || property == "liveUpdate" ||
        property == "maxBytes") return IsInput(type);
    if (property == "selection")
        return IsType(type, { ViewNodeType::TextInput,
            ViewNodeType::TextArea, ViewNodeType::SearchBox });
    if (property == "readOnly") return IsInput(type);
    if (property == "required")
        return IsInput(type) || type == ViewNodeType::Select;
    if (property == "indeterminate")
        return type == ViewNodeType::Checkbox ||
            type == ViewNodeType::ProgressBar ||
            type == ViewNodeType::ProgressRing;
    if (property == "validationState" || property == "validationMessage" ||
        property == "validationStyle")
        return IsInput(type) || type == ViewNodeType::Select;
    if (property == "year" || property == "month" ||
        property == "firstDayOfWeek" || property == "selectedDate" ||
        property == "todayDate" || property == "eventDates" ||
        property == "weekdayLabels" || property == "showAdjacentDates" ||
        property == "todayStyle" ||
        property == "adjacentStyle" || property == "eventStyle")
        return type == ViewNodeType::MonthCalendar;
    if (property == "binding" || property == "collection" ||
        property == "revision") return type == ViewNodeType::SlotSurface;
    if (property == "dropStyle") return type == ViewNodeType::SlotSurface;
    if (property == "reference")
        return type == ViewNodeType::SlotItem ||
            type == ViewNodeType::ReferenceIcon;
    if (property == "child")
        return type == ViewNodeType::SlotSurface ||
            type == ViewNodeType::SlotItem;
    if (property == "thickness")
        return IsProgress(type) || IsStrokedSeries(type) ||
            type == ViewNodeType::Divider;
    if (property == "trackOpacity")
        return IsProgress(type) || IsTrackedSeries(type);
    if (property == "fillOpacity")
        return IsProgress(type) || IsSeries(type);
    if (property == "columns") return IsGrid(type);
    if (property == "rows")
        return type == ViewNodeType::Grid || type == ViewNodeType::GridList;
    if (property == "columnGap")
        return IsGrid(type) || type == ViewNodeType::Flow ||
            type == ViewNodeType::VirtualList;
    if (property == "rowGap")
        return IsGrid(type) || type == ViewNodeType::Flow ||
            type == ViewNodeType::VirtualList;
    if (property == "itemCount" || property == "itemExtent" ||
        property == "firstIndex" || property == "overscan")
        return IsVirtual(type);
    if (property == "estimatedItemSize" || property == "layoutRevision")
        return type == ViewNodeType::VirtualList;
    if (property == "sectionHeaderIndices" ||
        property == "stickyHeaderIndex")
        return type == ViewNodeType::VirtualList;
    if (property == "initialScrollKey")
        return type == ViewNodeType::Scroll;
    if (property == "initialScrollIndex") return IsVirtual(type);
    if (property == "selectionMode" || property == "selectedKeys")
        return IsType(type, { ViewNodeType::List, ViewNodeType::GridList,
            ViewNodeType::VirtualList, ViewNodeType::VirtualGrid });
    if (property == "emptyContent")
        return type == ViewNodeType::SlotSurface ||
            IsType(type, { ViewNodeType::List, ViewNodeType::GridList,
                ViewNodeType::VirtualList, ViewNodeType::VirtualGrid });
    if (property == "loadingContent")
        return IsType(type, { ViewNodeType::List, ViewNodeType::GridList,
            ViewNodeType::VirtualList, ViewNodeType::VirtualGrid });
    if (property == "flexDirection" || property == "flexWrap" ||
        property == "alignContent")
        return type == ViewNodeType::Row || type == ViewNodeType::Column;
    if (property == "checked") return IsCheck(type);
    if (property == "sticky") return type == ViewNodeType::ListItem;
    if (property == "checkedStyle")
        return IsCheck(type) || type == ViewNodeType::RadioGroup;
    if (property == "showScrollbar")
        return type == ViewNodeType::Scroll || IsVirtual(type);
    if (property == "accessKey" || property == "acceleratorText")
        return IsActionNode(type) &&
            type != ViewNodeType::RadioGroup &&
            type != ViewNodeType::MonthCalendar;
    if (property == "capturePointer")
        return !IsInput(type) && type != ViewNodeType::Select &&
            type != ViewNodeType::Spacer;
    if (property == "action") return IsActionNode(type);
    return false;
}

bool ViewNodeRequiresProperty(
    ViewNodeType type, std::string_view property) noexcept
{
    if (property == "type" || property == "key") return true;
    if (property == "columns") return IsGrid(type);
    if (property == "itemCount" || property == "firstIndex")
        return IsVirtual(type);
    if (property == "itemExtent") return type == ViewNodeType::VirtualGrid;
    if (property == "source") return type == ViewNodeType::Image;
    if (property == "reference")
        return type == ViewNodeType::ReferenceIcon ||
            type == ViewNodeType::SlotItem;
    if (property == "alt")
        return type == ViewNodeType::Image ||
            type == ViewNodeType::ReferenceIcon;
    if (property == "spans") return type == ViewNodeType::StyledText;
    if (property == "text") return type == ViewNodeType::Badge;
    if (property == "label")
        return IsType(type, { ViewNodeType::Button, ViewNodeType::Link,
            ViewNodeType::Toggle, ViewNodeType::Checkbox });
    if (property == "glyph")
        return type == ViewNodeType::Icon || type == ViewNodeType::IconButton;
    if (property == "checked") return IsCheck(type);
    if (property == "value")
        return type == ViewNodeType::Slider || IsInput(type);
    if (property == "options" || property == "selectedValue")
        return IsChoice(type);
    if (property == "values") return IsSeries(type);
    if (property == "year" || property == "month" ||
        property == "selectedDate" || property == "weekdayLabels")
        return type == ViewNodeType::MonthCalendar;
    if (property == "accessibility")
        return IsSeries(type) || IsInput(type) ||
            IsType(type, { ViewNodeType::IconButton, ViewNodeType::Meter,
                ViewNodeType::Slider, ViewNodeType::Select,
                ViewNodeType::MonthCalendar });
    return false;
}

ViewPropertyDefault ViewNodePropertyDefault(
    ViewNodeType type, std::string_view property) noexcept
{
    if (!ViewNodeAllowsProperty(type, property))
        return { ViewPropertyDefaultKind::NotApplicable, {} };
    if (ViewNodeRequiresProperty(type, property))
        return { ViewPropertyDefaultKind::Required, {} };

    const auto literal = [](std::string_view expression) {
        return ViewPropertyDefault{
            ViewPropertyDefaultKind::Literal, expression };
    };
    const auto conditional = [](std::string_view expression) {
        return ViewPropertyDefault{
            ViewPropertyDefaultKind::Conditional, expression };
    };

    if (property == "binding" || property == "collection")
        return conditional("exactly one of binding or collection is required");
    if ((property == "child" || property == "children") &&
        IsType(type, { ViewNodeType::Scroll, ViewNodeType::ListItem,
            ViewNodeType::SlotItem }))
        return conditional("exactly one content child is required");
    if ((property == "itemExtent" || property == "estimatedItemSize") &&
        type == ViewNodeType::VirtualList)
        return conditional(
            "one fixed itemExtent or estimatedItemSize is required");
    if (property == "min" || property == "max")
    {
        if (IsSeries(type)) return conditional("automatic range from values");
        return literal(property == "min" ? "0" : "1");
    }
    if (property == "width")
        return type == ViewNodeType::Divider
            ? conditional("fill; auto when orientation is vertical")
            : literal("fill");
    if (property == "height")
        return type == ViewNodeType::Divider
            ? conditional("auto; fill when orientation is vertical")
            : literal("auto");
    if (property == "padding")
    {
        if (type == ViewNodeType::Badge) return literal("4");
        if (IsInput(type)) return literal("8");
        return literal("0");
    }
    if (property == "gap")
        return literal(type == ViewNodeType::RadioGroup ? "8" : "0");
    if (property == "orientation")
        return literal(IsType(type, { ViewNodeType::Scroll,
            ViewNodeType::List, ViewNodeType::VirtualList })
            ? "vertical" : "horizontal");
    if (property == "flexDirection")
        return literal(type == ViewNodeType::Column ? "column" : "row");
    if (property == "textWrap")
        return literal(type == ViewNodeType::StyledText
            ? "wrap" : "noWrap");
    if (property == "overflowText")
        return literal(type == ViewNodeType::StyledText
            ? "clip" : "ellipsis");
    if (property == "accessibility")
        return conditional("host role and state derived from node type");

    const ViewPropertyContract* contract =
        FindViewPropertyContract(property);
    if (!contract)
        return { ViewPropertyDefaultKind::NotApplicable, {} };
    switch (contract->valueKind)
    {
    case ViewPropertyValueKind::String:
        return literal("\"\"");
    case ViewPropertyValueKind::Boolean:
        return literal(IsNamed(property, { "liveUpdate", "showScrollbar",
            "showAdjacentDates", "visible", "enabled" })
            ? "true" : "false");
    case ViewPropertyValueKind::Number:
        if (property == "step") return literal("0.01");
        if (property == "thickness") return literal("4");
        if (property == "trackOpacity" || property == "fillOpacity")
            return literal("1");
        if (property == "fontSize") return literal("15");
        if (property == "flexShrink") return literal("1");
        if (property == "lineHeight" || property == "estimatedItemSize" ||
            IsNamed(property, { "minWidth", "maxWidth", "minHeight",
                "maxHeight", "aspectRatio", "columnGap", "rowGap" }))
            return literal("nil");
        return literal("0");
    case ViewPropertyValueKind::Integer:
        if (property == "firstDayOfWeek" || property == "columnSpan" ||
            property == "rowSpan") return literal("1");
        if (property == "overscan") return literal("2");
        if (property == "stickyHeaderIndex" ||
            property == "initialScrollIndex" || property == "gridColumn" ||
            property == "gridRow" || property == "tabIndex")
            return literal("nil");
        return literal("0");
    case ViewPropertyValueKind::StringOrNumber:
        return literal("0");
    case ViewPropertyValueKind::Length:
        return literal("auto");
    case ViewPropertyValueKind::EdgeInsets:
        return literal("0");
    case ViewPropertyValueKind::Offset:
        return literal("{ x=0, y=0 }");
    case ViewPropertyValueKind::Resource:
    case ViewPropertyValueKind::Color:
    case ViewPropertyValueKind::Shadow:
    case ViewPropertyValueKind::Transform:
    case ViewPropertyValueKind::Transition:
    case ViewPropertyValueKind::PresenceTransition:
    case ViewPropertyValueKind::Tooltip:
    case ViewPropertyValueKind::Node:
    case ViewPropertyValueKind::Action:
    case ViewPropertyValueKind::TextSelection:
        return literal("nil");
    case ViewPropertyValueKind::StringArray:
    case ViewPropertyValueKind::NumberArray:
    case ViewPropertyValueKind::IndexArray:
    case ViewPropertyValueKind::Spans:
    case ViewPropertyValueKind::ChoiceOptions:
    case ViewPropertyValueKind::NodeArray:
    case ViewPropertyValueKind::Events:
        return literal("{}");
    case ViewPropertyValueKind::Style:
        return literal("{}");
    case ViewPropertyValueKind::GridTracks:
        return literal("nil");
    case ViewPropertyValueKind::Enum:
        if (property == "iconFont") return literal("fa");
        if (property == "fit") return literal("contain");
        if (property == "alignment") return literal("center");
        if (property == "interpolation") return literal("linear");
        if (property == "shape") return literal("rectangle");
        if (property == "validationState") return literal("none");
        if (property == "overflow") return literal("visible");
        if (property == "selectionMode") return literal("none");
        if (property == "flexWrap") return literal("noWrap");
        if (property == "alignContent") return literal("stretch");
        if (property == "fontStyle") return literal("normal");
        if (property == "textDirection") return literal("auto");
        if (property == "visibility") return literal("visible");
        if (property == "alignItems") return literal("stretch");
        if (property == "alignSelf") return literal("auto");
        if (property == "justifyContent") return literal("start");
        if (property == "textAlign") return literal("start");
        if (property == "verticalAlign") return literal("center");
        return literal("nil");
    case ViewPropertyValueKind::Accessibility:
        return conditional("host role and state derived from node type");
    }
    return { ViewPropertyDefaultKind::NotApplicable, {} };
}

std::vector<std::string_view> ViewNodeAllowedProperties(ViewNodeType type)
{
    std::vector<std::string_view> result;
    result.reserve(kProperties.size());
    for (const ViewPropertyContract& property : kProperties)
        if (ViewNodeAllowsProperty(type, property.name))
            result.push_back(property.name);
    return result;
}

std::vector<std::string_view> ViewNodeProhibitedProperties(ViewNodeType type)
{
    std::vector<std::string_view> result;
    result.reserve(kProperties.size());
    for (const ViewPropertyContract& property : kProperties)
        if (!ViewNodeAllowsProperty(type, property.name))
            result.push_back(property.name);
    return result;
}

std::vector<std::string_view> ViewNodeRequiredProperties(ViewNodeType type)
{
    std::vector<std::string_view> result;
    for (const ViewPropertyContract& property : kProperties)
        if (ViewNodeRequiresProperty(type, property.name))
            result.push_back(property.name);
    return result;
}

std::span<const ViewEventContract> ViewEventContracts() noexcept
{
    return kEventContracts;
}

bool IsKnownViewEvent(std::string_view event) noexcept
{
    return std::any_of(kEventContracts.begin(), kEventContracts.end(),
        [event](const ViewEventContract& contract) {
            return contract.name == event;
        });
}

bool ViewNodeAllowsEvent(
    ViewNodeType type, std::string_view event) noexcept
{
    if (!IsKnownViewEvent(event)) return false;
    if (event == "change")
        return IsControlledNode(type) || IsCollectionNode(type);
    if (event == "selectionChange")
        return IsType(type, { ViewNodeType::TextInput,
            ViewNodeType::TextArea, ViewNodeType::SearchBox });
    if (event == "focus" || event == "blur" || event == "submit")
        return IsInput(type);
    if (event == "scrollEnd")
        return type == ViewNodeType::Scroll ||
            type == ViewNodeType::VirtualList ||
            type == ViewNodeType::VirtualGrid;
    return true;
}

std::vector<std::string_view> ViewNodeAllowedEvents(ViewNodeType type)
{
    std::vector<std::string_view> result;
    result.reserve(kEventContracts.size());
    for (const ViewEventContract& contract : kEventContracts)
        if (ViewNodeAllowsEvent(type, contract.name))
            result.push_back(contract.name);
    return result;
}

std::span<const ViewValidationDiagnosticContract>
ViewValidationDiagnosticContracts() noexcept
{
    return kValidationDiagnostics;
}

bool IsKnownViewValidationDiagnosticCode(
    std::string_view code) noexcept
{
    return std::any_of(kValidationDiagnostics.begin(),
        kValidationDiagnostics.end(), [code](const auto& contract) {
            return contract.code == code;
        });
}
}
