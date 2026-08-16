#include "widget_view_accessibility.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace snowdesktop::widget_runtime
{
namespace
{
bool Overlaps(const ViewRect& left, const ViewRect& right) noexcept
{
    return left.x < right.x + right.width &&
        left.x + left.width > right.x &&
        left.y < right.y + right.height &&
        left.y + left.height > right.y;
}

std::optional<ViewRect> Intersect(
    const std::optional<ViewRect>& first,
    const ViewRect& second) noexcept
{
    if (!first) return second;
    const float left = std::max(first->x, second.x);
    const float top = std::max(first->y, second.y);
    const float right = std::min(
        first->x + first->width, second.x + second.width);
    const float bottom = std::min(
        first->y + first->height, second.y + second.height);
    return ViewRect{ left, top, std::max(0.0f, right - left),
        std::max(0.0f, bottom - top) };
}

std::string FormatNumber(float value)
{
    char buffer[48]{};
    std::snprintf(buffer, sizeof(buffer), "%.9g",
        static_cast<double>(value));
    return buffer;
}

std::string AccessibleName(const ViewNode& node)
{
    if (!node.accessibilityLabel.empty())
        return node.accessibilityLabel;
    if (!node.alt.empty()) return node.alt;
    return node.text;
}

std::string FormatAccessKey(std::string_view key)
{
    if (key.size() != 1) return {};
    char character = key[0];
    if (character >= 'a' && character <= 'z')
        character = static_cast<char>(character - 'a' + 'A');
    return std::string("Alt+") + character;
}

struct GridPosition
{
    int row = 0;
    int column = 0;
    int rowSpan = 1;
    int columnSpan = 1;
};

bool IsGridContainer(ViewNodeType type) noexcept
{
    return type == ViewNodeType::Grid ||
        type == ViewNodeType::GridList ||
        type == ViewNodeType::VirtualGrid ||
        type == ViewNodeType::MonthCalendar;
}

void PopulateContainerState(const ViewNode& source,
    ViewAccessibilityNode& target)
{
    target.canSelectMultiple = source.selectionMode ==
        ViewSelectionMode::Multiple;
    target.selectionRequired = source.type == ViewNodeType::RadioGroup ||
        source.type == ViewNodeType::MonthCalendar;
    if (IsGridContainer(source.type))
    {
        const std::size_t columns = source.type ==
                ViewNodeType::MonthCalendar
            ? 7 : std::max<std::size_t>(1, source.columns);
        std::size_t rows = 0;
        if (source.type == ViewNodeType::Grid ||
            source.type == ViewNodeType::GridList)
        {
            if (source.collectionContent == ViewCollectionContent::Items)
                for (const auto& child : source.children)
                    if (child.visible)
                        rows = std::max(rows,
                            child.resolvedGridRow + child.rowSpan);
        }
        else
        {
            const std::size_t items = source.type ==
                    ViewNodeType::MonthCalendar
                ? 42 : source.children.size();
            rows = items == 0 ? 0 : (items + columns - 1) / columns;
        }
        target.gridColumnCount = static_cast<int>(columns);
        target.gridRowCount = static_cast<int>(rows);
    }
    if (source.type == ViewNodeType::Scroll ||
        source.type == ViewNodeType::VirtualList ||
        source.type == ViewNodeType::VirtualGrid)
    {
        target.scrollHorizontal = source.orientation ==
            ViewOrientation::Horizontal;
        target.scrollViewportExtent = std::max(
            0.0f, source.scrollViewportExtent);
        target.scrollContentExtent = std::max(
            target.scrollViewportExtent, source.scrollContentExtent);
        const float maximum = std::max(0.0f,
            target.scrollContentExtent - target.scrollViewportExtent);
        target.scrollOffset = std::clamp(
            source.scrollOffset, 0.0f, maximum);
    }
}

void PopulateGridItemState(const GridPosition& position,
    ViewAccessibilityNode& target)
{
    target.patterns = target.patterns |
        ViewAccessibilityPattern::GridItem;
    target.gridRow = position.row;
    target.gridColumn = position.column;
    target.gridRowSpan = position.rowSpan;
    target.gridColumnSpan = position.columnSpan;
}

struct ImmediateAccessibilityMapping
{
    const char* controlType = "Custom";
    ViewAccessibilityPattern patterns = ViewAccessibilityPattern::None;
    bool focusable = false;
};

ImmediateAccessibilityMapping MapImmediateRegion(
    const InteractionRegion& region) noexcept
{
    if (region.controlKind == InteractionControlKind::Toggle)
        return { "Button", ViewAccessibilityPattern::Toggle, true };
    if (region.controlKind == InteractionControlKind::Checkbox)
        return { "CheckBox", ViewAccessibilityPattern::Toggle, true };
    if (region.controlKind == InteractionControlKind::Radio)
        return { "RadioButton",
            ViewAccessibilityPattern::SelectionItem, true };
    if (region.controlKind == InteractionControlKind::Slider)
        return { "Slider", ViewAccessibilityPattern::RangeValue, true };
    if (region.accessibilityRole == "button")
        return { "Button", ViewAccessibilityPattern::Invoke, true };
    if (region.accessibilityRole == "link")
        return { "Hyperlink", ViewAccessibilityPattern::Invoke, true };
    if (region.accessibilityRole == "textbox" ||
        region.accessibilityRole == "searchbox")
        return { "Edit", ViewAccessibilityPattern::Value, true };
    if (region.accessibilityRole == "spinbutton")
        return { "Spinner", ViewAccessibilityPattern::Value |
            ViewAccessibilityPattern::RangeValue, true };
    if (region.accessibilityRole == "combobox")
        return { "ComboBox", ViewAccessibilityPattern::ExpandCollapse |
            ViewAccessibilityPattern::Selection, true };
    if (region.accessibilityRole == "option" ||
        region.accessibilityRole == "listitem")
        return { "ListItem", ViewAccessibilityPattern::SelectionItem,
            region.enabled };
    if (region.accessibilityRole == "img") return { "Image" };
    if (region.accessibilityRole == "separator") return { "Separator" };
    if (region.accessibilityRole == "status") return { "Text" };
    if (region.accessibilityRole == "meter")
        return { "ProgressBar", ViewAccessibilityPattern::RangeValue };
    if (region.accessibilityRole == "group") return { "Group" };
    if (region.events.contains("click"))
        return { "Custom", ViewAccessibilityPattern::Invoke, true };
    return {};
}

ViewRect ImmediateBounds(const InteractionShape& shape) noexcept
{
    if (shape.type == InteractionShapeType::Circle)
        return { shape.x - shape.radius, shape.y - shape.radius,
            shape.radius * 2.0f, shape.radius * 2.0f };
    return { shape.x, shape.y, shape.width, shape.height };
}

void PopulateValueState(const ViewNode& source,
    ViewAccessibilityNode& target)
{
    target.helpText = source.validationMessage.empty()
        ? source.tooltip : source.validationMessage;
    target.required = source.required;
    if (source.type == ViewNodeType::TextInput ||
        source.type == ViewNodeType::TextArea ||
        source.type == ViewNodeType::SearchBox)
    {
        target.valueText = source.inputValue;
        target.valueReadOnly = source.readOnly;
    }
    else if (source.type == ViewNodeType::NumberInput)
    {
        target.valueText = FormatNumber(source.value);
        target.value = source.value;
        target.minimum = source.minimum;
        target.maximum = source.maximum;
        target.step = source.step;
        target.valueReadOnly = source.readOnly;
        target.rangeValueReadOnly = source.readOnly;
    }
    else if (source.type == ViewNodeType::Select ||
        source.type == ViewNodeType::RadioGroup)
    {
        target.valueText = source.selectedValue;
    }
    else if (source.type == ViewNodeType::Slider ||
        source.type == ViewNodeType::ProgressBar ||
        source.type == ViewNodeType::ProgressRing ||
        source.type == ViewNodeType::Meter)
    {
        target.valueText = FormatNumber(source.value);
        target.value = source.value;
        target.minimum = source.minimum;
        target.maximum = source.maximum;
        target.step = source.step;
        target.rangeValueReadOnly = source.type != ViewNodeType::Slider;
    }
    else if (source.type == ViewNodeType::Text ||
        source.type == ViewNodeType::StyledText ||
        source.type == ViewNodeType::Badge)
    {
        target.valueText = source.text;
    }

    if (source.type == ViewNodeType::Toggle)
        target.checked = source.checked;
    else if (source.type == ViewNodeType::Checkbox && !source.indeterminate)
        target.checked = source.checked;
    if (HasViewAccessibilityPattern(target.patterns,
            ViewAccessibilityPattern::SelectionItem))
        target.checked = source.selected;
    if (source.type == ViewNodeType::Select)
        target.expanded = source.expanded;
}

bool AppendVirtualAccessibilityNode(const ViewNode& source,
    std::string key, std::string name, std::string role,
    std::string controlType, std::string valueText,
    const ViewRect& bounds, const std::optional<ViewRect>& clip,
    std::size_t parentIndex, bool enabled, bool checked,
    std::optional<GridPosition> gridPosition,
    std::string_view focusedKey,
    std::vector<ViewAccessibilityNode>& nodes,
    std::string& error)
{
    if (parentIndex == ViewAccessibilityNode::NoParent ||
        parentIndex >= nodes.size())
    {
        error = "virtual accessibility node is missing its semantic parent";
        return false;
    }
    if (nodes.size() >= ViewTreeLimits::MaximumNodes)
    {
        error = "view accessibility node limit exceeded (512)";
        return false;
    }
    ViewAccessibilityNode target;
    target.sourceType = source.type;
    target.semanticId = "key:" + key;
    target.key = std::move(key);
    target.name = std::move(name);
    target.role = std::move(role);
    target.controlType = std::move(controlType);
    target.valueText = std::move(valueText);
    target.patterns = ViewAccessibilityPattern::SelectionItem;
    target.bounds = bounds;
    target.clip = clip;
    target.parentIndex = parentIndex;
    target.enabled = enabled;
    target.focusable = source.focusable.value_or(true) && enabled;
    target.focused = enabled && target.key == focusedKey;
    target.offscreen = bounds.width <= 0.0f || bounds.height <= 0.0f ||
        (clip && !Overlaps(bounds, *clip));
    target.checked = checked;
    if (gridPosition)
        PopulateGridItemState(*gridPosition, target);
    const std::size_t index = nodes.size();
    nodes.push_back(std::move(target));
    nodes[parentIndex].children.push_back(index);
    return true;
}

bool AppendVirtualAccessibilityChildren(const ViewNode& source,
    float viewportHeight, std::string_view focusedKey,
    const std::optional<ViewRect>& inheritedClip,
    std::size_t parentIndex,
    std::vector<ViewAccessibilityNode>& nodes,
    std::string& error)
{
    if (source.type == ViewNodeType::RadioGroup)
    {
        for (std::size_t index = 0; index < source.options.size(); ++index)
        {
            const auto& option = source.options[index];
            if (!AppendVirtualAccessibilityNode(source,
                    source.key + "/" + option.key,
                    option.label, "radio", "RadioButton", option.value,
                    ViewRadioOptionFrame(source, index), inheritedClip,
                    parentIndex, source.enabled && option.enabled,
                    option.value == source.selectedValue,
                    std::nullopt,
                    focusedKey, nodes, error))
                return false;
        }
    }
    else if (source.type == ViewNodeType::Select && source.expanded)
    {
        for (std::size_t index = 0; index < source.options.size(); ++index)
        {
            const auto& option = source.options[index];
            if (!AppendVirtualAccessibilityNode(source,
                    source.key + "/" + option.key,
                    option.label, "option", "ListItem", option.value,
                    ViewSelectOptionFrame(source, index, viewportHeight),
                    inheritedClip, parentIndex,
                    source.enabled && option.enabled,
                    option.value == source.selectedValue,
                    std::nullopt,
                    focusedKey, nodes, error))
                return false;
        }
    }
    else if (source.type == ViewNodeType::MonthCalendar)
    {
        std::array<ViewMonthCalendarCell, 42> cells;
        if (!BuildViewMonthCalendarCells(source, cells, error))
            return false;
        for (std::size_t index = 0; index < cells.size(); ++index)
        {
            const auto& cell = cells[index];
            if (!cell.currentMonth && !source.showAdjacentDates) continue;
            if (!AppendVirtualAccessibilityNode(source,
                    source.key + "/" + cell.date,
                    cell.date, "gridcell", "DataItem", cell.date,
                    ViewMonthCalendarCellFrame(source, index),
                    inheritedClip, parentIndex, source.enabled,
                    cell.selected,
                    GridPosition{ static_cast<int>(index / 7),
                        static_cast<int>(index % 7) },
                    focusedKey, nodes, error))
                return false;
        }
    }
    return true;
}

bool CollectNode(const ViewNode& source, std::string_view semanticPath,
    std::string_view focusedKey,
    float viewportHeight,
    const std::optional<ViewRect>& inheritedClip,
    std::optional<GridPosition> gridPosition,
    std::size_t parentIndex,
    std::vector<ViewAccessibilityNode>& nodes,
    std::string& error)
{
    if (!source.visible || source.visibility == ViewVisibility::Hidden)
        return true;
    const ViewNodeContract* contract = FindViewNodeContract(source.type);
    if (!contract)
    {
        error = "accessibility snapshot encountered an unknown view node";
        return false;
    }
    std::size_t semanticParent = parentIndex;
    if (!contract->uiaControlType.empty())
    {
        if (nodes.size() >= ViewTreeLimits::MaximumNodes)
        {
            error = "view accessibility node limit exceeded (512)";
            return false;
        }
        ViewAccessibilityNode target;
        target.sourceType = source.type;
        target.semanticId = source.key.empty()
            ? "path:" + std::string(semanticPath)
            : "key:" + source.key;
        target.key = source.key;
        target.name = AccessibleName(source);
        target.accessKey = FormatAccessKey(source.accessKey);
        target.acceleratorText = source.acceleratorText;
        target.role = source.accessibilityRole.empty()
            ? std::string(contract->defaultAccessibilityRole)
            : source.accessibilityRole;
        target.controlType = std::string(contract->uiaControlType);
        target.patterns = contract->uiaPatterns;
        target.bounds = source.frame;
        target.clip = inheritedClip;
        target.parentIndex = parentIndex;
        target.enabled = source.enabled;
        target.busy = source.busy;
        target.focusable = source.focusable.value_or(
            contract->keyboardFocusable) && source.enabled;
        target.focused = target.focusable && source.key == focusedKey;
        target.canSelectMultiple = source.inheritedSelectionMode ==
            ViewSelectionMode::Multiple;
        target.offscreen = source.frame.width <= 0.0f ||
            source.frame.height <= 0.0f ||
            (inheritedClip && !Overlaps(source.frame, *inheritedClip));
        PopulateValueState(source, target);
        PopulateContainerState(source, target);
        if (gridPosition)
            PopulateGridItemState(*gridPosition, target);
        semanticParent = nodes.size();
        nodes.push_back(std::move(target));
        if (parentIndex != ViewAccessibilityNode::NoParent)
            nodes[parentIndex].children.push_back(semanticParent);
        if (!AppendVirtualAccessibilityChildren(source, viewportHeight,
                focusedKey, inheritedClip, semanticParent,
                nodes, error))
            return false;
    }

    std::optional<ViewRect> childClip = inheritedClip;
    if (source.clipFrame)
    {
        childClip = Intersect(inheritedClip, *source.clipFrame);
    }
    for (std::size_t index = 0; index < source.children.size(); ++index)
    {
        std::optional<GridPosition> childGridPosition;
        if (IsGridContainer(source.type) &&
            source.collectionContent == ViewCollectionContent::Items)
        {
            const auto& child = source.children[index];
            if (source.type == ViewNodeType::Grid ||
                source.type == ViewNodeType::GridList)
            {
                childGridPosition = GridPosition{
                    static_cast<int>(child.resolvedGridRow),
                    static_cast<int>(child.resolvedGridColumn),
                    static_cast<int>(child.rowSpan),
                    static_cast<int>(child.columnSpan) };
            }
            else
            {
                const std::size_t columns = source.type ==
                        ViewNodeType::MonthCalendar
                    ? 7 : std::max<std::size_t>(1, source.columns);
                childGridPosition = GridPosition{
                    static_cast<int>(index / columns),
                    static_cast<int>(index % columns) };
            }
        }
        const std::string childPath = std::string(semanticPath) + "/" +
            std::to_string(index);
        if (!CollectNode(source.children[index], childPath, focusedKey,
                viewportHeight,
                childClip, childGridPosition, semanticParent,
                nodes, error)) return false;
    }
    return true;
}
}

bool CollectViewAccessibilityNodes(const ViewNode& root,
    std::string_view focusedKey,
    std::vector<ViewAccessibilityNode>& nodes,
    std::string& error)
{
    nodes.clear();
    error.clear();
    if (!CollectNode(root, "0", focusedKey, root.frame.height,
            std::nullopt, std::nullopt,
            ViewAccessibilityNode::NoParent, nodes, error))
    {
        nodes.clear();
        return false;
    }
    for (auto& node : nodes)
    {
        node.bounds = ApplyViewTransform(node.bounds,
            ResolveViewTransformForKey(root, node.key));
        node.clip = ResolveViewClipForKey(root, node.key, false);
        node.offscreen = node.bounds.width <= 0.0f ||
            node.bounds.height <= 0.0f ||
            (node.clip && !Overlaps(node.bounds, *node.clip));
    }
    return true;
}


bool CollectInteractionAccessibilityNodes(
    const std::vector<InteractionRegion>& regions,
    float surfaceWidth, float surfaceHeight,
    std::string_view focusedKey,
    std::vector<ViewAccessibilityNode>& nodes,
    std::string& error)
{
    nodes.clear();
    error.clear();
    const ViewRect surface{ 0.0f, 0.0f,
        std::max(0.0f, surfaceWidth), std::max(0.0f, surfaceHeight) };
    for (const auto& region : regions)
    {
        if (nodes.size() >= WidgetInteractionRegions::kMaximumRegions)
        {
            nodes.clear();
            error = "interaction accessibility node limit exceeded (256)";
            return false;
        }
        const ImmediateAccessibilityMapping mapping =
            MapImmediateRegion(region);
        ViewAccessibilityNode node;
        node.semanticId = "key:" + region.key;
        node.key = region.key;
        node.name = region.accessibilityLabel;
        node.accessKey = FormatAccessKey(region.accessKey);
        node.acceleratorText = region.acceleratorText;
        node.role = region.accessibilityRole;
        node.controlType = mapping.controlType;
        node.patterns = mapping.patterns;
        node.bounds = ImmediateBounds(region.shape);
        if (region.clip)
            node.clip = ViewRect{ region.clip->x, region.clip->y,
                region.clip->width, region.clip->height };
        node.enabled = region.enabled;
        node.focusable = region.focusable.value_or(mapping.focusable) &&
            region.enabled;
        node.focused = node.focusable && region.key == focusedKey;
        const std::optional<ViewRect> visibleClip = region.clip
            ? Intersect(surface, *node.clip)
            : std::optional<ViewRect>(surface);
        node.offscreen = node.bounds.width <= 0.0f ||
            node.bounds.height <= 0.0f || !visibleClip ||
            !Overlaps(node.bounds, *visibleClip);
        if (region.controlKind == InteractionControlKind::Toggle ||
            region.controlKind == InteractionControlKind::Radio ||
            (region.controlKind == InteractionControlKind::Checkbox &&
                !region.indeterminate))
            node.checked = region.checked;
        if (region.controlKind == InteractionControlKind::Slider)
        {
            node.value = region.controlValue;
            node.minimum = region.minimum;
            node.maximum = region.maximum;
            node.step = region.step;
            node.valueText = FormatNumber(region.controlValue);
            node.rangeValueReadOnly = false;
        }
        if (mapping.patterns == ViewAccessibilityPattern::Value)
            node.valueReadOnly = false;
        if (region.hasExpandedProposal) node.expanded = region.expanded;
        if (!region.currentSelection.empty())
            node.valueText = region.currentSelection;
        nodes.push_back(std::move(node));
    }
    return true;
}
}
