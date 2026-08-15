#include <ole2.h>
#include <UIAutomation.h>

#include "widget_accessibility_provider.h"
#include "widget_accessibility_events.h"

#include <oleauto.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using snowdesktop::WidgetAccessibilityProviderHost;
using snowdesktop::WidgetAccessibilityChangeKind;
using snowdesktop::WidgetAccessibilityElementKind;
using snowdesktop::widget_runtime::ViewAccessibilityNode;
using snowdesktop::widget_runtime::ViewAccessibilityPattern;

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

    ViewAccessibilityNode status;
    status.semanticId = "path:0/1";
    status.name = "Ready";
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
    WidgetAccessibilityProviderHost host(
        [&]() { return snapshots; },
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

    IRawElementProviderSimple* rootSimple = host.RootProvider();
    Check(rootSimple != nullptr, "attached provider must expose a root");
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
    Check(PropertyString(AsSimple(button.Get()).Get(),
            UIA_AutomationIdPropertyId) == L"open" &&
            PropertyString(AsSimple(status.Get()).Get(),
                UIA_AutomationIdPropertyId) == L"path:0/1",
        "keyed and keyless elements must expose deterministic automation ids");
    Check(PropertyString(AsSimple(group.Get()).Get(),
            UIA_AriaPropertiesPropertyId) == L"busy=true",
        "busy view state must be exposed without localized placeholder text");

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

    snapshots[0].nodes[3].checked = true;
    host.RefreshEvents();
    host.RefreshEvents();

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
}

int main()
{
    TestSnapshotDiff();
    TestProviderTreeAndLifetime();
    std::cout << "widget accessibility provider tests passed\n";
    return 0;
}
