#include <ole2.h>
#include <UIAutomation.h>

#include "widget/view/widget_accessibility_provider.h"
#include "widget/view/widget_accessibility_events.h"
#include "ui/input/text_input_accessibility.h"

#include <oleauto.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using snowdesktop::WidgetAccessibilityProviderHost;
using snowdesktop::WidgetAccessibilityChangeKind;
using snowdesktop::WidgetAccessibilityElementKind;
using snowdesktop::widget_runtime::ViewAccessibilityNode;
using snowdesktop::widget_runtime::ViewAccessibilityPattern;
using snowdesktop::widget_runtime::AccessibilityLive;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

std::wstring PropertyString(
    IRawElementProviderSimple* provider, PROPERTYID property)
{
    VARIANT value;
    VariantInit(&value);
    Check(SUCCEEDED(provider->GetPropertyValue(property, &value)) &&
            value.vt == VT_BSTR,
        "string property must return a BSTR");
    const std::wstring result(value.bstrVal,
        static_cast<std::size_t>(SysStringLen(value.bstrVal)));
    VariantClear(&value);
    return result;
}

bool PropertyBool(IRawElementProviderSimple* provider, PROPERTYID property)
{
    VARIANT value;
    VariantInit(&value);
    Check(SUCCEEDED(provider->GetPropertyValue(property, &value)) &&
            value.vt == VT_BOOL,
        "boolean property must return VT_BOOL");
    const bool result = value.boolVal == VARIANT_TRUE;
    VariantClear(&value);
    return result;
}

int PropertyInt(IRawElementProviderSimple* provider, PROPERTYID property)
{
    VARIANT value;
    VariantInit(&value);
    Check(SUCCEEDED(provider->GetPropertyValue(property, &value)) &&
            value.vt == VT_I4,
        "integer property must return VT_I4");
    const int result = value.lVal;
    VariantClear(&value);
    return result;
}

ComPtr<IRawElementProviderSimple> AsSimple(
    IRawElementProviderFragment* fragment)
{
    ComPtr<IRawElementProviderSimple> result;
    Check(fragment && SUCCEEDED(fragment->QueryInterface(
            IID_PPV_ARGS(&result))),
        "fragment must also expose the simple provider interface");
    return result;
}

LuaWidgetAccessibilitySnapshot Snapshot()
{
    LuaWidgetAccessibilitySnapshot widget;
    widget.widgetId = L"widget-a";
    widget.packageId = "example.accessibility";
    widget.name = "Sample widget";
    widget.bounds = RECT{ 10, 20, 210, 170 };

    ViewAccessibilityNode group;
    group.semanticId = "key:root";
    group.key = "root";
    group.name = "Panel";
    group.busy = true;
    group.controlType = "Group";
    group.bounds = { 0, 0, 300, 330 };
    group.children = { 1, 2, 3, 4, 5, 6, 9, 10, 13 };

    ViewAccessibilityNode button;
    button.semanticId = "key:open";
    button.key = "open";
    button.name = "Open";
    button.controlType = "Button";
    button.bounds = { 10, 10, 80, 30 };
    button.parentIndex = 0;
    button.focusable = true;
    button.focused = true;
    button.patterns = ViewAccessibilityPattern::Invoke;
    button.accessKey = "Alt+O";
    button.acceleratorText = "Ctrl+O";

    ViewAccessibilityNode status;
    status.semanticId = "path:0/1";
    status.name = "Ready";
    status.role = "status";
    status.valueText = "All systems ready";
    status.helpText = "Current system status";
    status.live = AccessibilityLive::Polite;
    status.headingLevel = 2;
    status.positionInSet = 1;
    status.setSize = 4;
    status.controlType = "Text";
    status.bounds = { 10, 60, 100, 20 };
    status.parentIndex = 0;

    ViewAccessibilityNode toggle;
    toggle.semanticId = "key:enabled";
    toggle.key = "enabled";
    toggle.name = "Enabled";
    toggle.controlType = "CheckBox";
    toggle.bounds = { 10, 90, 100, 24 };
    toggle.parentIndex = 0;
    toggle.patterns = ViewAccessibilityPattern::Toggle;
    toggle.checked = false;

    ViewAccessibilityNode slider;
    slider.semanticId = "key:level";
    slider.key = "level";
    slider.name = "Level";
    slider.controlType = "Slider";
    slider.bounds = { 10, 125, 160, 24 };
    slider.parentIndex = 0;
    slider.patterns = ViewAccessibilityPattern::RangeValue;
    slider.value = 4.0f;
    slider.minimum = 0.0f;
    slider.maximum = 10.0f;
    slider.step = 0.5f;
    slider.rangeValueReadOnly = false;

    ViewAccessibilityNode input;
    input.semanticId = "key:query";
    input.key = "query";
    input.name = "Query";
    input.controlType = "Edit";
    input.bounds = { 10, 160, 160, 28 };
    input.parentIndex = 0;
    input.patterns = ViewAccessibilityPattern::Value;
    input.valueText = "before";
    input.helpText = "Enter a search term";
    input.valueReadOnly = false;
    input.required = true;
    input.labelledBySemanticId = "path:0/1";
    input.describedBySemanticId = "path:0/1";

    ViewAccessibilityNode combo;
    combo.semanticId = "key:choice";
    combo.key = "choice";
    combo.name = "Choice";
    combo.controlType = "ComboBox";
    combo.bounds = { 10, 200, 160, 28 };
    combo.parentIndex = 0;
    combo.sourceType = snowdesktop::widget_runtime::ViewNodeType::Select;
    combo.patterns = ViewAccessibilityPattern::ExpandCollapse |
        ViewAccessibilityPattern::Selection;
    combo.expanded = false;
    combo.children = { 7, 8 };

    ViewAccessibilityNode optionA;
    optionA.semanticId = "key:choice/a";
    optionA.key = "choice/a";
    optionA.name = "Option A";
    optionA.controlType = "ListItem";
    optionA.bounds = { 10, 235, 160, 28 };
    optionA.parentIndex = 6;
    optionA.patterns = ViewAccessibilityPattern::SelectionItem;
    optionA.checked = false;

    ViewAccessibilityNode optionB = optionA;
    optionB.semanticId = "key:choice/b";
    optionB.key = "choice/b";
    optionB.name = "Option B";
    optionB.bounds.y = 265;
    optionB.checked = true;

    ViewAccessibilityNode scroll;
    scroll.sourceType = snowdesktop::widget_runtime::ViewNodeType::Scroll;
    scroll.semanticId = "key:feed-scroll";
    scroll.key = "feed-scroll";
    scroll.name = "Feed";
    scroll.controlType = "Pane";
    scroll.bounds = { 180, 10, 100, 100 };
    scroll.parentIndex = 0;
    scroll.patterns = ViewAccessibilityPattern::Scroll;
    scroll.scrollHorizontal = false;
    scroll.scrollOffset = 25.0f;
    scroll.scrollViewportExtent = 100.0f;
    scroll.scrollContentExtent = 300.0f;

    ViewAccessibilityNode grid;
    grid.semanticId = "key:grid";
    grid.key = "grid";
    grid.name = "Grid";
    grid.controlType = "DataGrid";
    grid.bounds = { 180, 125, 100, 80 };
    grid.parentIndex = 0;
    grid.patterns = ViewAccessibilityPattern::Grid;
    grid.gridRowCount = 1;
    grid.gridColumnCount = 2;
    grid.children = { 11, 12 };

    ViewAccessibilityNode cellA;
    cellA.semanticId = "key:cell-a";
    cellA.key = "cell-a";
    cellA.name = "Cell A";
    cellA.controlType = "DataItem";
    cellA.bounds = { 180, 125, 50, 80 };
    cellA.parentIndex = 10;
    cellA.patterns = ViewAccessibilityPattern::GridItem;
    cellA.gridRow = 0;
    cellA.gridColumn = 0;
    cellA.gridRowSpan = 1;
    cellA.gridColumnSpan = 1;

    ViewAccessibilityNode cellB = cellA;
    cellB.semanticId = "key:cell-b";
    cellB.key = "cell-b";
    cellB.name = "Cell B";
    cellB.bounds.x = 230;
    cellB.gridColumn = 1;

    ViewAccessibilityNode multi;
    multi.semanticId = "key:messages";
    multi.key = "messages";
    multi.name = "Messages";
    multi.controlType = "List";
    multi.bounds = { 10, 300, 160, 40 };
    multi.parentIndex = 0;
    multi.patterns = ViewAccessibilityPattern::Selection;
    multi.canSelectMultiple = true;
    multi.children = { 14, 15 };

    ViewAccessibilityNode messageA;
    messageA.semanticId = "key:message-a";
    messageA.key = "message-a";
    messageA.name = "Message A";
    messageA.controlType = "ListItem";
    messageA.bounds = { 10, 300, 80, 40 };
    messageA.parentIndex = 13;
    messageA.patterns = ViewAccessibilityPattern::SelectionItem;
    messageA.checked = true;
    messageA.canSelectMultiple = true;

    ViewAccessibilityNode messageB = messageA;
    messageB.semanticId = "key:message-b";
    messageB.key = "message-b";
    messageB.name = "Message B";
    messageB.bounds.x = 90;
    messageB.checked = false;

    widget.bounds = RECT{ 10, 20, 310, 350 };
    widget.nodes = { group, button, status, toggle, slider, input, combo,
        optionA, optionB, scroll, grid, cellA, cellB, multi,
        messageA, messageB };
    return widget;
}

