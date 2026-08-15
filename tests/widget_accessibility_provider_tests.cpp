#include <ole2.h>
#include <UIAutomation.h>

#include "widget_accessibility_provider.h"

#include <oleauto.h>
#include <wrl/client.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using snowdesktop::WidgetAccessibilityProviderHost;
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
    group.controlType = "Group";
    group.bounds = { 0, 0, 300, 330 };
    group.children = { 1, 2, 3, 4, 5, 6 };

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
    input.valueReadOnly = false;

    ViewAccessibilityNode combo;
    combo.semanticId = "key:choice";
    combo.key = "choice";
    combo.name = "Choice";
    combo.controlType = "ComboBox";
    combo.bounds = { 10, 200, 160, 28 };
    combo.parentIndex = 0;
    combo.patterns = ViewAccessibilityPattern::ExpandCollapse;
    combo.expanded = false;

    widget.bounds = RECT{ 10, 20, 310, 350 };
    widget.nodes = { group, button, status, toggle, slider, input, combo };
    return widget;
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
    TestProviderTreeAndLifetime();
    std::cout << "widget accessibility provider tests passed\n";
    return 0;
}
