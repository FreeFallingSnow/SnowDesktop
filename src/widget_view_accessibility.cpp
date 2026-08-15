#include "widget_view_accessibility.h"

#include <algorithm>
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
    if (source.type == ViewNodeType::TextInput ||
        source.type == ViewNodeType::TextArea ||
        source.type == ViewNodeType::SearchBox)
    {
        target.valueText = source.inputValue;
    }
    else if (source.type == ViewNodeType::NumberInput)
    {
        target.valueText = FormatNumber(source.value);
        target.value = source.value;
        target.minimum = source.minimum;
        target.maximum = source.maximum;
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
    }
    else if (source.type == ViewNodeType::Text ||
        source.type == ViewNodeType::StyledText ||
        source.type == ViewNodeType::Badge)
    {
        target.valueText = source.text;
    }

    if (source.type == ViewNodeType::Toggle ||
        source.type == ViewNodeType::Checkbox)
        target.checked = source.checked;
    if (source.type == ViewNodeType::Select)
        target.expanded = source.expanded;
}

bool CollectNode(const ViewNode& source, std::string_view semanticPath,
    std::string_view focusedKey,
    const std::optional<ViewRect>& inheritedClip,
    std::size_t parentIndex,
    std::vector<ViewAccessibilityNode>& nodes,
    std::string& error)
{
    if (!source.visible) return true;
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
        target.role = source.accessibilityRole.empty()
            ? std::string(contract->defaultAccessibilityRole)
            : source.accessibilityRole;
        target.controlType = std::string(contract->uiaControlType);
        target.patterns = contract->uiaPatterns;
        target.bounds = source.frame;
        target.clip = inheritedClip;
        target.parentIndex = parentIndex;
        target.enabled = source.enabled;
        target.focusable = contract->keyboardFocusable && source.enabled;
        target.focused = target.focusable && source.key == focusedKey;
        target.offscreen = source.frame.width <= 0.0f ||
            source.frame.height <= 0.0f ||
            (inheritedClip && !Overlaps(source.frame, *inheritedClip));
        PopulateValueState(source, target);
        semanticParent = nodes.size();
        nodes.push_back(std::move(target));
        if (parentIndex != ViewAccessibilityNode::NoParent)
            nodes[parentIndex].children.push_back(semanticParent);
    }

    std::optional<ViewRect> childClip = inheritedClip;
    if ((source.type == ViewNodeType::Scroll ||
            source.type == ViewNodeType::VirtualList ||
            source.type == ViewNodeType::VirtualGrid) &&
        source.clipFrame)
        childClip = Intersect(inheritedClip, *source.clipFrame);
    for (std::size_t index = 0; index < source.children.size(); ++index)
    {
        const std::string childPath = std::string(semanticPath) + "/" +
            std::to_string(index);
        if (!CollectNode(source.children[index], childPath, focusedKey,
                childClip, semanticParent, nodes, error)) return false;
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
    if (!CollectNode(root, "0", focusedKey, std::nullopt,
            ViewAccessibilityNode::NoParent, nodes, error))
    {
        nodes.clear();
        return false;
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
        node.role = region.accessibilityRole;
        node.controlType = mapping.controlType;
        node.patterns = mapping.patterns;
        node.bounds = ImmediateBounds(region.shape);
        if (region.clip)
            node.clip = ViewRect{ region.clip->x, region.clip->y,
                region.clip->width, region.clip->height };
        node.enabled = region.enabled;
        node.focusable = mapping.focusable && region.enabled;
        node.focused = node.focusable && region.key == focusedKey;
        const std::optional<ViewRect> visibleClip = region.clip
            ? Intersect(surface, *node.clip)
            : std::optional<ViewRect>(surface);
        node.offscreen = node.bounds.width <= 0.0f ||
            node.bounds.height <= 0.0f || !visibleClip ||
            !Overlaps(node.bounds, *visibleClip);
        if (region.controlKind == InteractionControlKind::Toggle ||
            region.controlKind == InteractionControlKind::Checkbox ||
            region.controlKind == InteractionControlKind::Radio)
            node.checked = region.checked;
        if (region.controlKind == InteractionControlKind::Slider)
        {
            node.value = region.controlValue;
            node.minimum = region.minimum;
            node.maximum = region.maximum;
            node.valueText = FormatNumber(region.controlValue);
        }
        if (region.hasExpandedProposal) node.expanded = region.expanded;
        if (!region.currentSelection.empty())
            node.valueText = region.currentSelection;
        nodes.push_back(std::move(node));
    }
    return true;
}
}