bool ContainsChange(
    const std::vector<snowdesktop::WidgetAccessibilityChange>& changes,
    WidgetAccessibilityChangeKind kind,
    WidgetAccessibilityElementKind element,
    std::string_view semanticId = {})
{
    return std::any_of(changes.begin(), changes.end(),
        [&](const auto& change) {
            return change.kind == kind && change.element == element &&
                change.semanticId == semanticId;
        });
}

void TestSnapshotDiff()
{
    const std::vector<LuaWidgetAccessibilitySnapshot> previous{
        Snapshot() };
    auto current = previous;
    Check(snowdesktop::DiffWidgetAccessibilitySnapshots(
            previous, current).empty(),
        "identical accessibility frames must not emit changes");

    current[0].name = "Renamed widget";
    current[0].bounds.right += 10;
    current[0].nodes[1].focused = false;
    current[0].nodes[3].focused = true;
    current[0].nodes[3].checked = true;
    current[0].nodes[4].value = 6.0f;
    current[0].nodes[5].valueText = "after";
    current[0].nodes[5].required = false;
    current[0].nodes[6].expanded = true;
    current[0].nodes[2].name = "Updated";
    current[0].nodes[2].role = "alert";
    current[0].nodes[2].valueText = "Attention required";
    current[0].nodes[2].helpText = "Updated status";
    current[0].nodes[2].headingLevel = 3;
    current[0].nodes[2].live = AccessibilityLive::Assertive;
    current[0].nodes[2].positionInSet = 2;
    current[0].nodes[2].setSize = 5;
    current[0].nodes[2].enabled = false;
    current[0].nodes[2].offscreen = true;
    current[0].nodes[2].bounds.x += 5.0f;
    current[0].nodes[9].scrollOffset = 0.0f;
    current[0].nodes[9].scrollContentExtent = 100.0f;
    const auto changes = snowdesktop::DiffWidgetAccessibilitySnapshots(
        previous, current);
    Check(ContainsChange(changes,
            WidgetAccessibilityChangeKind::Name,
            WidgetAccessibilityElementKind::Widget) &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::Bounds,
                WidgetAccessibilityElementKind::Widget) &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::Focus,
                WidgetAccessibilityElementKind::Node, "key:enabled") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::Toggle,
                WidgetAccessibilityElementKind::Node, "key:enabled") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::RangeValue,
                WidgetAccessibilityElementKind::Node, "key:level") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::Value,
                WidgetAccessibilityElementKind::Node, "key:query") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::Required,
                WidgetAccessibilityElementKind::Node, "key:query") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::ExpandCollapse,
                WidgetAccessibilityElementKind::Node, "key:choice") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::Enabled,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::AriaRole,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::HelpText,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::ItemStatus,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::HeadingLevel,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::LiveSetting,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::PositionInSet,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::SizeOfSet,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::LiveRegion,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::Offscreen,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::Bounds,
                WidgetAccessibilityElementKind::Node, "path:0/1") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::VerticalScrollPercent,
                WidgetAccessibilityElementKind::Node,
                "key:feed-scroll") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::VerticalViewSize,
                WidgetAccessibilityElementKind::Node,
                "key:feed-scroll") &&
            ContainsChange(changes,
                WidgetAccessibilityChangeKind::VerticallyScrollable,
                WidgetAccessibilityElementKind::Node,
                "key:feed-scroll"),
        "snapshot diff must classify focus, property, state, and bounds changes");

    current[0].nodes.pop_back();
    const auto structure = snowdesktop::DiffWidgetAccessibilitySnapshots(
        previous, current);
    Check(ContainsChange(structure,
            WidgetAccessibilityChangeKind::Structure,
            WidgetAccessibilityElementKind::Root),
        "semantic additions, removals, and ordering changes must invalidate structure");
}

