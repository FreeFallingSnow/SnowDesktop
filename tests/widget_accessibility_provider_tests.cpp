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
    group.bounds = { 0, 0, 200, 150 };
    group.children = { 1, 2 };

    ViewAccessibilityNode button;
    button.semanticId = "key:open";
    button.key = "open";
    button.name = "Open";
    button.controlType = "Button";
    button.bounds = { 10, 10, 80, 30 };
    button.parentIndex = 0;
    button.focusable = true;
    button.focused = true;

    ViewAccessibilityNode status;
    status.semanticId = "path:0/1";
    status.name = "Ready";
    status.controlType = "Text";
    status.bounds = { 10, 60, 100, 20 };
    status.parentIndex = 0;

    widget.nodes = { group, button, status };
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
    WidgetAccessibilityProviderHost host(
        [&]() { return snapshots; },
        [&](const std::wstring& widgetId, const std::string& nodeKey) {
            focusedWidget = widgetId;
            focusedKey = nodeKey;
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
