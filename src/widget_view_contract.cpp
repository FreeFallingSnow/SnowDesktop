#include "widget_view_contract.h"

#include <algorithm>
#include <array>
#include <initializer_list>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr auto kContracts = std::to_array<ViewNodeContract>({
    { ViewNodeType::Box, "box", "layout", "view.tree.core", "group" },
    { ViewNodeType::Row, "row", "layout", "view.tree.core", "group" },
    { ViewNodeType::Column, "column", "layout", "view.tree.core", "group" },
    { ViewNodeType::Grid, "grid", "layout", "view.grid.uniform", "group" },
    { ViewNodeType::Flow, "flow", "layout", "view.flow.wrap", "group" },
    { ViewNodeType::Stack, "stack", "layout", "view.tree.core", "group" },
    { ViewNodeType::Scroll, "scroll", "layout", "view.scroll", "group" },
    { ViewNodeType::List, "list", "collection", "view.collection.basic", "list" },
    { ViewNodeType::GridList, "gridList", "collection", "view.collection.basic", "grid" },
    { ViewNodeType::VirtualList, "virtualList", "collection", "view.collection.virtual", "list" },
    { ViewNodeType::VirtualGrid, "virtualGrid", "collection", "view.collection.virtual", "grid" },
    { ViewNodeType::ListItem, "listItem", "collection", "view.collection.basic", "listitem" },
    { ViewNodeType::Text, "text", "content", "view.tree.core", "text" },
    { ViewNodeType::StyledText, "styledText", "content", "view.styledText.basic", "text" },
    { ViewNodeType::TextInput, "textInput", "input", "view.inputControls", "textbox" },
    { ViewNodeType::TextArea, "textArea", "input", "view.inputControls", "textbox" },
    { ViewNodeType::SearchBox, "searchBox", "input", "view.inputControls", "searchbox" },
    { ViewNodeType::NumberInput, "numberInput", "input", "view.inputControls", "spinbutton" },
    { ViewNodeType::Select, "select", "input", "view.inputControls", "combobox" },
    { ViewNodeType::Image, "image", "content", "view.image", "img" },
    { ViewNodeType::ReferenceIcon, "referenceIcon", "content", "view.referenceIcon", "img" },
    { ViewNodeType::Button, "button", "action", "view.tree.core", "button" },
    { ViewNodeType::Link, "link", "action", "view.actionControls", "link" },
    { ViewNodeType::Toggle, "toggle", "action", "view.selectionControls", "switch" },
    { ViewNodeType::Checkbox, "checkbox", "action", "view.selectionControls", "checkbox" },
    { ViewNodeType::RadioGroup, "radioGroup", "action", "view.actionControls", "radiogroup" },
    { ViewNodeType::Slider, "slider", "action", "view.actionControls", "slider" },
    { ViewNodeType::Icon, "icon", "content", "view.tree.core", "img" },
    { ViewNodeType::IconButton, "iconButton", "action", "view.tree.core", "button" },
    { ViewNodeType::Shape, "shape", "content", "view.tree.core", "img" },
    { ViewNodeType::Badge, "badge", "status", "view.statusVisuals", "status" },
    { ViewNodeType::Divider, "divider", "layout", "view.statusVisuals", "separator" },
    { ViewNodeType::ProgressBar, "progressBar", "status", "view.tree.core", "progressbar" },
    { ViewNodeType::ProgressRing, "progressRing", "status", "view.tree.core", "progressbar" },
    { ViewNodeType::Meter, "meter", "status", "view.statusVisuals", "meter" },
    { ViewNodeType::Sparkline, "sparkline", "data", "view.dataSeries", "img" },
    { ViewNodeType::LineChart, "lineChart", "data", "view.dataSeries", "img" },
    { ViewNodeType::BarChart, "barChart", "data", "view.dataSeries", "img" },
    { ViewNodeType::Waveform, "waveform", "data", "view.dataSeries", "img" },
    { ViewNodeType::Spectrum, "spectrum", "data", "view.dataSeries", "img" },
    { ViewNodeType::MonthCalendar, "monthCalendar", "date", "view.monthCalendar", "grid" },
    { ViewNodeType::SlotSurface, "slotSurface", "slot", "view.logicalSlots", "group" },
    { ViewNodeType::SlotItem, "slotItem", "slot", "view.logicalSlots", "listitem" },
    { ViewNodeType::Spacer, "spacer", "layout", "view.tree.core", "" },
});