void TestIndividualEventInputs()
{
    using Kind = WidgetAccessibilityChangeKind;
    using Element = WidgetAccessibilityElementKind;
    struct Mutation
    {
        const char* description;
        std::function<void(LuaWidgetAccessibilitySnapshot&)> apply;
        Kind kind;
        Element element = Element::Node;
        std::string_view semanticId = "path:0/1";
    };
    const Mutation mutations[] = {
        { "widget identity", [](auto& s) { s.widgetId += L"-new"; }, Kind::Structure, Element::Root, {} },
        { "package identity", [](auto& s) { s.packageId += ".new"; }, Kind::Structure, Element::Root, {} },
        { "widget name", [](auto& s) { s.name += " new"; }, Kind::Name, Element::Widget, {} },
        { "widget left", [](auto& s) { ++s.bounds.left; }, Kind::Bounds, Element::Widget, {} },
        { "widget top", [](auto& s) { ++s.bounds.top; }, Kind::Bounds, Element::Widget, {} },
        { "widget right", [](auto& s) { ++s.bounds.right; }, Kind::Bounds, Element::Widget, {} },
        { "widget bottom", [](auto& s) { ++s.bounds.bottom; }, Kind::Bounds, Element::Widget, {} },
        { "node count", [](auto& s) { s.nodes.pop_back(); }, Kind::Structure, Element::Root, {} },
        { "node order", [](auto& s) { std::swap(s.nodes[1], s.nodes[2]); }, Kind::Structure, Element::Root, {} },
        { "semantic identity", [](auto& s) { s.nodes[2].semanticId += "-new"; }, Kind::Structure, Element::Root, {} },
        { "semantic parent", [](auto& s) { s.nodes[2].parentIndex = 1; }, Kind::Structure, Element::Root, {} },
        { "control type", [](auto& s) { s.nodes[2].controlType = "Pane"; }, Kind::Structure, Element::Root, {} },
        { "patterns", [](auto& s) { s.nodes[2].patterns = ViewAccessibilityPattern::Value; }, Kind::Structure, Element::Root, {} },
        { "label relation", [](auto& s) { s.nodes[2].labelledBySemanticId = "key:open"; }, Kind::Structure, Element::Root, {} },
        { "description relation", [](auto& s) { s.nodes[2].describedBySemanticId = "key:open"; }, Kind::Structure, Element::Root, {} },
        { "focus loss", [](auto& s) { s.nodes[1].focused = false; }, Kind::Focus, Element::Root, {} },
        { "name", [](auto& s) { s.nodes[2].name += " new"; }, Kind::Name },
        { "role", [](auto& s) { s.nodes[2].role = "alert"; }, Kind::AriaRole },
        { "help", [](auto& s) { s.nodes[2].helpText += " new"; }, Kind::HelpText },
        { "status", [](auto& s) { s.nodes[2].valueText += " new"; }, Kind::ItemStatus },
        { "heading", [](auto& s) { ++s.nodes[2].headingLevel; }, Kind::HeadingLevel },
        { "live setting", [](auto& s) { s.nodes[2].live = AccessibilityLive::Assertive; }, Kind::LiveSetting },
        { "position", [](auto& s) { s.nodes[2].positionInSet = 2; }, Kind::PositionInSet },
        { "set size", [](auto& s) { s.nodes[2].setSize = 5; }, Kind::SizeOfSet },
        { "enabled", [](auto& s) { s.nodes[2].enabled = false; }, Kind::Enabled },
        { "required", [](auto& s) { s.nodes[2].required = true; }, Kind::Required },
        { "offscreen", [](auto& s) { s.nodes[2].offscreen = true; }, Kind::Offscreen },
        { "checked", [](auto& s) { s.nodes[3].checked = true; }, Kind::Toggle, Element::Node, "key:enabled" },
        { "selected", [](auto& s) { s.nodes[7].checked = true; }, Kind::SelectionItem, Element::Node, "key:choice/a" },
        { "range value", [](auto& s) { s.nodes[4].value = 5.f; }, Kind::RangeValue, Element::Node, "key:level" },
        { "edit value", [](auto& s) { s.nodes[5].valueText += " new"; }, Kind::Value, Element::Node, "key:query" },
        { "expanded", [](auto& s) { s.nodes[6].expanded = true; }, Kind::ExpandCollapse, Element::Node, "key:choice" },
        { "node x", [](auto& s) { ++s.nodes[2].bounds.x; }, Kind::Bounds },
        { "node y", [](auto& s) { ++s.nodes[2].bounds.y; }, Kind::Bounds },
        { "node width", [](auto& s) { ++s.nodes[2].bounds.width; }, Kind::Bounds },
        { "node height", [](auto& s) { ++s.nodes[2].bounds.height; }, Kind::Bounds },
        { "scroll axis", [](auto& s) { s.nodes[9].scrollHorizontal = true; }, Kind::HorizontalScrollPercent, Element::Node, "key:feed-scroll" },
        { "scroll offset", [](auto& s) { ++s.nodes[9].scrollOffset; }, Kind::VerticalScrollPercent, Element::Node, "key:feed-scroll" },
        { "scroll viewport", [](auto& s) { ++s.nodes[9].scrollViewportExtent; }, Kind::VerticalViewSize, Element::Node, "key:feed-scroll" },
        { "scroll content", [](auto& s) { s.nodes[9].scrollContentExtent = 100.f; }, Kind::VerticallyScrollable, Element::Node, "key:feed-scroll" },
    };
    const std::vector<LuaWidgetAccessibilitySnapshot> previous{ Snapshot() };
    for (const auto& mutation : mutations)
    {
        auto current = previous;
        mutation.apply(current[0]);
        const auto changes = snowdesktop::DiffWidgetAccessibilitySnapshots(previous, current);
        Check(ContainsChange(changes, mutation.kind, mutation.element,
            mutation.semanticId), mutation.description);
    }
    auto adapterOnly = previous;
    adapterOnly[0].nodes[5].textAccess =
        std::make_shared<snowdesktop::text_input::TextAccess>();
    Check(snowdesktop::DiffWidgetAccessibilitySnapshots(previous, adapterOnly).empty(),
        "fresh text query adapters alone must not create events");
}

void TestEventListeningBaseline()
{
    // RefreshEvents uses this state directly. Only OS listening and runtime
    // capture/failure are injected here; native event delivery needs a client.
    snowdesktop::WidgetAccessibilityEventState events;
    std::vector<LuaWidgetAccessibilitySnapshot> snapshots{ Snapshot() };
    int captures = 0;
    bool captureSucceeds = true;
    const LuaWidgetAccessibilitySnapshot* captured = nullptr;
    const auto capture = [&](auto& output) {
        ++captures;
        output = snapshots;
        captured = output.data();
        return captureSucceeds;
    };
    const auto refresh = [&](bool listening) {
        return events.Refresh(capture, [listening] { return listening; });
    };
    for (int frame = 0; frame < 5; ++frame)
        Check(refresh(false).changes.empty(),
            "unobserved frames must emit no events");
    Check(captures == 5 && events.Current().data() == captured,
        "unobserved frames must move the live baseline without diffing or copying snapshots");

    snapshots[0].nodes[5].valueText = "before client connected";
    Check(refresh(false).changes.empty(),
        "changes before client connection must update the baseline silently");
    auto update = refresh(true);
    Check(captures == 7 && events.Current().data() == captured &&
            events.Current()[0].nodes[5].valueText == "before client connected" &&
            update.previous.empty() && update.changes.empty(),
        "late listeners must use the current baseline without replaying historical events or copying snapshots");
    update = refresh(true);
    Check(update.changes.empty() && update.previous.empty() && captures == 8 &&
            events.Current().data() == captured,
        "unchanged observed frames must keep the moved live baseline without events");

    const auto* previousStorage = events.Current().data();
    snapshots[0].bounds.left += 10;
    snapshots[0].nodes[1].focused = false;
    snapshots[0].nodes[5].focused = true;
    snapshots[0].nodes[5].valueText = "after client connected";
    update = refresh(true);
    Check(update.changes.size() == 3 &&
            ContainsChange(update.changes, WidgetAccessibilityChangeKind::Bounds,
                WidgetAccessibilityElementKind::Widget) &&
            ContainsChange(update.changes, WidgetAccessibilityChangeKind::Focus,
                WidgetAccessibilityElementKind::Node, "key:query") &&
            ContainsChange(update.changes, WidgetAccessibilityChangeKind::Value,
                WidgetAccessibilityElementKind::Node, "key:query") &&
            update.previous.data() == previousStorage &&
            update.previous[0].nodes[5].valueText == "before client connected" &&
            events.Current().data() == captured,
        "observed property, bounds and focus transitions must preserve exact old/new values by moving snapshots");

    const auto successful = snapshots;
    snapshots[0].name = "retry after capture failure";
    captureSucceeds = false;
    Check(refresh(true).changes.empty() &&
            events.Current()[0].name == successful[0].name,
        "failed captures must preserve the last successful baseline");
    captureSucceeds = true;
    update = refresh(true);
    Check(update.changes.size() == 1 &&
            ContainsChange(update.changes, WidgetAccessibilityChangeKind::Name,
                WidgetAccessibilityElementKind::Widget) &&
            update.previous[0].name == successful[0].name &&
            events.Current()[0].name == "retry after capture failure",
        "the next successful capture must retry every change since the successful baseline");

    const int beforeDisconnect = captures;
    Check(refresh(false).changes.empty() &&
            captures == beforeDisconnect + 1 &&
            events.Current()[0].name == "retry after capture failure",
        "disconnect must continue maintaining a fresh baseline without emitting changes");
    snapshots[0].name = "new client current name";
    snapshots[0].nodes[5].valueText = "new client current value";
    Check(refresh(false).changes.empty(),
        "disconnected changes must refresh the baseline without events");
    update = refresh(true);
    Check(update.previous.empty() && update.changes.empty() &&
            events.Current()[0].name == "new client current name",
        "reconnected listeners must not receive stale name, value, focus or structure events");
    snapshots[0].nodes[5].valueText = "changed immediately after subscribing";
    update = refresh(true);
    Check(update.changes.size() == 1 &&
            ContainsChange(update.changes, WidgetAccessibilityChangeKind::Value,
                WidgetAccessibilityElementKind::Node, "key:query") &&
            update.previous[0].nodes[5].valueText == "new client current value",
        "a change between subscription and the first paint must retain its property event and old value");
    // A client can also connect while runtime snapshot capture is in progress.
    bool listeningDuringCapture = false;
    update = events.Refresh([&](auto& output) {
        output = snapshots;
        output[0].nodes[5].valueText = "changed while client connected";
        listeningDuringCapture = true;
        return true;
    }, [&] { return listeningDuringCapture; });
    Check(update.changes.size() == 1 &&
            ContainsChange(update.changes, WidgetAccessibilityChangeKind::Value,
                WidgetAccessibilityElementKind::Node, "key:query"),
        "listener state must be checked after capture to retain connections made during capture");
    snapshots.clear();
    update = refresh(true);
    Check(ContainsChange(update.changes, WidgetAccessibilityChangeKind::Structure,
            WidgetAccessibilityElementKind::Root) &&
            ContainsChange(update.changes, WidgetAccessibilityChangeKind::Focus,
                WidgetAccessibilityElementKind::Root),
        "removing the final focused widget must still invalidate structure and return focus to the root");
    events.Reset();
    Check(events.Current().empty(), "window identity changes must clear the event baseline");
}

