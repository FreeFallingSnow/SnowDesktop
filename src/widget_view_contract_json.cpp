#include "widget_view_contract_json.h"

#include "widget_view_contract.h"
#include "widget_view_tree.h"

#include <array>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <string_view>

namespace snowdesktop::widget_runtime
{
namespace
{
void WriteJsonString(std::ostream& output, std::string_view value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20)
            {
                output << "\\u00" << kHex[character >> 4]
                       << kHex[character & 0x0f];
            }
            else
            {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
}

template <typename Range, typename Writer>
void WriteJsonArray(std::ostream& output, const Range& values,
    Writer&& writer)
{
    output << '[';
    bool first = true;
    for (const auto& value : values)
    {
        if (!first) output << ',';
        first = false;
        writer(output, value);
    }
    output << ']';
}

std::string_view ChildPolicyName(ViewChildPolicy policy) noexcept
{
    switch (policy)
    {
    case ViewChildPolicy::Any: return "any";
    case ViewChildPolicy::None: return "none";
    case ViewChildPolicy::Single: return "single";
    case ViewChildPolicy::Collection: return "collection";
    case ViewChildPolicy::LogicalSlot: return "logical-slot";
    }
    return {};
}

std::string_view EventPayloadName(ViewEventPayloadKind payload) noexcept
{
    switch (payload)
    {
    case ViewEventPayloadKind::Pointer: return "pointer";
    case ViewEventPayloadKind::Wheel: return "wheel";
    case ViewEventPayloadKind::Key: return "key";
    case ViewEventPayloadKind::Action: return "action";
    case ViewEventPayloadKind::Change: return "change";
    case ViewEventPayloadKind::SelectionChange: return "selection-change";
    case ViewEventPayloadKind::Focus: return "focus";
    case ViewEventPayloadKind::Submit: return "submit";
    case ViewEventPayloadKind::ScrollEnd: return "scroll-end";
    }
    return {};
}

std::string_view DefaultKindName(ViewPropertyDefaultKind kind) noexcept
{
    switch (kind)
    {
    case ViewPropertyDefaultKind::NotApplicable: return "not-applicable";
    case ViewPropertyDefaultKind::Required: return "required";
    case ViewPropertyDefaultKind::Literal: return "literal";
    case ViewPropertyDefaultKind::Conditional: return "conditional";
    }
    return {};
}

struct AccessibilityPatternName
{
    ViewAccessibilityPattern pattern;
    std::string_view name;
};

constexpr auto kAccessibilityPatterns =
    std::to_array<AccessibilityPatternName>({
        { ViewAccessibilityPattern::Invoke, "invoke" },
        { ViewAccessibilityPattern::Toggle, "toggle" },
        { ViewAccessibilityPattern::Selection, "selection" },
        { ViewAccessibilityPattern::SelectionItem, "selection-item" },
        { ViewAccessibilityPattern::RangeValue, "range-value" },
        { ViewAccessibilityPattern::Value, "value" },
        { ViewAccessibilityPattern::ExpandCollapse, "expand-collapse" },
        { ViewAccessibilityPattern::Scroll, "scroll" },
        { ViewAccessibilityPattern::Grid, "grid" },
        { ViewAccessibilityPattern::GridItem, "grid-item" },
    });

struct PropertyEffectName
{
    ViewPropertyEffect effect;
    std::string_view name;
};

constexpr auto kPropertyEffects = std::to_array<PropertyEffectName>({
    { ViewPropertyEffect::Layout, "layout" },
    { ViewPropertyEffect::Paint, "paint" },
    { ViewPropertyEffect::HitTest, "hit-test" },
    { ViewPropertyEffect::Input, "input" },
    { ViewPropertyEffect::Accessibility, "accessibility" },
    { ViewPropertyEffect::Resource, "resource" },
    { ViewPropertyEffect::Tree, "tree" },
});

struct PropertyTransitionEffectName
{
    ViewPropertyTransitionEffect effect;
    std::string_view name;
};

constexpr auto kPropertyTransitionEffects =
    std::to_array<PropertyTransitionEffectName>({
        { ViewPropertyTransitionEffect::Visual, "visual" },
        { ViewPropertyTransitionEffect::Transform, "transform" },
        { ViewPropertyTransitionEffect::Layout, "layout" },
    });

void WriteAccessibilityPatterns(std::ostream& output,
    ViewAccessibilityPattern patterns)
{
    output << '[';
    bool first = true;
    for (const auto& entry : kAccessibilityPatterns)
    {
        if (!HasViewAccessibilityPattern(patterns, entry.pattern)) continue;
        if (!first) output << ',';
        first = false;
        WriteJsonString(output, entry.name);
    }
    output << ']';
}

void WritePropertyEffects(std::ostream& output, ViewPropertyEffect effects)
{
    output << '[';
    bool first = true;
    for (const auto& entry : kPropertyEffects)
    {
        if (!HasViewPropertyEffect(effects, entry.effect)) continue;
        if (!first) output << ',';
        first = false;
        WriteJsonString(output, entry.name);
    }
    output << ']';
}

void WritePropertyTransitionEffects(std::ostream& output,
    ViewPropertyTransitionEffect effects)
{
    output << '[';
    bool first = true;
    for (const auto& entry : kPropertyTransitionEffects)
    {
        if (!HasViewPropertyTransitionEffect(effects, entry.effect))
            continue;
        if (!first) output << ',';
        first = false;
        WriteJsonString(output, entry.name);
    }
    output << ']';
}

void WriteNode(std::ostream& output, const ViewNodeContract& node)
{
    output << "{\"name\":";
    WriteJsonString(output, node.name);
    output << ",\"category\":";
    WriteJsonString(output, node.category);
    output << ",\"feature\":";
    WriteJsonString(output, node.feature);
    output << ",\"childPolicy\":";
    WriteJsonString(output, ChildPolicyName(node.childPolicy));
    output << ",\"accessibility\":{\"role\":";
    WriteJsonString(output, node.defaultAccessibilityRole);
    output << ",\"uiaControlType\":";
    WriteJsonString(output, node.uiaControlType);
    output << ",\"keyboardFocusable\":"
           << (node.keyboardFocusable ? "true" : "false")
           << ",\"patterns\":";
    WriteAccessibilityPatterns(output, node.uiaPatterns);
    output << "},\"properties\":";

    const auto properties = ViewNodeAllowedProperties(node.type);
    WriteJsonArray(output, properties,
        [type = node.type](std::ostream& stream, std::string_view name) {
            const ViewPropertyDefault defaultValue =
                ViewNodePropertyDefault(type, name);
            stream << "{\"name\":";
            WriteJsonString(stream, name);
            stream << ",\"required\":"
                   << (ViewNodeRequiresProperty(type, name)
                           ? "true" : "false")
                   << ",\"default\":{\"kind\":";
            WriteJsonString(stream, DefaultKindName(defaultValue.kind));
            stream << ",\"expression\":";
            WriteJsonString(stream, defaultValue.expression);
            stream << "}}";
        });

    output << ",\"prohibitedProperties\":";
    const auto prohibited = ViewNodeProhibitedProperties(node.type);
    WriteJsonArray(output, prohibited,
        [](std::ostream& stream, std::string_view name) {
            WriteJsonString(stream, name);
        });

    output << ",\"events\":";
    const auto events = ViewNodeAllowedEvents(node.type);
    WriteJsonArray(output, events,
        [](std::ostream& stream, std::string_view event) {
            WriteJsonString(stream, event);
        });
    output << '}';
}

void WriteProperty(std::ostream& output,
    const ViewPropertyContract& property)
{
    output << "{\"name\":";
    WriteJsonString(output, property.name);
    output << ",\"type\":";
    WriteJsonString(output, ViewPropertyValueKindName(property.valueKind));
    output << ",\"enumValues\":";
    WriteJsonArray(output, ViewPropertyEnumValues(property),
        [](std::ostream& stream, std::string_view value) {
            WriteJsonString(stream, value);
        });
    output << ",\"numericRange\":";
    if (property.hasNumericRange)
    {
        output << "{\"minimum\":" << property.numericMinimum
               << ",\"maximum\":" << property.numericMaximum << '}';
    }
    else
    {
        output << "null";
    }
    output << ",\"effects\":";
    WritePropertyEffects(output, property.effects);
    output << ",\"transitionEffects\":";
    WritePropertyTransitionEffects(output, property.transitionEffects);
    output << '}';
}

void WriteEvent(std::ostream& output, const ViewEventContract& event)
{
    output << "{\"name\":";
    WriteJsonString(output, event.name);
    output << ",\"payload\":";
    WriteJsonString(output, EventPayloadName(event.payload));
    output << '}';
}

void WriteLimits(std::ostream& output)
{
    output << "{\"maximumNodes\":" << ViewTreeLimits::MaximumNodes
           << ",\"maximumDepth\":" << ViewTreeLimits::MaximumDepth
           << ",\"maximumTextBytes\":" << ViewTreeLimits::MaximumTextBytes
           << ",\"maximumTooltipTitleBytes\":"
           << ViewTreeLimits::MaximumTooltipTitleBytes
           << ",\"maximumTotalTextBytes\":"
           << ViewTreeLimits::MaximumTotalTextBytes
           << ",\"maximumResources\":" << ViewTreeLimits::MaximumResources
           << ",\"maximumSeriesPoints\":"
           << ViewTreeLimits::MaximumSeriesPoints
           << ",\"maximumTotalSeriesPoints\":"
           << ViewTreeLimits::MaximumTotalSeriesPoints
           << ",\"maximumChoiceOptions\":"
           << ViewTreeLimits::MaximumChoiceOptions
           << ",\"maximumTextSpans\":" << ViewTreeLimits::MaximumTextSpans
           << ",\"maximumCalendarEventDates\":"
           << ViewTreeLimits::MaximumCalendarEventDates
           << ",\"maximumCollectionItems\":"
           << ViewTreeLimits::MaximumCollectionItems
           << ",\"maximumScrollContainers\":"
           << ViewTreeLimits::MaximumScrollContainers
           << ",\"maximumVirtualItemCount\":"
           << ViewTreeLimits::MaximumVirtualItemCount
           << ",\"maximumVirtualWindowItems\":"
           << ViewTreeLimits::MaximumVirtualWindowItems
           << ",\"maximumVirtualOverscan\":"
           << ViewTreeLimits::MaximumVirtualOverscan
           << ",\"maximumVirtualSectionHeaders\":"
           << ViewTreeLimits::MaximumVirtualSectionHeaders
           << ",\"maximumDebugNameBytes\":"
           << ViewTreeLimits::MaximumDebugNameBytes
           << ",\"maximumTestIdBytes\":"
           << ViewTreeLimits::MaximumTestIdBytes << '}';
}

void WriteTransitions(std::ostream& output)
{
    constexpr auto easings = std::to_array<std::string_view>({
        "linear", "easeIn", "easeOut", "easeInOut",
    });
    constexpr auto updateProperties = std::to_array<std::string_view>({
        "background", "foreground", "borderColor", "opacity",
        "transform", "layout",
    });
    constexpr auto presenceFields = std::to_array<std::string_view>({
        "opacity", "transform",
    });
    const auto writeString = [](std::ostream& stream,
                                 std::string_view value) {
        WriteJsonString(stream, value);
    };

    output << "{\"durationMs\":{\"minimum\":1,\"maximum\":2000,"
              "\"default\":120},\"easings\":";
    WriteJsonArray(output, easings, writeString);
    output << ",\"defaultEasing\":\"easeOut\","
              "\"update\":{\"properties\":";
    WriteJsonArray(output, updateProperties, writeString);
    output << ",\"minimumProperties\":1,\"maximumProperties\":4},"
              "\"presence\":{\"fields\":";
    WriteJsonArray(output, presenceFields, writeString);
    output << ",\"minimumFields\":1},"
              "\"runtime\":{\"driver\":\"host\","
              "\"runsLuaPerFrame\":false,"
              "\"preview\":\"final-state\","
              "\"reducedMotion\":\"final-state\"}}";
}

void WritePreview(std::ostream& output)
{
    output << "{\"renderer\":\"host\",\"context\":\"preview\","
              "\"storage\":\"isolated-overlay\","
              "\"validatesTree\":true,"
              "\"transitions\":\"final-state\"}";
}

void WriteDirectionality(std::ostream& output)
{
    output << "{\"localeProperty\":\"locale\","
              "\"directionProperty\":\"textDirection\","
              "\"values\":[\"auto\",\"ltr\",\"rtl\"],"
              "\"autoResolution\":[\"first-strong-character\","
              "\"locale-language\",\"ltr-fallback\"],"
              "\"textShaping\":\"directwrite\","
              "\"startEndAlignment\":\"direction-aware\","
              "\"controlAdornmentPlacement\":\"direction-aware\","
              "\"layoutOrder\":\"declaration-order\","
              "\"keyboardOrder\":\"declaration-order\","
              "\"accessibilityOrder\":\"declaration-order\","
              "\"inherited\":false}";
}

void WriteValidation(std::ostream& output)
{
    output << "{\"unknownNode\":\"reject-tree\","
              "\"unknownProperty\":\"reject-tree\","
              "\"prohibitedProperty\":\"reject-tree\","
              "\"invalidValue\":\"reject-tree\","
              "\"commit\":\"atomic\","
              "\"onFailure\":\"retain-last-successful-tree\","
              "\"diagnostic\":\"bounded-message\"}";
}
}

std::string SerializeViewContractJson()
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17)
           << "{\"ok\":true,\"schemaVersion\":2,\"apiVersion\":2,"
              "\"propertyPolicy\":\"closed-world\","
              "\"nodes\":";
    WriteJsonArray(output, ViewNodeContracts(), WriteNode);
    output << ",\"properties\":";
    WriteJsonArray(output, ViewPropertyContracts(), WriteProperty);
    output << ",\"events\":";
    WriteJsonArray(output, ViewEventContracts(), WriteEvent);
    output << ",\"limits\":";
    WriteLimits(output);
    output << ",\"transitions\":";
    WriteTransitions(output);
    output << ",\"preview\":";
    WritePreview(output);
    output << ",\"directionality\":";
    WriteDirectionality(output);
    output << ",\"validation\":";
    WriteValidation(output);
    output << '}';
    return output.str();
}
}