constexpr auto kProperties = std::to_array<std::string_view>({
    "type", "key", "text", "spans", "label", "glyph", "iconFont",
    "source", "font", "fit", "alignment", "interpolation", "alt",
    "shape", "orientation", "value", "values", "min", "max", "step",
    "options", "selectedValue", "placeholder", "expanded", "selectAll",
    "liveUpdate", "maxBytes", "year", "month", "firstDayOfWeek",
    "selectedDate", "todayDate", "eventDates", "weekdayLabels",
    "showAdjacentDates", "binding", "collection", "revision", "reference",
    "child", "thickness", "trackOpacity", "fillOpacity", "width", "height",
    "padding", "gap", "columns", "columnGap", "rowGap", "itemCount",
    "itemExtent", "firstIndex", "overscan", "flexGrow", "fontSize", "bold",
    "checked", "visible", "enabled", "cursor", "alignItems", "showScrollbar",
    "alignSelf", "justifyContent", "textAlign", "style", "hoverStyle",
    "pressedStyle", "checkedStyle", "selectedStyle", "todayStyle",
    "adjacentStyle", "eventStyle", "accessibility", "events", "action",
    "children",
});

constexpr auto kCommonProperties = std::to_array<std::string_view>({
    "type", "key", "width", "height", "padding", "gap", "flexGrow",
    "fontSize", "bold", "visible", "enabled", "cursor", "alignItems",
    "alignSelf", "justifyContent", "textAlign", "style", "hoverStyle",
    "pressedStyle", "accessibility", "events", "children",
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

std::span<const std::string_view> ViewNodePropertyNames() noexcept
{
    return kProperties;
}

bool IsKnownViewNodeProperty(std::string_view property) noexcept
{
    return Contains(kProperties, property);
}

bool ViewNodeAllowsProperty(
    ViewNodeType type, std::string_view property) noexcept
{
    if (Contains(kCommonProperties, property)) return true;
    if (property == "text")
        return type == ViewNodeType::Text || type == ViewNodeType::Badge;
    if (property == "spans") return type == ViewNodeType::StyledText;
    if (property == "label") return IsLabelNode(type);
    if (property == "glyph" || property == "iconFont")
        return type == ViewNodeType::Icon || type == ViewNodeType::IconButton;
    if (property == "source") return type == ViewNodeType::Image;
    if (property == "font") return IsTextResourceNode(type);
    if (property == "fit" || property == "alignment" ||
        property == "interpolation" || property == "alt")
        return type == ViewNodeType::Image ||
            type == ViewNodeType::ReferenceIcon;
    if (property == "shape") return type == ViewNodeType::Shape;
    if (property == "orientation")
        return IsType(type, { ViewNodeType::Divider,
            ViewNodeType::RadioGroup, ViewNodeType::Slider,
            ViewNodeType::Scroll });
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
    if (property == "year" || property == "month" ||
        property == "firstDayOfWeek" || property == "selectedDate" ||
        property == "todayDate" || property == "eventDates" ||
        property == "weekdayLabels" || property == "showAdjacentDates" ||
        property == "selectedStyle" || property == "todayStyle" ||
        property == "adjacentStyle" || property == "eventStyle")
        return type == ViewNodeType::MonthCalendar;
    if (property == "binding" || property == "collection" ||
        property == "revision") return type == ViewNodeType::SlotSurface;
    if (property == "reference")
        return type == ViewNodeType::SlotItem ||
            type == ViewNodeType::ReferenceIcon;
    if (property == "child")
        return type == ViewNodeType::SlotSurface ||
            type == ViewNodeType::SlotItem;
    if (property == "thickness" || property == "trackOpacity" ||
        property == "fillOpacity")
        return IsProgress(type) || IsSeries(type) ||
            type == ViewNodeType::Divider;
    if (property == "columns") return IsGrid(type);
    if (property == "columnGap")
        return IsGrid(type) || type == ViewNodeType::Flow;
    if (property == "rowGap")
        return IsGrid(type) || type == ViewNodeType::Flow ||
            type == ViewNodeType::VirtualList;
    if (property == "itemCount" || property == "itemExtent" ||
        property == "firstIndex" || property == "overscan")
        return IsVirtual(type);
    if (property == "checked") return IsCheck(type);
    if (property == "checkedStyle") return IsCheck(type) || IsChoice(type);
    if (property == "showScrollbar")
        return type == ViewNodeType::Scroll || IsVirtual(type);
    if (property == "action") return IsActionNode(type);
    return false;
}

bool ViewNodeRequiresProperty(
    ViewNodeType type, std::string_view property) noexcept
{
    if (property == "type" || property == "key") return true;
    if (property == "columns") return IsGrid(type);
    if (property == "itemCount" || property == "itemExtent" ||
        property == "firstIndex") return IsVirtual(type);
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

std::vector<std::string_view> ViewNodeAllowedProperties(ViewNodeType type)
{
    std::vector<std::string_view> result;
    result.reserve(kProperties.size());
    for (const std::string_view property : kProperties)
        if (ViewNodeAllowsProperty(type, property)) result.push_back(property);
    return result;
}

std::vector<std::string_view> ViewNodeRequiredProperties(ViewNodeType type)
{
    std::vector<std::string_view> result;
    for (const std::string_view property : kProperties)
        if (ViewNodeRequiresProperty(type, property)) result.push_back(property);
    return result;
}
}