void TestProviderTreeAndLifetime()
{
    const HRESULT initialized = OleInitialize(nullptr);
    Check(SUCCEEDED(initialized), "OLE must initialize for the provider test");
    HWND window = CreateWindowExW(0, L"STATIC", L"provider-test",
        WS_POPUP, 100, 100, 400, 300,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "provider test window must be created");

    std::vector<LuaWidgetAccessibilitySnapshot> snapshots{ Snapshot() };
    std::wstring focusedWidget;
    std::string focusedKey;
    std::vector<LuaWidgetAccessibilityActionRequest> actions;
    int captures = 0;
    WidgetAccessibilityProviderHost host(
        [&]() { ++captures; return snapshots; },
        [&](const std::wstring& widgetId, const std::string& nodeKey) {
            focusedWidget = widgetId;
            focusedKey = nodeKey;
            return true;
        },
        [&](const LuaWidgetAccessibilityActionRequest& request) {
            actions.push_back(request);
            return true;
        });
    Check(host.AttachWindow(window),
        "provider host must attach to a valid window");
    Check(captures == 1, "attaching a provider must seed its event baseline before a client can subscribe");

    IRawElementProviderSimple* rootSimple = host.RootProvider();
    Check(rootSimple != nullptr, "attached provider must expose a root");
    ProviderOptions rootOptions{};
    Check(SUCCEEDED(rootSimple->get_ProviderOptions(&rootOptions)) &&
            (rootOptions & ProviderOptions_UseComThreading) != 0,
        "root UIA callbacks must respect the owning COM apartment");
    Check(PropertyString(rootSimple, UIA_AutomationIdPropertyId) ==
            L"SnowDesktop.Desktop",
        "root provider must expose its desktop automation id");

    ComPtr<IRawElementProviderFragment> root;
    Check(SUCCEEDED(rootSimple->QueryInterface(IID_PPV_ARGS(&root))),
        "root must expose fragment navigation");
    ComPtr<IRawElementProviderFragment> widget;
    Check(SUCCEEDED(root->Navigate(
            NavigateDirection_FirstChild, &widget)) && widget,
        "root first child must be the visible Lua widget");
    Check(PropertyString(AsSimple(widget.Get()).Get(),
            UIA_NamePropertyId) == L"Sample widget",
        "widget provider must expose the package display name");

    ComPtr<IRawElementProviderFragment> group;
    ComPtr<IRawElementProviderFragment> button;
    ComPtr<IRawElementProviderFragment> status;
    Check(SUCCEEDED(widget->Navigate(
            NavigateDirection_FirstChild, &group)) && group &&
            SUCCEEDED(group->Navigate(
                NavigateDirection_FirstChild, &button)) && button &&
            SUCCEEDED(button->Navigate(
                NavigateDirection_NextSibling, &status)) && status,
        "widget semantic hierarchy must support child and sibling navigation");
    ProviderOptions nodeOptions{};
    Check(SUCCEEDED(AsSimple(button.Get())->get_ProviderOptions(&nodeOptions)) &&
            (nodeOptions & ProviderOptions_UseComThreading) != 0,
        "fragment UIA callbacks must respect the owning COM apartment");
    Check(PropertyString(AsSimple(button.Get()).Get(),
            UIA_AutomationIdPropertyId) == L"open" &&
            PropertyString(AsSimple(status.Get()).Get(),
                UIA_AutomationIdPropertyId) == L"path:0/1",
        "keyed and keyless elements must expose deterministic automation ids");
    Check(PropertyString(AsSimple(button.Get()).Get(),
            UIA_AccessKeyPropertyId) == L"Alt+O" &&
            PropertyString(AsSimple(button.Get()).Get(),
                UIA_AcceleratorKeyPropertyId) == L"Ctrl+O",
        "action elements must expose access-key and accelerator metadata");
    Check(PropertyString(AsSimple(group.Get()).Get(),
            UIA_AriaPropertiesPropertyId) == L"busy=true",
        "busy view state must be exposed without localized placeholder text");
    Check(PropertyString(AsSimple(status.Get()).Get(),
                UIA_AriaRolePropertyId) == L"status" &&
            PropertyString(AsSimple(status.Get()).Get(),
                UIA_ItemStatusPropertyId) == L"All systems ready" &&
            PropertyString(AsSimple(status.Get()).Get(),
                UIA_HelpTextPropertyId) == L"Current system status" &&
            PropertyInt(AsSimple(status.Get()).Get(),
                UIA_HeadingLevelPropertyId) == HeadingLevel2 &&
            PropertyInt(AsSimple(status.Get()).Get(),
                UIA_LiveSettingPropertyId) == Polite &&
            PropertyInt(AsSimple(status.Get()).Get(),
                UIA_PositionInSetPropertyId) == 1 &&
            PropertyInt(AsSimple(status.Get()).Get(),
                UIA_SizeOfSetPropertyId) == 4,
        "semantic metadata must map to native UIA properties");

    SAFEARRAY* buttonId = nullptr;
    SAFEARRAY* statusId = nullptr;
    Check(SUCCEEDED(button->GetRuntimeId(&buttonId)) && buttonId &&
            SUCCEEDED(status->GetRuntimeId(&statusId)) && statusId,
        "fragment elements must expose runtime ids");
    LONG* buttonValues = nullptr;
    LONG* statusValues = nullptr;
    Check(SUCCEEDED(SafeArrayAccessData(buttonId,
            reinterpret_cast<void**>(&buttonValues))) &&
            SUCCEEDED(SafeArrayAccessData(statusId,
                reinterpret_cast<void**>(&statusValues))) &&
            (buttonValues[2] != statusValues[2] ||
                buttonValues[3] != statusValues[3]),
        "different semantic elements must have different runtime ids");
    SafeArrayUnaccessData(buttonId);
    SafeArrayUnaccessData(statusId);
    SafeArrayDestroy(buttonId);
    SafeArrayDestroy(statusId);

    ComPtr<IRawElementProviderFragmentRoot> fragmentRoot;
    Check(SUCCEEDED(rootSimple->QueryInterface(
            IID_PPV_ARGS(&fragmentRoot))),
        "root must expose point and focus lookup");
    POINT origin{};
    Check(ClientToScreen(window, &origin),
        "test window must provide a client origin");
    ComPtr<IRawElementProviderFragment> hit;
    Check(SUCCEEDED(fragmentRoot->ElementProviderFromPoint(
            origin.x + 25, origin.y + 45, &hit)) && hit &&
            PropertyString(AsSimple(hit.Get()).Get(),
                UIA_AutomationIdPropertyId) == L"open",
        "point lookup must return the deepest visible semantic element");
    ComPtr<IRawElementProviderFragment> focused;
    Check(SUCCEEDED(fragmentRoot->GetFocus(&focused)) && focused &&
            PropertyString(AsSimple(focused.Get()).Get(),
                UIA_AutomationIdPropertyId) == L"open",
        "focus lookup must follow the host semantic focus snapshot");
    Check(SUCCEEDED(button->SetFocus()) &&
            focusedWidget == L"widget-a" && focusedKey == "open",
        "fragment focus must route through the host focus callback");

    ComPtr<IUnknown> pattern;
    Check(SUCCEEDED(AsSimple(button.Get())->GetPatternProvider(
            UIA_InvokePatternId, &pattern)) && pattern,
        "button nodes must expose the Invoke pattern");
    ComPtr<IInvokeProvider> invoke;
    Check(SUCCEEDED(pattern.As(&invoke)) && SUCCEEDED(invoke->Invoke()) &&
            actions.size() == 1 &&
            actions.back().kind ==
                LuaWidgetAccessibilityActionKind::Invoke &&
            actions.back().nodeKey == "open",
        "Invoke must route the keyed action through the host callback");

    ComPtr<IRawElementProviderFragment> toggle;
    ComPtr<IRawElementProviderFragment> slider;
    ComPtr<IRawElementProviderFragment> input;
    ComPtr<IRawElementProviderFragment> combo;
    Check(SUCCEEDED(status->Navigate(
            NavigateDirection_NextSibling, &toggle)) && toggle &&
            SUCCEEDED(toggle->Navigate(
                NavigateDirection_NextSibling, &slider)) && slider &&
            SUCCEEDED(slider->Navigate(
                NavigateDirection_NextSibling, &input)) && input &&
            SUCCEEDED(input->Navigate(
                NavigateDirection_NextSibling, &combo)) && combo,
        "stateful controls must remain navigable semantic siblings");

    pattern.Reset();
    Check(SUCCEEDED(AsSimple(toggle.Get())->GetPatternProvider(
            UIA_TogglePatternId, &pattern)) && pattern,
        "checkbox nodes must expose the Toggle pattern");
    ComPtr<IToggleProvider> togglePattern;
    ToggleState toggleState = ToggleState_Indeterminate;
    Check(SUCCEEDED(pattern.As(&togglePattern)) &&
            SUCCEEDED(togglePattern->get_ToggleState(&toggleState)) &&
            toggleState == ToggleState_Off &&
            SUCCEEDED(togglePattern->Toggle()) &&
            actions.back().kind ==
                LuaWidgetAccessibilityActionKind::Toggle,
        "Toggle must expose state and route a toggle action");

    pattern.Reset();
    Check(SUCCEEDED(AsSimple(slider.Get())->GetPatternProvider(
            UIA_RangeValuePatternId, &pattern)) && pattern,
        "slider nodes must expose the RangeValue pattern");
    ComPtr<IRangeValueProvider> rangePattern;
    double rangeValue = 0.0;
    double smallChange = 0.0;
    Check(SUCCEEDED(pattern.As(&rangePattern)) &&
            SUCCEEDED(rangePattern->get_Value(&rangeValue)) &&
            rangeValue == 4.0 &&
            SUCCEEDED(rangePattern->get_SmallChange(&smallChange)) &&
            smallChange == 0.5 &&
            rangePattern->SetValue(12.0) == E_INVALIDARG &&
            SUCCEEDED(rangePattern->SetValue(7.0)) &&
            actions.back().kind ==
                LuaWidgetAccessibilityActionKind::SetRangeValue &&
            actions.back().numericValue == 7.0,
        "RangeValue must validate bounds and route numeric values");

    pattern.Reset();
    Check(SUCCEEDED(AsSimple(input.Get())->GetPatternProvider(
            UIA_ValuePatternId, &pattern)) && pattern,
        "input nodes must expose the Value pattern");
    ComPtr<IValueProvider> valuePattern;
    BSTR inputValue = nullptr;
    Check(SUCCEEDED(pattern.As(&valuePattern)) &&
            SUCCEEDED(valuePattern->get_Value(&inputValue)) &&
            std::wstring(inputValue, SysStringLen(inputValue)) == L"before" &&
            PropertyString(AsSimple(input.Get()).Get(),
                UIA_HelpTextPropertyId) == L"Enter a search term" &&
            PropertyBool(AsSimple(input.Get()).Get(),
                UIA_IsRequiredForFormPropertyId) &&
            SUCCEEDED(valuePattern->SetValue(L"after")) &&
            actions.back().kind ==
                LuaWidgetAccessibilityActionKind::SetValue &&
            actions.back().textValue == L"after",
        "Value must expose text and route replacement values");
    SysFreeString(inputValue);

    VARIANT labelledBy;
    VariantInit(&labelledBy);
    Check(SUCCEEDED(AsSimple(input.Get())->GetPropertyValue(
            UIA_LabeledByPropertyId, &labelledBy)) &&
            labelledBy.vt == VT_UNKNOWN && labelledBy.punkVal,
        "LabeledBy must return the referenced element provider");
    ComPtr<IRawElementProviderSimple> labelProvider;
    Check(SUCCEEDED(labelledBy.punkVal->QueryInterface(
            IID_PPV_ARGS(&labelProvider))) &&
            PropertyString(labelProvider.Get(),
                UIA_AutomationIdPropertyId) == L"path:0/1",
        "LabeledBy must preserve the referenced semantic identity");
    VariantClear(&labelledBy);

    VARIANT describedBy;
    VariantInit(&describedBy);
    Check(SUCCEEDED(AsSimple(input.Get())->GetPropertyValue(
            UIA_DescribedByPropertyId, &describedBy)) &&
            describedBy.vt == (VT_ARRAY | VT_UNKNOWN) &&
            describedBy.parray,
        "DescribedBy must return a provider array");
    LONG describedIndex = 0;
    IUnknown* describedUnknown = nullptr;
    Check(SUCCEEDED(SafeArrayGetElement(describedBy.parray,
            &describedIndex, &describedUnknown)) && describedUnknown,
        "DescribedBy must contain the referenced provider");
    ComPtr<IRawElementProviderSimple> descriptionProvider;
    Check(SUCCEEDED(describedUnknown->QueryInterface(
            IID_PPV_ARGS(&descriptionProvider))) &&
            PropertyString(descriptionProvider.Get(),
                UIA_AutomationIdPropertyId) == L"path:0/1",
        "DescribedBy must preserve the referenced semantic identity");
    describedUnknown->Release();
    VariantClear(&describedBy);

    pattern.Reset();
    Check(SUCCEEDED(AsSimple(combo.Get())->GetPatternProvider(
            UIA_ExpandCollapsePatternId, &pattern)) && pattern,
        "select nodes must expose the ExpandCollapse pattern");
    ComPtr<IExpandCollapseProvider> expandPattern;
    ExpandCollapseState expandState = ExpandCollapseState_LeafNode;
    Check(SUCCEEDED(pattern.As(&expandPattern)) &&
            SUCCEEDED(expandPattern->get_ExpandCollapseState(
                &expandState)) &&
            expandState == ExpandCollapseState_Collapsed &&
            SUCCEEDED(expandPattern->Expand()) &&
            actions.back().kind ==
                LuaWidgetAccessibilityActionKind::Expand,
        "ExpandCollapse must expose state and route expansion");

    pattern.Reset();
    Check(SUCCEEDED(AsSimple(combo.Get())->GetPatternProvider(
            UIA_SelectionPatternId, &pattern)) && pattern,
        "selection containers must expose the Selection pattern");
    ComPtr<ISelectionProvider> selectionPattern;
    SAFEARRAY* selection = nullptr;
    Check(SUCCEEDED(pattern.As(&selectionPattern)) &&
            SUCCEEDED(selectionPattern->GetSelection(&selection)) &&
            selection && SafeArrayGetDim(selection) == 1,
        "Selection must return a provider array");
    LONG lower = 0;
    LONG upper = -1;
    Check(SUCCEEDED(SafeArrayGetLBound(selection, 1, &lower)) &&
            SUCCEEDED(SafeArrayGetUBound(selection, 1, &upper)) &&
            lower == 0 && upper == 0,
        "single-selection controls must report one selected child");
    IUnknown* selectedUnknown = nullptr;
    Check(SUCCEEDED(SafeArrayGetElement(selection,
            &lower, &selectedUnknown)) && selectedUnknown,
        "Selection must return the selected child provider");
    ComPtr<IRawElementProviderSimple> selectedSimple;
    Check(SUCCEEDED(selectedUnknown->QueryInterface(
            IID_PPV_ARGS(&selectedSimple))) &&
            PropertyString(selectedSimple.Get(),
                UIA_AutomationIdPropertyId) == L"choice/b",
        "Selection must preserve the selected option identity");
    selectedUnknown->Release();
    SafeArrayDestroy(selection);

    ComPtr<IRawElementProviderFragment> multi;
    Check(SUCCEEDED(group->Navigate(
            NavigateDirection_LastChild, &multi)) && multi,
        "multiple-selection collection must remain a semantic child");
    pattern.Reset();
    Check(SUCCEEDED(AsSimple(multi.Get())->GetPatternProvider(
            UIA_SelectionPatternId, &pattern)) && pattern,
        "multiple-selection collection must expose Selection");
    ComPtr<ISelectionProvider> multiSelection;
    BOOL canSelectMultiple = FALSE;
    Check(SUCCEEDED(pattern.As(&multiSelection)) &&
            SUCCEEDED(multiSelection->get_CanSelectMultiple(
                &canSelectMultiple)) && canSelectMultiple,
        "Selection must report multiple-selection capability");
    ComPtr<IRawElementProviderFragment> messageA;
    Check(SUCCEEDED(multi->Navigate(
            NavigateDirection_FirstChild, &messageA)) && messageA,
        "multiple-selection items must remain navigable");
    pattern.Reset();
    Check(SUCCEEDED(AsSimple(messageA.Get())->GetPatternProvider(
            UIA_SelectionItemPatternId, &pattern)) && pattern,
        "multiple-selection items must expose SelectionItem");
    ComPtr<ISelectionItemProvider> multiItem;
    Check(SUCCEEDED(pattern.As(&multiItem)) &&
            SUCCEEDED(multiItem->RemoveFromSelection()) &&
            actions.back().kind ==
                LuaWidgetAccessibilityActionKind::RemoveFromSelection &&
            SUCCEEDED(multiItem->AddToSelection()) &&
            actions.back().kind ==
                LuaWidgetAccessibilityActionKind::AddToSelection,
        "multiple SelectionItem add/remove must route distinct host actions");

    ComPtr<IRawElementProviderFragment> scroll;
    ComPtr<IRawElementProviderFragment> grid;
    Check(SUCCEEDED(combo->Navigate(
            NavigateDirection_NextSibling, &scroll)) && scroll &&
            SUCCEEDED(scroll->Navigate(
                NavigateDirection_NextSibling, &grid)) && grid,
        "scroll and grid containers must remain navigable siblings");

    pattern.Reset();
    Check(SUCCEEDED(AsSimple(scroll.Get())->GetPatternProvider(
            UIA_ScrollPatternId, &pattern)) && pattern,
        "scroll containers must expose the Scroll pattern");
    ComPtr<IScrollProvider> scrollPattern;
    double verticalPercent = 0.0;
    double verticalViewSize = 0.0;
    BOOL verticallyScrollable = FALSE;
    Check(SUCCEEDED(pattern.As(&scrollPattern)) &&
            SUCCEEDED(scrollPattern->get_VerticalScrollPercent(
                &verticalPercent)) && verticalPercent == 12.5 &&
            SUCCEEDED(scrollPattern->get_VerticalViewSize(
                &verticalViewSize)) &&
            verticalViewSize > 33.0 && verticalViewSize < 34.0 &&
            SUCCEEDED(scrollPattern->get_VerticallyScrollable(
                &verticallyScrollable)) && verticallyScrollable &&
            scrollPattern->SetScrollPercent(0.0, 50.0) ==
                UIA_E_INVALIDOPERATION &&
            SUCCEEDED(scrollPattern->SetScrollPercent(
                UIA_ScrollPatternNoScroll, 50.0)) &&
            actions.back().kind ==
                LuaWidgetAccessibilityActionKind::SetScrollOffset &&
            actions.back().nodeKey == "feed-scroll" &&
            actions.back().numericValue == 100.0 &&
            SUCCEEDED(scrollPattern->Scroll(
                ScrollAmount_NoAmount, ScrollAmount_LargeIncrement)) &&
            actions.back().numericValue == 125.0,
        "Scroll must expose percentages and route bounded pixel offsets");

    pattern.Reset();
    Check(SUCCEEDED(AsSimple(grid.Get())->GetPatternProvider(
            UIA_GridPatternId, &pattern)) && pattern,
        "grid containers must expose the Grid pattern");
    ComPtr<IGridProvider> gridPattern;
    int rowCount = 0;
    int columnCount = 0;
    ComPtr<IRawElementProviderSimple> cell;
    Check(SUCCEEDED(pattern.As(&gridPattern)) &&
            SUCCEEDED(gridPattern->get_RowCount(&rowCount)) &&
            SUCCEEDED(gridPattern->get_ColumnCount(&columnCount)) &&
            rowCount == 1 && columnCount == 2 &&
            SUCCEEDED(gridPattern->GetItem(0, 1, &cell)) && cell &&
            PropertyString(cell.Get(), UIA_AutomationIdPropertyId) ==
                L"cell-b",
        "Grid must expose zero-based dimensions and addressable cells");
    pattern.Reset();
    Check(SUCCEEDED(cell->GetPatternProvider(
            UIA_GridItemPatternId, &pattern)) && pattern,
        "grid children must expose the GridItem pattern");
    ComPtr<IGridItemProvider> gridItemPattern;
    int row = -1;
    int column = -1;
    ComPtr<IRawElementProviderSimple> containingGrid;
    Check(SUCCEEDED(pattern.As(&gridItemPattern)) &&
            SUCCEEDED(gridItemPattern->get_Row(&row)) &&
            SUCCEEDED(gridItemPattern->get_Column(&column)) &&
            row == 0 && column == 1 &&
            SUCCEEDED(gridItemPattern->get_ContainingGrid(
                &containingGrid)) && containingGrid &&
            PropertyString(containingGrid.Get(),
                UIA_AutomationIdPropertyId) == L"grid",
        "GridItem must expose coordinates and its containing grid");

    snapshots[0].nodes[10].gridRowCount = 3;
    snapshots[0].nodes[10].gridColumnCount = 3;
    snapshots[0].nodes[11].gridRowSpan = 2;
    snapshots[0].nodes[11].gridColumnSpan = 2;
    snapshots[0].nodes[12].gridRow = 2;
    snapshots[0].nodes[12].gridColumn = 1;
    snapshots[0].nodes[12].gridColumnSpan = 2;
    for (int queriedRow = 0; queriedRow < 3; ++queriedRow)
        for (int queriedColumn = 0; queriedColumn < 3; ++queriedColumn)
        {
            const wchar_t* expected = queriedRow < 2 && queriedColumn < 2
                ? L"cell-a" : queriedRow == 2 && queriedColumn >= 1 ? L"cell-b" : nullptr;
            ComPtr<IRawElementProviderSimple> covered;
            Check(SUCCEEDED(gridPattern->GetItem(queriedRow, queriedColumn, &covered)),
                "merged-grid coordinates remain valid provider queries");
            Check(expected ? covered && PropertyString(covered.Get(), UIA_AutomationIdPropertyId) == expected
                           : !covered,
                "every occupied coordinate resolves to its covering item; empty coordinates stay empty");
            if (!expected) continue;
            ComPtr<IUnknown> coveredPattern;
            ComPtr<IGridItemProvider> coveredItem;
            int startRow = -1, startColumn = -1, rowSpan = 0, columnSpan = 0;
            Check(SUCCEEDED(covered->GetPatternProvider(UIA_GridItemPatternId, &coveredPattern)) &&
                    SUCCEEDED(coveredPattern.As(&coveredItem)) &&
                    SUCCEEDED(coveredItem->get_Row(&startRow)) &&
                    SUCCEEDED(coveredItem->get_Column(&startColumn)) &&
                    SUCCEEDED(coveredItem->get_RowSpan(&rowSpan)) &&
                    SUCCEEDED(coveredItem->get_ColumnSpan(&columnSpan)) &&
                    (queriedRow < 2 ? startRow == 0 && startColumn == 0 && rowSpan == 2 && columnSpan == 2
                                    : startRow == 2 && startColumn == 1 && rowSpan == 1 && columnSpan == 2),
                "covered cells retain the merged item's origin and full spans through GridItem");
        }
    for (const auto coordinate : {std::pair{-1, 0}, std::pair{0, -1}, std::pair{3, 0}, std::pair{0, 3}})
    {
        ComPtr<IRawElementProviderSimple> outside;
        Check(gridPattern->GetItem(coordinate.first, coordinate.second, &outside) == E_INVALIDARG && !outside,
            "negative and half-open outer grid coordinates remain rejected");
    }

    snapshots[0].nodes[3].checked = true;
    host.RefreshEvents();
    host.RefreshEvents();

    snapshots[0].nodes[5].valueText = "live query after paint";
    const int beforeLiveQuery = captures;
    BSTR liveValue = nullptr;
    Check(SUCCEEDED(valuePattern->get_Value(&liveValue)) &&
            liveValue && std::wstring(liveValue, SysStringLen(liveValue)) == L"live query after paint" &&
            captures > beforeLiveQuery,
        "retained provider queries must capture current values independently of the event baseline");
    SysFreeString(liveValue);

    host.DetachWindow(window);
    VARIANT unavailable;
    VariantInit(&unavailable);
    Check(AsSimple(button.Get())->GetPropertyValue(
            UIA_NamePropertyId, &unavailable) ==
            UIA_E_ELEMENTNOTAVAILABLE,
        "providers retained after window detach must become unavailable");
    VariantClear(&unavailable);
    DestroyWindow(window);
    OleUninitialize();
}

