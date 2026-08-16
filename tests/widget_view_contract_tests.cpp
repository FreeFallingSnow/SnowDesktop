#include "widget_view_contract.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace
{
using snowdesktop::widget_runtime::FindViewNodeContract;
using snowdesktop::widget_runtime::FindViewNodeType;
using snowdesktop::widget_runtime::FindViewPropertyContract;
using snowdesktop::widget_runtime::HasViewAccessibilityPattern;
using snowdesktop::widget_runtime::IsKnownViewEvent;
using snowdesktop::widget_runtime::IsKnownViewNodeProperty;
using snowdesktop::widget_runtime::ViewAccessibilityPattern;
using snowdesktop::widget_runtime::ViewChildPolicy;
using snowdesktop::widget_runtime::ViewEventContracts;
using snowdesktop::widget_runtime::ViewEventPayloadKind;
using snowdesktop::widget_runtime::ViewNodeAllowedProperties;
using snowdesktop::widget_runtime::ViewNodeAllowedEvents;
using snowdesktop::widget_runtime::ViewNodeAllowsEvent;
using snowdesktop::widget_runtime::ViewNodeContracts;
using snowdesktop::widget_runtime::ViewNodeRequiresProperty;
using snowdesktop::widget_runtime::ViewNodeRequiredProperties;
using snowdesktop::widget_runtime::ViewNodeType;
using snowdesktop::widget_runtime::ViewNodePropertyNames;
using snowdesktop::widget_runtime::ViewPropertyContracts;
using snowdesktop::widget_runtime::ViewPropertyNumericValueInRange;
using snowdesktop::widget_runtime::ViewPropertyValueKind;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

void TestContractCoverageAndRoundTrip()
{
    const auto contracts = ViewNodeContracts();
    Check(contracts.size() == 44,
        "the matrix must cover every currently public view node");
    std::set<ViewNodeType> types;
    std::set<std::string> names;
    for (const auto& contract : contracts)
    {
        Check(!contract.name.empty() && !contract.category.empty() &&
                !contract.feature.empty(),
            "each node contract must publish its name, category, and feature");
        Check(contract.type == ViewNodeType::Spacer ||
                !contract.uiaControlType.empty(),
            "every semantic node must publish a UIA control type");
        Check(contract.childPolicy >= ViewChildPolicy::Any &&
                contract.childPolicy <= ViewChildPolicy::LogicalSlot,
            "every node must publish a recognized child policy");
        Check(types.insert(contract.type).second,
            "node contract types must be unique");
        Check(names.emplace(contract.name).second,
            "node contract names must be unique");
        const auto byName = FindViewNodeType(contract.name);
        Check(byName && *byName == contract.type,
            "node names must round-trip to their enum type");
        Check(FindViewNodeContract(contract.type) == &contract &&
                FindViewNodeContract(contract.name) == &contract,
            "contract lookups must resolve the canonical matrix entry");

        const auto allowed = ViewNodeAllowedProperties(contract.type);
        const auto required = ViewNodeRequiredProperties(contract.type);
        Check(!allowed.empty(),
            "every public node must have an enumerable property contract");
        for (const auto property : required)
        {
            Check(IsKnownViewNodeProperty(property),
                "required properties must belong to the public vocabulary");
            Check(std::find(allowed.begin(), allowed.end(), property) !=
                    allowed.end(),
                "required properties must also be allowed");
        }
        Check(ViewNodeRequiresProperty(contract.type, "type") &&
                ViewNodeRequiresProperty(contract.type, "key"),
            "every node must require type and key");
    }
    Check(!FindViewNodeType("webView") &&
            FindViewNodeContract("webView") == nullptr,
        "unpublished nodes must not appear in the public matrix");
}

void TestPropertyMetadata()
{
    const auto properties = ViewPropertyContracts();
    const auto names = ViewNodePropertyNames();
    Check(properties.size() == 146 && names.size() == properties.size(),
        "the property metadata must cover the complete public vocabulary");
    std::set<std::string> uniqueNames;
    for (std::size_t index = 0; index < properties.size(); ++index)
    {
        const auto& property = properties[index];
        Check(!property.name.empty() && names[index] == property.name,
            "property name enumeration must reuse the metadata catalog");
        Check(uniqueNames.emplace(property.name).second,
            "property metadata names must be unique");
        Check(property.valueKind >= ViewPropertyValueKind::String &&
                property.valueKind <= ViewPropertyValueKind::Action,
            "every property must publish a recognized semantic value kind");
        Check(!property.hasNumericRange ||
                property.numericMinimum <= property.numericMaximum,
            "published numeric ranges must be ordered");
        Check(FindViewPropertyContract(property.name) == &property,
            "property metadata must round-trip by canonical name");
    }
    Check(FindViewPropertyContract("notAProperty") == nullptr,
        "unknown properties must not resolve to metadata");

    const auto* fontSize = FindViewPropertyContract("fontSize");
    const auto* opacity = FindViewPropertyContract("trackOpacity");
    const auto* children = FindViewPropertyContract("children");
    const auto* value = FindViewPropertyContract("value");
    Check(fontSize && fontSize->valueKind == ViewPropertyValueKind::Number &&
            fontSize->hasNumericRange &&
            fontSize->numericMinimum == 1.0 &&
            fontSize->numericMaximum == 512.0 &&
            opacity && opacity->valueKind == ViewPropertyValueKind::Number &&
            opacity->hasNumericRange &&
            opacity->numericMinimum == 0.0 &&
            opacity->numericMaximum == 1.0 &&
            ViewPropertyNumericValueInRange(*opacity, 0.5) &&
            !ViewPropertyNumericValueInRange(*opacity, 1.01),
        "bounded scalar properties must publish their host validation range");
    Check(children &&
            children->valueKind == ViewPropertyValueKind::NodeArray &&
            value &&
            value->valueKind == ViewPropertyValueKind::StringOrNumber,
        "structured and overloaded properties must publish semantic kinds");
}

void TestRepresentativeApplicability()
{
    using snowdesktop::widget_runtime::ViewNodeAllowsProperty;
    Check(ViewNodeAllowsProperty(ViewNodeType::Grid, "columns") &&
            ViewNodeAllowsProperty(ViewNodeType::Grid, "rows") &&
            ViewNodeAllowsProperty(ViewNodeType::GridList, "rows") &&
            !ViewNodeAllowsProperty(ViewNodeType::VirtualGrid, "rows") &&
            !ViewNodeAllowsProperty(ViewNodeType::Row, "columns"),
        "grid-only properties must be machine readable");
    Check(ViewNodeAllowsProperty(ViewNodeType::Box, "children") &&
            ViewNodeAllowsProperty(ViewNodeType::Scroll, "children") &&
            !ViewNodeAllowsProperty(ViewNodeType::Text, "children") &&
            !ViewNodeAllowsProperty(ViewNodeType::Button, "children"),
        "leaf nodes must reject children in the property matrix");
    Check(ViewNodeAllowsProperty(ViewNodeType::Text, "fontSize") &&
            ViewNodeAllowsProperty(ViewNodeType::Icon, "fontSize") &&
            ViewNodeAllowsProperty(ViewNodeType::MonthCalendar,
                "fontSize") &&
            !ViewNodeAllowsProperty(ViewNodeType::Shape, "fontSize") &&
            !ViewNodeAllowsProperty(ViewNodeType::ProgressRing,
                "textWrap"),
        "typography properties must not be accepted by non-text visuals");
    Check(ViewNodeAllowsProperty(ViewNodeType::TextInput, "fontSize") &&
            ViewNodeAllowsProperty(ViewNodeType::Select, "textAlign") &&
            ViewNodeAllowsProperty(ViewNodeType::RadioGroup, "bold") &&
            ViewNodeAllowsProperty(ViewNodeType::MonthCalendar, "bold") &&
            !ViewNodeAllowsProperty(ViewNodeType::TextInput,
                "fontWeight") &&
            !ViewNodeAllowsProperty(ViewNodeType::RadioGroup,
                "maxLines") &&
            !ViewNodeAllowsProperty(ViewNodeType::MonthCalendar,
                "lineHeight"),
        "advanced typography must stay on renderers that apply it");
    Check(ViewNodeAllowsProperty(ViewNodeType::Row, "gap") &&
            ViewNodeAllowsProperty(ViewNodeType::VirtualGrid, "gap") &&
            ViewNodeAllowsProperty(ViewNodeType::RadioGroup, "gap") &&
            !ViewNodeAllowsProperty(ViewNodeType::Box, "gap") &&
            !ViewNodeAllowsProperty(ViewNodeType::Button, "gap"),
        "gap must stay on layouts and controls that consume it");
    Check(ViewNodeAllowsProperty(ViewNodeType::Grid, "alignItems") &&
            ViewNodeAllowsProperty(ViewNodeType::List,
                "justifyContent") &&
            !ViewNodeAllowsProperty(ViewNodeType::Stack,
                "alignItems") &&
            !ViewNodeAllowsProperty(ViewNodeType::VirtualList,
                "justifyContent"),
        "container alignment must stay on layouts that consume it");
    Check(ViewNodeAllowsProperty(ViewNodeType::Scroll, "clip") &&
            ViewNodeAllowsProperty(ViewNodeType::ListItem, "overflow") &&
            !ViewNodeAllowsProperty(ViewNodeType::Image, "clip") &&
            !ViewNodeAllowsProperty(ViewNodeType::Text, "overflow"),
        "descendant clipping must stay on container nodes");
    Check(ViewNodeAllowsProperty(ViewNodeType::Box, "debugName") &&
            ViewNodeAllowsProperty(ViewNodeType::Text, "testId") &&
            !ViewNodeRequiresProperty(ViewNodeType::Button,
                "debugName") &&
            !ViewNodeRequiresProperty(ViewNodeType::Button, "testId"),
        "developer identity metadata must be optional on every node");
    Check(ViewNodeAllowsProperty(ViewNodeType::Row, "flexDirection") &&
            ViewNodeAllowsProperty(ViewNodeType::Column, "flexWrap") &&
            ViewNodeAllowsProperty(ViewNodeType::Row, "alignContent") &&
            !ViewNodeAllowsProperty(ViewNodeType::Box, "flexDirection") &&
            !ViewNodeAllowsProperty(ViewNodeType::List, "flexWrap"),
        "flex-container properties must be scoped to row and column");
    Check(ViewNodeAllowsProperty(ViewNodeType::Shape, "gridColumn") &&
            ViewNodeAllowsProperty(ViewNodeType::ListItem, "gridRow") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "columnSpan") &&
            ViewNodeAllowsProperty(ViewNodeType::Text, "rowSpan"),
        "grid child placement properties must be machine readable");
    Check(ViewNodeAllowsProperty(ViewNodeType::VirtualGrid, "itemCount") &&
            !ViewNodeAllowsProperty(ViewNodeType::GridList, "itemCount") &&
            ViewNodeAllowsProperty(ViewNodeType::Scroll,
                "initialScrollKey") &&
            !ViewNodeAllowsProperty(ViewNodeType::List,
                "initialScrollKey") &&
            ViewNodeAllowsProperty(ViewNodeType::VirtualList,
                "initialScrollIndex") &&
            !ViewNodeAllowsProperty(ViewNodeType::Scroll,
                "initialScrollIndex") &&
            ViewNodeAllowsProperty(ViewNodeType::VirtualList,
                "estimatedItemSize") &&
            ViewNodeAllowsProperty(ViewNodeType::VirtualList,
                "layoutRevision") &&
            ViewNodeAllowsProperty(ViewNodeType::VirtualList,
                "sectionHeaderIndices") &&
            ViewNodeAllowsProperty(ViewNodeType::VirtualList,
                "stickyHeaderIndex") &&
            !ViewNodeAllowsProperty(ViewNodeType::VirtualGrid,
                "sectionHeaderIndices") &&
            !ViewNodeAllowsProperty(ViewNodeType::VirtualGrid,
                "estimatedItemSize"),
        "virtual collection properties must not leak to eager collections");
    Check(ViewNodeAllowsProperty(ViewNodeType::List, "selectionMode") &&
            ViewNodeAllowsProperty(ViewNodeType::VirtualGrid,
                "selectedKeys") &&
            !ViewNodeAllowsProperty(ViewNodeType::ListItem,
                "selectionMode") &&
            !ViewNodeAllowsProperty(ViewNodeType::Grid, "selectedKeys"),
        "controlled selection properties must stay on collection containers");
    Check(ViewNodeAllowsProperty(ViewNodeType::List, "orientation") &&
            ViewNodeAllowsProperty(ViewNodeType::VirtualList,
                "orientation") &&
            ViewNodeAllowsProperty(ViewNodeType::VirtualList,
                "columnGap") &&
            !ViewNodeAllowsProperty(ViewNodeType::GridList, "orientation") &&
            !ViewNodeAllowsProperty(ViewNodeType::VirtualGrid,
                "orientation"),
        "only eager and virtual linear lists must expose collection orientation");
    Check(ViewNodeAllowsProperty(ViewNodeType::List, "emptyContent") &&
            ViewNodeAllowsProperty(ViewNodeType::VirtualList,
                "loadingContent") &&
            !ViewNodeAllowsProperty(ViewNodeType::Grid, "emptyContent") &&
            ViewNodeAllowsProperty(ViewNodeType::Grid, "busy") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "busy") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "visibility") &&
            ViewNodeAllowsProperty(ViewNodeType::Box, "visibility"),
        "collection content alternatives and common busy state must remain scoped");
    Check(ViewNodeAllowsProperty(ViewNodeType::ListItem, "sticky") &&
            !ViewNodeAllowsProperty(ViewNodeType::List, "sticky") &&
            !ViewNodeAllowsProperty(ViewNodeType::VirtualList, "sticky"),
        "sticky headers must remain an eager listItem-only contract");
    Check(ViewNodeAllowsProperty(ViewNodeType::Image, "source") &&
            ViewNodeAllowsProperty(ViewNodeType::ReferenceIcon, "reference") &&
            ViewNodeAllowsProperty(ViewNodeType::Image, "tint") &&
            !ViewNodeAllowsProperty(ViewNodeType::ReferenceIcon, "tint") &&
            !ViewNodeAllowsProperty(ViewNodeType::ReferenceIcon, "source"),
        "package images and opaque reference icons must remain distinct");
    Check(ViewNodeAllowsProperty(ViewNodeType::Button, "accessKey") &&
            ViewNodeAllowsProperty(
                ViewNodeType::TextInput, "acceleratorText") &&
            !ViewNodeAllowsProperty(ViewNodeType::Text, "accessKey") &&
            !ViewNodeAllowsProperty(
                ViewNodeType::RadioGroup, "accessKey") &&
            !ViewNodeAllowsProperty(
                ViewNodeType::MonthCalendar, "acceleratorText"),
        "access-key metadata must stay on single-target action nodes");
    Check(ViewNodeAllowsProperty(ViewNodeType::Shape, "capturePointer") &&
            ViewNodeAllowsProperty(ViewNodeType::Button,
                "capturePointer") &&
            !ViewNodeAllowsProperty(ViewNodeType::TextInput,
                "capturePointer") &&
            !ViewNodeAllowsProperty(ViewNodeType::Select,
                "capturePointer"),
        "pointer capture must stay on host-independent interaction nodes");
    Check(ViewNodeAllowsProperty(ViewNodeType::TextInput, "liveUpdate") &&
            ViewNodeAllowsProperty(ViewNodeType::TextInput, "selection") &&
            ViewNodeAllowsProperty(ViewNodeType::TextArea, "selection") &&
            ViewNodeAllowsProperty(ViewNodeType::SearchBox, "selection") &&
            !ViewNodeAllowsProperty(ViewNodeType::NumberInput,
                "selection") &&
            !ViewNodeAllowsProperty(ViewNodeType::Select, "selection") &&
            ViewNodeAllowsProperty(ViewNodeType::TextArea, "readOnly") &&
            !ViewNodeAllowsProperty(ViewNodeType::Button, "liveUpdate") &&
            !ViewNodeAllowsProperty(ViewNodeType::Select, "readOnly"),
        "input editing properties must be scoped to input nodes");
    Check(ViewNodeAllowsProperty(ViewNodeType::Button, "focusStyle") &&
            ViewNodeAllowsProperty(ViewNodeType::Text, "disabledStyle") &&
            ViewNodeAllowsProperty(ViewNodeType::ListItem, "selected") &&
            ViewNodeAllowsProperty(ViewNodeType::Box, "selectedStyle") &&
            ViewNodeAllowsProperty(ViewNodeType::Text, "focusable") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "tabIndex"),
        "focus, disabled, selected, and focus-order fields must be common properties");
    Check(ViewNodeAllowsProperty(ViewNodeType::Checkbox,
                "indeterminate") &&
            ViewNodeAllowsProperty(ViewNodeType::ProgressBar,
                "indeterminate") &&
            ViewNodeAllowsProperty(ViewNodeType::ProgressRing,
                "indeterminate") &&
            !ViewNodeAllowsProperty(ViewNodeType::Toggle,
                "indeterminate") &&
            !ViewNodeAllowsProperty(ViewNodeType::Meter,
                "indeterminate") &&
            ViewNodeAllowsProperty(ViewNodeType::TextInput, "required") &&
            ViewNodeAllowsProperty(ViewNodeType::Select, "required") &&
            !ViewNodeAllowsProperty(ViewNodeType::Button, "required"),
        "mixed checkbox, indeterminate progress, and required form states must remain scoped to their controls");
    Check(ViewNodeAllowsProperty(ViewNodeType::Divider, "thickness") &&
            !ViewNodeAllowsProperty(ViewNodeType::Divider,
                "trackOpacity") &&
            !ViewNodeAllowsProperty(ViewNodeType::Divider,
                "fillOpacity") &&
            ViewNodeAllowsProperty(ViewNodeType::ProgressRing,
                "trackOpacity") &&
            ViewNodeAllowsProperty(ViewNodeType::LineChart,
                "trackOpacity") &&
            ViewNodeAllowsProperty(ViewNodeType::BarChart,
                "trackOpacity") &&
            !ViewNodeAllowsProperty(ViewNodeType::Sparkline,
                "trackOpacity") &&
            !ViewNodeAllowsProperty(ViewNodeType::Spectrum,
                "trackOpacity") &&
            ViewNodeAllowsProperty(ViewNodeType::Waveform,
                "fillOpacity") &&
            ViewNodeAllowsProperty(ViewNodeType::Sparkline,
                "thickness") &&
            !ViewNodeAllowsProperty(ViewNodeType::BarChart,
                "thickness") &&
            !ViewNodeAllowsProperty(ViewNodeType::Spectrum,
                "thickness"),
        "series styling properties must stay on renderers that consume them");
    Check(ViewNodeAllowsProperty(ViewNodeType::RadioGroup,
                "checkedStyle") &&
            !ViewNodeAllowsProperty(ViewNodeType::Select,
                "checkedStyle"),
        "checkedStyle must not be accepted by the hard-coded select overlay");
    Check(ViewNodeAllowsProperty(ViewNodeType::TextInput,
                "validationState") &&
            ViewNodeAllowsProperty(ViewNodeType::NumberInput,
                "validationMessage") &&
            ViewNodeAllowsProperty(ViewNodeType::Select,
                "validationStyle") &&
            !ViewNodeAllowsProperty(ViewNodeType::Button,
                "validationState"),
        "validation fields must remain scoped to inputs and selects");
    Check(ViewNodeAllowsProperty(ViewNodeType::Box, "minWidth") &&
            ViewNodeAllowsProperty(ViewNodeType::Text, "maxHeight") &&
            ViewNodeAllowsProperty(ViewNodeType::Image, "aspectRatio") &&
            ViewNodeAllowsProperty(ViewNodeType::Spacer, "margin") &&
            ViewNodeAllowsProperty(ViewNodeType::Stack, "clip") &&
            ViewNodeAllowsProperty(ViewNodeType::Column, "overflow") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "shadow") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "transform") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "transition") &&
            ViewNodeAllowsProperty(ViewNodeType::Button,
                "enterTransition") &&
            ViewNodeAllowsProperty(ViewNodeType::Button,
                "exitTransition") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "offset") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "zIndex") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "flexBasis") &&
            ViewNodeAllowsProperty(ViewNodeType::Text, "flexShrink") &&
            ViewNodeAllowsProperty(ViewNodeType::StyledText, "textWrap") &&
            ViewNodeAllowsProperty(ViewNodeType::Text, "locale") &&
            ViewNodeAllowsProperty(ViewNodeType::TextInput,
                "textDirection") &&
            !ViewNodeAllowsProperty(ViewNodeType::Shape, "locale") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "maxLines") &&
            ViewNodeAllowsProperty(ViewNodeType::Link, "overflowText") &&
            ViewNodeAllowsProperty(ViewNodeType::Text, "verticalAlign") &&
            ViewNodeAllowsProperty(ViewNodeType::StyledText, "fontWeight") &&
            ViewNodeAllowsProperty(ViewNodeType::Button, "fontStyle") &&
            ViewNodeAllowsProperty(ViewNodeType::Text, "lineHeight") &&
            ViewNodeAllowsProperty(ViewNodeType::Link, "letterSpacing") &&
            ViewNodeAllowsProperty(ViewNodeType::Shape, "tooltip"),
        "bounded size and flex constraints must be common machine-readable properties");
    Check(ViewNodeAllowsProperty(ViewNodeType::MonthCalendar, "eventDates") &&
            !ViewNodeAllowsProperty(ViewNodeType::List, "eventDates"),
        "calendar properties must be scoped to monthCalendar");
    Check(ViewNodeAllowsProperty(ViewNodeType::SlotSurface, "binding") &&
            ViewNodeAllowsProperty(ViewNodeType::SlotSurface, "dropStyle") &&
            ViewNodeAllowsProperty(ViewNodeType::SlotSurface, "emptyContent") &&
            !ViewNodeAllowsProperty(ViewNodeType::SlotSurface, "loadingContent") &&
            ViewNodeAllowsProperty(ViewNodeType::SlotItem, "reference") &&
            !ViewNodeAllowsProperty(ViewNodeType::SlotItem, "binding") &&
            !ViewNodeAllowsProperty(ViewNodeType::SlotItem, "dropStyle") &&
            !ViewNodeAllowsProperty(ViewNodeType::Box, "dropStyle"),
        "logical-slot model, item, and drop-style fields must remain separated");

    const auto* button = FindViewNodeContract(ViewNodeType::Button);
    const auto* slider = FindViewNodeContract(ViewNodeType::Slider);
    const auto* input = FindViewNodeContract(ViewNodeType::TextInput);
    const auto* box = FindViewNodeContract(ViewNodeType::Box);
    const auto* scroll = FindViewNodeContract(ViewNodeType::Scroll);
    const auto* list = FindViewNodeContract(ViewNodeType::List);
    const auto* slot = FindViewNodeContract(ViewNodeType::SlotSurface);
    const auto* text = FindViewNodeContract(ViewNodeType::Text);
    Check(box && box->childPolicy == ViewChildPolicy::Any &&
            scroll && scroll->childPolicy == ViewChildPolicy::Single &&
            list && list->childPolicy == ViewChildPolicy::Collection &&
            slot && slot->childPolicy == ViewChildPolicy::LogicalSlot &&
            text && text->childPolicy == ViewChildPolicy::None,
        "representative node child policies must be machine readable");
    Check(button && button->uiaControlType == "Button" &&
            HasViewAccessibilityPattern(button->uiaPatterns,
                ViewAccessibilityPattern::Invoke) &&
            button->keyboardFocusable,
        "button UIA mapping must expose Invoke and keyboard focus");
    Check(slider && slider->uiaControlType == "Slider" &&
            HasViewAccessibilityPattern(slider->uiaPatterns,
                ViewAccessibilityPattern::RangeValue),
        "slider UIA mapping must expose RangeValue");
    Check(input && input->uiaControlType == "Edit" &&
            HasViewAccessibilityPattern(input->uiaPatterns,
                ViewAccessibilityPattern::Value),
        "text input UIA mapping must expose Value");
}

