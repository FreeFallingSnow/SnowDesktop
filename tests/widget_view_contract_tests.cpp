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
using snowdesktop::widget_runtime::HasViewAccessibilityPattern;
using snowdesktop::widget_runtime::IsKnownViewNodeProperty;
using snowdesktop::widget_runtime::ViewAccessibilityPattern;
using snowdesktop::widget_runtime::ViewNodeAllowedProperties;
using snowdesktop::widget_runtime::ViewNodeContracts;
using snowdesktop::widget_runtime::ViewNodeRequiresProperty;
using snowdesktop::widget_runtime::ViewNodeRequiredProperties;
using snowdesktop::widget_runtime::ViewNodeType;

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

void TestRepresentativeApplicability()
{
    using snowdesktop::widget_runtime::ViewNodeAllowsProperty;
    Check(ViewNodeAllowsProperty(ViewNodeType::Grid, "columns") &&
            !ViewNodeAllowsProperty(ViewNodeType::Row, "columns"),
        "grid-only properties must be machine readable");
    Check(ViewNodeAllowsProperty(ViewNodeType::VirtualGrid, "itemCount") &&
            !ViewNodeAllowsProperty(ViewNodeType::GridList, "itemCount"),
        "virtual collection properties must not leak to eager collections");
    Check(ViewNodeAllowsProperty(ViewNodeType::Image, "source") &&
            ViewNodeAllowsProperty(ViewNodeType::ReferenceIcon, "reference") &&
            !ViewNodeAllowsProperty(ViewNodeType::ReferenceIcon, "source"),
        "package images and opaque reference icons must remain distinct");
    Check(ViewNodeAllowsProperty(ViewNodeType::TextInput, "liveUpdate") &&
            ViewNodeAllowsProperty(ViewNodeType::TextArea, "readOnly") &&
            !ViewNodeAllowsProperty(ViewNodeType::Button, "liveUpdate") &&
            !ViewNodeAllowsProperty(ViewNodeType::Select, "readOnly"),
        "input editing properties must be scoped to input nodes");
    Check(ViewNodeAllowsProperty(ViewNodeType::Button, "focusStyle") &&
            ViewNodeAllowsProperty(ViewNodeType::Text, "disabledStyle"),
        "focus and disabled state styles must be common properties");
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
            ViewNodeAllowsProperty(ViewNodeType::Image, "aspectRatio"),
        "bounded size constraints must be common machine-readable properties");
    Check(ViewNodeAllowsProperty(ViewNodeType::MonthCalendar, "eventDates") &&
            !ViewNodeAllowsProperty(ViewNodeType::List, "eventDates"),
        "calendar properties must be scoped to monthCalendar");
    Check(ViewNodeAllowsProperty(ViewNodeType::SlotSurface, "binding") &&
            ViewNodeAllowsProperty(ViewNodeType::SlotItem, "reference") &&
            !ViewNodeAllowsProperty(ViewNodeType::SlotItem, "binding"),
        "logical-slot model and item fields must remain separated");

    const auto* button = FindViewNodeContract(ViewNodeType::Button);
    const auto* slider = FindViewNodeContract(ViewNodeType::Slider);
    const auto* input = FindViewNodeContract(ViewNodeType::TextInput);
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
}

int main()
{
    TestContractCoverageAndRoundTrip();
    TestRepresentativeApplicability();
    std::cout << "widget view contract tests passed\n";
    return 0;
}