constexpr UINT kDetachProvider = WM_APP + 73;

struct ApartmentFixture
{
    DWORD ownerThread = GetCurrentThreadId();
    std::atomic<unsigned> wrongThreadCalls{ 0 };
    std::atomic<unsigned> captures{ 0 };
    std::atomic<unsigned> focuses{ 0 };
    std::atomic<unsigned> actions{ 0 };
    std::atomic<unsigned> textReads{ 0 };
    unsigned replacements = 0;
    std::wstring text = L"before";
    std::vector<LuaWidgetAccessibilitySnapshot> snapshots{ Snapshot() };
    std::unique_ptr<WidgetAccessibilityProviderHost> host;
    HANDLE detached = nullptr;

    bool OnOwnerThread()
    {
        if (GetCurrentThreadId() == ownerThread) return true;
        ++wrongThreadCalls;
        return false;
    }

    void ReplaceSnapshot()
    {
        auto replacement = Snapshot();
        replacement.nodes[5].valueText = std::string(text.begin(), text.end());
        auto access = std::make_shared<snowdesktop::text_input::TextAccess>();
        access->document = [this]() ->
            std::optional<snowdesktop::text_input::AccessibleDocument> {
            if (!OnOwnerThread()) return {};
            ++textReads;
            snowdesktop::text_input::AccessibleDocument document;
            document.text = text;
            return document;
        };
        replacement.nodes[5].textAccess = std::move(access);
        snapshots = { std::move(replacement) };
        ++replacements;
    }
};