void TestEventContract()
{
    const auto events = ViewEventContracts();
    Check(events.size() == 17,
        "the event matrix must enumerate the complete public event vocabulary");
    std::set<std::string> names;
    for (const auto& event : events)
    {
        Check(!event.name.empty() && names.emplace(event.name).second,
            "event contract names must be non-empty and unique");
        Check(IsKnownViewEvent(event.name),
            "event names must round-trip through the public vocabulary");
    }
    for (const auto& node : ViewNodeContracts())
    {
        const auto allowed = ViewNodeAllowedEvents(node.type);
        Check(!allowed.empty(),
            "every public node must have an enumerable event contract");
        for (const auto event : allowed)
            Check(IsKnownViewEvent(event) &&
                    ViewNodeAllowsEvent(node.type, event),
                "enumerated node events must belong to the public matrix");
    }
    Check(!IsKnownViewEvent("dragDrop") &&
            !ViewNodeAllowsEvent(ViewNodeType::Button, "dragDrop"),
        "unpublished events must not enter the node matrix");

    Check(ViewNodeAllowsEvent(ViewNodeType::Box, "click") &&
            ViewNodeAllowsEvent(ViewNodeType::Text, "pointerMove") &&
            ViewNodeAllowsEvent(ViewNodeType::Button, "keyDown"),
        "common pointer, action, and key events must remain composable");
    Check(ViewNodeAllowsEvent(ViewNodeType::Slider, "change") &&
            ViewNodeAllowsEvent(ViewNodeType::List, "change") &&
            !ViewNodeAllowsEvent(ViewNodeType::Text, "change"),
        "change must stay scoped to controlled nodes and collections");
    Check(ViewNodeAllowsEvent(ViewNodeType::TextInput,
                "selectionChange") &&
            !ViewNodeAllowsEvent(ViewNodeType::NumberInput,
                "selectionChange") &&
            ViewNodeAllowsEvent(ViewNodeType::NumberInput, "submit") &&
            !ViewNodeAllowsEvent(ViewNodeType::Select, "submit"),
        "text-selection and input lifecycle events must remain scoped");
    Check(ViewNodeAllowsEvent(ViewNodeType::Scroll, "scrollEnd") &&
            ViewNodeAllowsEvent(ViewNodeType::VirtualGrid, "scrollEnd") &&
            !ViewNodeAllowsEvent(ViewNodeType::List, "scrollEnd"),
        "scrollEnd must stay on host-managed scroll containers");

    const auto key = std::find_if(events.begin(), events.end(),
        [](const auto& event) { return event.name == "keyDown"; });
    const auto change = std::find_if(events.begin(), events.end(),
        [](const auto& event) { return event.name == "change"; });
    Check(key != events.end() &&
            key->payload == ViewEventPayloadKind::Key &&
            change != events.end() &&
            change->payload == ViewEventPayloadKind::Change,
        "event payload categories must be machine readable");
}
}

int main()
{
    TestContractCoverageAndRoundTrip();
    TestPropertyMetadata();
    TestRepresentativeApplicability();
    TestEventContract();
    std::cout << "widget view contract tests passed\n";
    return 0;
}