LRESULT CALLBACK ApartmentWindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* fixture = reinterpret_cast<ApartmentFixture*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (fixture && fixture->host)
    {
        LRESULT result = 0;
        if (message == WM_GETOBJECT &&
            fixture->host->TryHandleGetObject(window, wParam, lParam, result))
            return result;
        if (message == kDetachProvider)
        {
            fixture->host->DetachWindow(window);
            fixture->host.reset();
            SetEvent(fixture->detached);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

ComPtr<IUIAutomationElement> FindClientElement(
    IUIAutomation* automation, IUIAutomationElement* root,
    const wchar_t* automationId)
{
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(automationId);
    ComPtr<IUIAutomationCondition> condition;
    const HRESULT conditionResult = automation->CreatePropertyCondition(
        UIA_AutomationIdPropertyId, value, &condition);
    VariantClear(&value);
    Check(SUCCEEDED(conditionResult), "client property condition must be created");
    ComPtr<IUIAutomationElement> element;
    Check(SUCCEEDED(root->FindFirst(TreeScope_Descendants,
            condition.Get(), &element)) && element,
        "real UIA must navigate the private fixture's provider tree");
    return element;
}

void TestRealUiaApartmentAndLifetime()
{
    Check(SUCCEEDED(OleInitialize(nullptr)),
        "the UIA provider fixture must own an OLE STA");
    ApartmentFixture fixture;
    fixture.detached = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(fixture.detached && finished, "fixture completion events must exist");
    fixture.ReplaceSnapshot();
    fixture.host = std::make_unique<WidgetAccessibilityProviderHost>(
        [&]() {
            if (!fixture.OnOwnerThread())
                return std::vector<LuaWidgetAccessibilitySnapshot>{};
            ++fixture.captures;
            return fixture.snapshots;
        },
        [&](const std::wstring&, const std::string&) {
            if (!fixture.OnOwnerThread()) return false;
            ++fixture.focuses;
            return true;
        },
        [&](const LuaWidgetAccessibilityActionRequest& request) {
            if (!fixture.OnOwnerThread()) return false;
            ++fixture.actions;
            if (request.kind == LuaWidgetAccessibilityActionKind::SetValue)
                fixture.text = request.textValue;
            fixture.ReplaceSnapshot();
            return true;
        });
    const wchar_t* className = L"SnowDesktopUiaApartmentTest";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = ApartmentWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    Check(RegisterClassW(&windowClass) != 0,
        "the isolated provider window class must register");
    HWND window = CreateWindowExW(0, className, L"UIA apartment fixture",
        WS_POPUP, 0, 0, 400, 400, nullptr, nullptr,
        windowClass.hInstance, &fixture);
    Check(window && fixture.host->AttachWindow(window),
        "the fixture provider must attach to its private hidden window");

    std::thread client([&]() {
        Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
            "the real UIA client must use a separate MTA");
        {
            ComPtr<IUIAutomation> automation;
            Check(SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))),
                "the Windows UIA client must initialize");
            ComPtr<IUIAutomationElement> root;
            Check(SUCCEEDED(automation->ElementFromHandle(window, &root)) && root,
                "UIA must resolve only the private fixture HWND");
            auto button = FindClientElement(automation.Get(), root.Get(), L"open");
            auto input = FindClientElement(automation.Get(), root.Get(), L"query");
            ComPtr<IUIAutomationInvokePattern> invoke;
            ComPtr<IUIAutomationValuePattern> value;
            ComPtr<IUIAutomationTextPattern> text;
            Check(SUCCEEDED(button->GetCurrentPatternAs(UIA_InvokePatternId,
                    IID_PPV_ARGS(&invoke))) && invoke &&
                    SUCCEEDED(input->GetCurrentPatternAs(UIA_ValuePatternId,
                        IID_PPV_ARGS(&value))) && value &&
                    SUCCEEDED(input->GetCurrentPatternAs(UIA_TextPatternId,
                        IID_PPV_ARGS(&text))) && text,
                "real UIA must marshal retained invoke, value and text patterns");
            for (unsigned index = 0; index < 20; ++index)
            {
                SAFEARRAY* runtimeId = nullptr;
                BSTR name = nullptr;
                Check(SUCCEEDED(button->GetRuntimeId(&runtimeId)) && runtimeId &&
                        SUCCEEDED(button->get_CurrentName(&name)) && name &&
                        std::wstring(name, SysStringLen(name)) == L"Open",
                    "background runtime-id and property queries must survive snapshot replacement");
                SafeArrayDestroy(runtimeId);
                SysFreeString(name);
                Check(SUCCEEDED(invoke->Invoke()),
                    "background UIA invoke must reach the owning STA");
            }
            Check(SUCCEEDED(button->SetFocus()) && fixture.focuses > 0,
                "background UIA focus must reach the owning STA");
            BSTR replacementValue = SysAllocString(L"after");
            Check(replacementValue != nullptr,
                "the client value argument must be an allocated BSTR");
            const HRESULT writeResult = value->SetValue(replacementValue);
            SysFreeString(replacementValue);
            Check(SUCCEEDED(writeResult),
                "background UIA value writes must reach the owning STA");
            BSTR currentValue = nullptr;
            Check(SUCCEEDED(value->get_CurrentValue(&currentValue)) &&
                    currentValue &&
                    std::wstring(currentValue, SysStringLen(currentValue)) == L"after",
                "marshalling must preserve live values without waiting for paint events");
            SysFreeString(currentValue);
            ComPtr<IUIAutomationTextRange> range;
            BSTR currentText = nullptr;
            Check(SUCCEEDED(text->get_DocumentRange(&range)) && range &&
                    SUCCEEDED(range->GetText(-1, &currentText)) && currentText &&
                    std::wstring(currentText, SysStringLen(currentText)) == L"after" &&
                    SUCCEEDED(range->Select()),
                "retained text ranges must read and select through the owning STA");
            SysFreeString(currentText);
            Check(fixture.wrongThreadCalls == 0 && fixture.textReads > 0,
                "no UIA snapshot, focus, action or text callback may run outside its STA");

            Check(PostMessageW(window, kDetachProvider, 0, 0) &&
                    WaitForSingleObject(fixture.detached, 10000) == WAIT_OBJECT_0,
                "the owner must detach and destroy the host while clients retain providers");
            const unsigned captures = fixture.captures;
            const unsigned actions = fixture.actions;
            currentText = nullptr;
            const HRESULT textResult = range->GetText(-1, &currentText);
            SysFreeString(currentText);
            Check(FAILED(invoke->Invoke()) && FAILED(textResult) &&
                    fixture.captures == captures && fixture.actions == actions,
                "retained patterns and text ranges must fail without touching the destroyed host");
        }
        CoUninitialize();
        SetEvent(finished);
    });

    const ULONGLONG deadline = GetTickCount64() + 20000;
    while (WaitForSingleObject(finished, 0) == WAIT_TIMEOUT)
    {
        Check(GetTickCount64() < deadline,
            "cross-apartment UIA calls must complete without deadlocking the GUI thread");
        if (fixture.host) fixture.ReplaceSnapshot();
        (void)MsgWaitForMultipleObjects(1, &finished, FALSE, 25, QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    client.join();
    Check(fixture.wrongThreadCalls == 0 && fixture.captures > 20 &&
            fixture.actions >= 21 && fixture.replacements > 20,
        "real UIA traffic must exercise live owner-thread reads, writes and replacement");
    DestroyWindow(window);
    UnregisterClassW(className, windowClass.hInstance);
    CloseHandle(finished);
    CloseHandle(fixture.detached);
    OleUninitialize();
}
}

int main()
{
    TestSnapshotDiff();
    TestIndividualEventInputs();
    TestEventListeningBaseline();
    TestProviderTreeAndLifetime();
    TestRealUiaApartmentAndLifetime();
    std::cout << "widget accessibility provider tests passed\n";
    return 0;
}
