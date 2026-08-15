#include <ole2.h>
#include <UIAutomation.h>
#include <UIAutomationCoreApi.h>

#include "widget_accessibility_provider.h"

#include <oleauto.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

namespace snowdesktop
{
namespace
{
using AccessibilityNode =
    snowdesktop::widget_runtime::ViewAccessibilityNode;
using AccessibilityPattern =
    snowdesktop::widget_runtime::ViewAccessibilityPattern;

enum class ElementKind
{
    Widget,
    Node,
};

struct ElementReference
{
    ElementKind kind = ElementKind::Widget;
    std::wstring widgetId;
    std::string semanticId;
};

struct ProviderState
{
    HWND window = nullptr;
    WidgetAccessibilityProviderHost::SnapshotProvider snapshotProvider;
    WidgetAccessibilityProviderHost::FocusProvider focusProvider;
    WidgetAccessibilityProviderHost::ActionProvider actionProvider;
    bool connected = true;

    bool Capture(
        std::vector<LuaWidgetAccessibilitySnapshot>& snapshots) const noexcept
    {
        snapshots.clear();
        if (!connected || !window || !IsWindow(window) || !snapshotProvider)
            return false;
        try
        {
            snapshots = snapshotProvider();
            return true;
        }
        catch (...)
        {
            snapshots.clear();
            return false;
        }
    }
};

bool SupportsPattern(
    const AccessibilityNode& node, AccessibilityPattern pattern) noexcept
{
    return snowdesktop::widget_runtime::HasViewAccessibilityPattern(
        node.patterns, pattern);
}

std::optional<AccessibilityPattern> PatternForId(
    PATTERNID patternId) noexcept
{
    if (patternId == UIA_InvokePatternId)
        return AccessibilityPattern::Invoke;
    if (patternId == UIA_TogglePatternId)
        return AccessibilityPattern::Toggle;
    if (patternId == UIA_RangeValuePatternId)
        return AccessibilityPattern::RangeValue;
    if (patternId == UIA_ValuePatternId)
        return AccessibilityPattern::Value;
    if (patternId == UIA_ExpandCollapsePatternId)
        return AccessibilityPattern::ExpandCollapse;
    if (patternId == UIA_SelectionItemPatternId)
        return AccessibilityPattern::SelectionItem;
    return std::nullopt;
}

std::optional<AccessibilityPattern> AvailabilityPropertyPattern(
    PROPERTYID propertyId) noexcept
{
    if (propertyId == UIA_IsInvokePatternAvailablePropertyId)
        return AccessibilityPattern::Invoke;
    if (propertyId == UIA_IsTogglePatternAvailablePropertyId)
        return AccessibilityPattern::Toggle;
    if (propertyId == UIA_IsRangeValuePatternAvailablePropertyId)
        return AccessibilityPattern::RangeValue;
    if (propertyId == UIA_IsValuePatternAvailablePropertyId)
        return AccessibilityPattern::Value;
    if (propertyId == UIA_IsExpandCollapsePatternAvailablePropertyId)
        return AccessibilityPattern::ExpandCollapse;
    if (propertyId == UIA_IsSelectionItemPatternAvailablePropertyId)
        return AccessibilityPattern::SelectionItem;
    return std::nullopt;
}

struct ResolvedElement
{
    std::size_t widgetIndex = 0;
    std::optional<std::size_t> nodeIndex;
};

std::optional<ResolvedElement> ResolveElement(
    const std::vector<LuaWidgetAccessibilitySnapshot>& snapshots,
    const ElementReference& reference) noexcept
{
    for (std::size_t widgetIndex = 0;
         widgetIndex < snapshots.size(); ++widgetIndex)
    {
        const auto& widget = snapshots[widgetIndex];
        if (widget.widgetId != reference.widgetId) continue;
        if (reference.kind == ElementKind::Widget)
            return ResolvedElement{ widgetIndex, std::nullopt };
        for (std::size_t nodeIndex = 0;
             nodeIndex < widget.nodes.size(); ++nodeIndex)
        {
            if (widget.nodes[nodeIndex].semanticId == reference.semanticId)
                return ResolvedElement{ widgetIndex, nodeIndex };
        }
        return std::nullopt;
    }
    return std::nullopt;
}

ElementReference WidgetReference(
    const LuaWidgetAccessibilitySnapshot& widget)
{
    return { ElementKind::Widget, widget.widgetId, {} };
}

ElementReference NodeReference(
    const LuaWidgetAccessibilitySnapshot& widget,
    const AccessibilityNode& node)
{
    return { ElementKind::Node, widget.widgetId, node.semanticId };
}

std::vector<std::size_t> TopLevelNodes(
    const LuaWidgetAccessibilitySnapshot& widget)
{
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < widget.nodes.size(); ++index)
        if (widget.nodes[index].parentIndex ==
            AccessibilityNode::NoParent)
            indices.push_back(index);
    return indices;
}

std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max()))
        return {};
    const int length = static_cast<int>(text.size());
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), length, nullptr, 0);
    if (required <= 0)
        required = MultiByteToWideChar(CP_UTF8, 0,
            text.data(), length, nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text.data(), length,
            result.data(), required) != required)
        return {};
    return result;
}

HRESULT SetStringVariant(VARIANT* value, std::wstring_view text)
{
    value->vt = VT_BSTR;
    value->bstrVal = SysAllocStringLen(text.data(),
        static_cast<UINT>(text.size()));
    return value->bstrVal || text.empty() ? S_OK : E_OUTOFMEMORY;
}

void SetBoolVariant(VARIANT* value, bool enabled) noexcept
{
    value->vt = VT_BOOL;
    value->boolVal = enabled ? VARIANT_TRUE : VARIANT_FALSE;
}

void SetIntVariant(VARIANT* value, LONG number) noexcept
{
    value->vt = VT_I4;
    value->lVal = number;
}

LONG ControlTypeId(std::string_view type) noexcept
{
    if (type == "Button") return UIA_ButtonControlTypeId;
    if (type == "Calendar") return UIA_CalendarControlTypeId;
    if (type == "CheckBox") return UIA_CheckBoxControlTypeId;
    if (type == "ComboBox") return UIA_ComboBoxControlTypeId;
    if (type == "Edit") return UIA_EditControlTypeId;
    if (type == "Hyperlink") return UIA_HyperlinkControlTypeId;
    if (type == "Image") return UIA_ImageControlTypeId;
    if (type == "ListItem") return UIA_ListItemControlTypeId;
    if (type == "List") return UIA_ListControlTypeId;
    if (type == "ProgressBar") return UIA_ProgressBarControlTypeId;
    if (type == "RadioButton") return UIA_RadioButtonControlTypeId;
    if (type == "Slider") return UIA_SliderControlTypeId;
    if (type == "Spinner") return UIA_SpinnerControlTypeId;
    if (type == "Text") return UIA_TextControlTypeId;
    if (type == "Group") return UIA_GroupControlTypeId;
    if (type == "DataGrid") return UIA_DataGridControlTypeId;
    if (type == "Pane") return UIA_PaneControlTypeId;
    if (type == "Separator") return UIA_SeparatorControlTypeId;
    return UIA_CustomControlTypeId;
}

std::uint64_t HashBytes(
    std::uint64_t hash, const void* data, std::size_t size) noexcept
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= prime;
    }
    return hash;
}

std::uint64_t RuntimeHash(const ElementReference& reference) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    hash = HashBytes(hash, reference.widgetId.data(),
        reference.widgetId.size() * sizeof(wchar_t));
    const unsigned char kind = static_cast<unsigned char>(reference.kind);
    hash = HashBytes(hash, &kind, sizeof(kind));
    return HashBytes(hash, reference.semanticId.data(),
        reference.semanticId.size());
}

HRESULT CreateRuntimeId(
    const ElementReference& reference, SAFEARRAY** result)
{
    if (!result) return E_POINTER;
    *result = SafeArrayCreateVector(VT_I4, 0, 4);
    if (!*result) return E_OUTOFMEMORY;
    LONG* values = nullptr;
    HRESULT hr = SafeArrayAccessData(
        *result, reinterpret_cast<void**>(&values));
    if (FAILED(hr))
    {
        SafeArrayDestroy(*result);
        *result = nullptr;
        return hr;
    }
    const std::uint64_t hash = RuntimeHash(reference);
    values[0] = UiaAppendRuntimeId;
    values[1] = reference.kind == ElementKind::Widget ? 1 : 2;
    values[2] = static_cast<LONG>(hash & 0x7fffffffULL);
    values[3] = static_cast<LONG>((hash >> 32) & 0x7fffffffULL);
    hr = SafeArrayUnaccessData(*result);
    if (FAILED(hr))
    {
        SafeArrayDestroy(*result);
        *result = nullptr;
    }
    return hr;
}

bool WindowClientOrigin(HWND window, POINT& origin) noexcept
{
    origin = {};
    return window && IsWindow(window) && ClientToScreen(window, &origin);
}

UiaRect RootBounds(HWND window) noexcept
{
    RECT bounds{};
    POINT origin{};
    if (!window || !IsWindow(window) || !GetClientRect(window, &bounds) ||
        !WindowClientOrigin(window, origin))
        return {};
    return { static_cast<double>(origin.x), static_cast<double>(origin.y),
        static_cast<double>(std::max<LONG>(0, bounds.right - bounds.left)),
        static_cast<double>(std::max<LONG>(0, bounds.bottom - bounds.top)) };
}

UiaRect WidgetBounds(HWND window,
    const LuaWidgetAccessibilitySnapshot& widget) noexcept
{
    POINT origin{};
    if (!WindowClientOrigin(window, origin)) return {};
    return {
        static_cast<double>(origin.x + widget.bounds.left),
        static_cast<double>(origin.y + widget.bounds.top),
        static_cast<double>(std::max<LONG>(
            0, widget.bounds.right - widget.bounds.left)),
        static_cast<double>(std::max<LONG>(
            0, widget.bounds.bottom - widget.bounds.top)) };
}

UiaRect NodeBounds(HWND window,
    const LuaWidgetAccessibilitySnapshot& widget,
    const AccessibilityNode& node) noexcept
{
    POINT origin{};
    if (!WindowClientOrigin(window, origin)) return {};
    return {
        static_cast<double>(origin.x + widget.bounds.left) + node.bounds.x,
        static_cast<double>(origin.y + widget.bounds.top) + node.bounds.y,
        static_cast<double>(std::max(0.0f, node.bounds.width)),
        static_cast<double>(std::max(0.0f, node.bounds.height)) };
}

bool Contains(const UiaRect& bounds, double x, double y) noexcept
{
    return bounds.width > 0.0 && bounds.height > 0.0 &&
        x >= bounds.left && y >= bounds.top &&
        x < bounds.left + bounds.width && y < bounds.top + bounds.height;
}

bool ContainsNodePoint(HWND window,
    const LuaWidgetAccessibilitySnapshot& widget,
    const AccessibilityNode& node, double x, double y) noexcept
{
    if (node.offscreen || !Contains(NodeBounds(window, widget, node), x, y))
        return false;
    if (!node.clip) return true;
    POINT origin{};
    if (!WindowClientOrigin(window, origin)) return false;
    const UiaRect clip{
        static_cast<double>(origin.x + widget.bounds.left) + node.clip->x,
        static_cast<double>(origin.y + widget.bounds.top) + node.clip->y,
        static_cast<double>(std::max(0.0f, node.clip->width)),
        static_cast<double>(std::max(0.0f, node.clip->height)) };
    return Contains(clip, x, y);
}

bool WidgetOffscreen(HWND window,
    const LuaWidgetAccessibilitySnapshot& widget) noexcept
{
    const UiaRect root = RootBounds(window);
    const UiaRect bounds = WidgetBounds(window, widget);
    return bounds.width <= 0.0 || bounds.height <= 0.0 ||
        bounds.left >= root.left + root.width ||
        bounds.left + bounds.width <= root.left ||
        bounds.top >= root.top + root.height ||
        bounds.top + bounds.height <= root.top;
}

class FragmentProvider;

HRESULT CreateFragmentProvider(
    const std::shared_ptr<ProviderState>& state,
    IRawElementProviderFragmentRoot* root,
    ElementReference reference,
    IRawElementProviderFragment** result);

class RootAutomationProvider final :
    public IRawElementProviderSimple,
    public IRawElementProviderFragment,
    public IRawElementProviderFragmentRoot
{
public:
    explicit RootAutomationProvider(std::shared_ptr<ProviderState> state)
        : state_(std::move(state))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IRawElementProviderSimple)
            *object = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == IID_IRawElementProviderFragment)
            *object = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == IID_IRawElementProviderFragmentRoot)
            *object = static_cast<IRawElementProviderFragmentRoot*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(
        ProviderOptions* result) override
    {
        if (!result) return E_POINTER;
        *result = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPatternProvider(
        PATTERNID, IUnknown** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyValue(
        PROPERTYID propertyId, VARIANT* result) override
    {
        if (!result) return E_POINTER;
        VariantInit(result);
        if (!state_->connected || !state_->window ||
            !IsWindow(state_->window))
            return UIA_E_ELEMENTNOTAVAILABLE;
        if (propertyId == UIA_ControlTypePropertyId)
            SetIntVariant(result, UIA_PaneControlTypeId);
        else if (propertyId == UIA_NamePropertyId)
            return SetStringVariant(result, L"SnowDesktop");
        else if (propertyId == UIA_AutomationIdPropertyId)
            return SetStringVariant(result, L"SnowDesktop.Desktop");
        else if (propertyId == UIA_ClassNamePropertyId)
            return SetStringVariant(result, L"SnowDesktopWindow");
        else if (propertyId == UIA_FrameworkIdPropertyId)
            return SetStringVariant(result, L"SnowDesktop");
        else if (propertyId == UIA_NativeWindowHandlePropertyId)
            SetIntVariant(result, static_cast<LONG>(
                reinterpret_cast<LONG_PTR>(state_->window)));
        else if (propertyId == UIA_IsControlElementPropertyId ||
                 propertyId == UIA_IsContentElementPropertyId ||
                 propertyId == UIA_IsEnabledPropertyId)
            SetBoolVariant(result, true);
        else if (propertyId == UIA_IsKeyboardFocusablePropertyId ||
                 propertyId == UIA_HasKeyboardFocusPropertyId ||
                 propertyId == UIA_IsOffscreenPropertyId)
            SetBoolVariant(result, false);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
        IRawElementProviderSimple** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!state_->connected || !state_->window ||
            !IsWindow(state_->window))
            return UIA_E_ELEMENTNOTAVAILABLE;
        return UiaHostProviderFromHwnd(state_->window, result);
    }

    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
        IRawElementProviderFragment** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (direction != NavigateDirection_FirstChild &&
            direction != NavigateDirection_LastChild)
            return S_OK;
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        if (snapshots.empty()) return S_OK;
        const auto& widget = direction == NavigateDirection_FirstChild
            ? snapshots.front() : snapshots.back();
        return CreateFragmentProvider(state_, this,
            WidgetReference(widget), result);
    }

    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        return state_->connected ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }

    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(
        UiaRect* result) override
    {
        if (!result) return E_POINTER;
        if (!state_->connected || !state_->window ||
            !IsWindow(state_->window))
            return UIA_E_ELEMENTNOTAVAILABLE;
        *result = RootBounds(state_->window);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(
        SAFEARRAY** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        return state_->connected ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }

    HRESULT STDMETHODCALLTYPE SetFocus() override
    {
        if (!state_->connected || !state_->window ||
            !IsWindow(state_->window))
            return UIA_E_ELEMENTNOTAVAILABLE;
        ::SetFocus(state_->window);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_FragmentRoot(
        IRawElementProviderFragmentRoot** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!state_->connected) return UIA_E_ELEMENTNOTAVAILABLE;
        *result = static_cast<IRawElementProviderFragmentRoot*>(this);
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(
        double x, double y,
        IRawElementProviderFragment** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        for (auto widgetIt = snapshots.rbegin();
             widgetIt != snapshots.rend(); ++widgetIt)
        {
            if (!Contains(WidgetBounds(state_->window, *widgetIt), x, y))
                continue;
            for (auto nodeIt = widgetIt->nodes.rbegin();
                 nodeIt != widgetIt->nodes.rend(); ++nodeIt)
            {
                if (ContainsNodePoint(state_->window,
                        *widgetIt, *nodeIt, x, y))
                    return CreateFragmentProvider(state_, this,
                        NodeReference(*widgetIt, *nodeIt), result);
            }
            return CreateFragmentProvider(state_, this,
                WidgetReference(*widgetIt), result);
        }
        return QueryInterface(IID_IRawElementProviderFragment,
            reinterpret_cast<void**>(result));
    }

    HRESULT STDMETHODCALLTYPE GetFocus(
        IRawElementProviderFragment** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        for (const auto& widget : snapshots)
            for (const auto& node : widget.nodes)
                if (node.focused)
                    return CreateFragmentProvider(state_, this,
                        NodeReference(widget, node), result);
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{ 1 };
    std::shared_ptr<ProviderState> state_;
};

class FragmentProvider final :
    public IRawElementProviderSimple,
    public IRawElementProviderFragment,
    public IInvokeProvider,
    public IToggleProvider,
    public IRangeValueProvider,
    public IValueProvider,
    public IExpandCollapseProvider,
    public ISelectionItemProvider
{
public:
    FragmentProvider(std::shared_ptr<ProviderState> state,
        IRawElementProviderFragmentRoot* root,
        ElementReference reference)
        : state_(std::move(state)), root_(root),
          reference_(std::move(reference))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IRawElementProviderSimple)
            *object = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == IID_IRawElementProviderFragment)
            *object = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == IID_IInvokeProvider)
            *object = static_cast<IInvokeProvider*>(this);
        else if (iid == IID_IToggleProvider)
            *object = static_cast<IToggleProvider*>(this);
        else if (iid == IID_IRangeValueProvider)
            *object = static_cast<IRangeValueProvider*>(this);
        else if (iid == IID_IValueProvider)
            *object = static_cast<IValueProvider*>(this);
        else if (iid == IID_IExpandCollapseProvider)
            *object = static_cast<IExpandCollapseProvider*>(this);
        else if (iid == IID_ISelectionItemProvider)
            *object = static_cast<ISelectionItemProvider*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(
        ProviderOptions* result) override
    {
        if (!result) return E_POINTER;
        *result = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPatternProvider(
        PATTERNID patternId, IUnknown** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto resolved = ResolveElement(snapshots, reference_);
        if (!resolved) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!resolved->nodeIndex) return S_OK;
        const auto pattern = PatternForId(patternId);
        const auto& node = snapshots[resolved->widgetIndex]
            .nodes[*resolved->nodeIndex];
        if (!pattern || !SupportsPattern(node, *pattern)) return S_OK;
        if (patternId == UIA_InvokePatternId)
            return QueryInterface(IID_IInvokeProvider,
                reinterpret_cast<void**>(result));
        if (patternId == UIA_TogglePatternId)
            return QueryInterface(IID_IToggleProvider,
                reinterpret_cast<void**>(result));
        if (patternId == UIA_RangeValuePatternId)
            return QueryInterface(IID_IRangeValueProvider,
                reinterpret_cast<void**>(result));
        if (patternId == UIA_ValuePatternId)
            return QueryInterface(IID_IValueProvider,
                reinterpret_cast<void**>(result));
        if (patternId == UIA_ExpandCollapsePatternId)
            return QueryInterface(IID_IExpandCollapseProvider,
                reinterpret_cast<void**>(result));
        if (patternId == UIA_SelectionItemPatternId)
            return QueryInterface(IID_ISelectionItemProvider,
                reinterpret_cast<void**>(result));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyValue(
        PROPERTYID propertyId, VARIANT* result) override
    {
        if (!result) return E_POINTER;
        VariantInit(result);
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto resolved = ResolveElement(snapshots, reference_);
        if (!resolved) return UIA_E_ELEMENTNOTAVAILABLE;
        const auto& widget = snapshots[resolved->widgetIndex];
        const AccessibilityNode* node = resolved->nodeIndex
            ? &widget.nodes[*resolved->nodeIndex] : nullptr;

        if (node)
        {
            const auto availability =
                AvailabilityPropertyPattern(propertyId);
            if (availability)
            {
                SetBoolVariant(result,
                    SupportsPattern(*node, *availability));
                return S_OK;
            }
        }

        if (propertyId == UIA_ControlTypePropertyId)
            SetIntVariant(result, node
                ? ControlTypeId(node->controlType)
                : UIA_GroupControlTypeId);
        else if (propertyId == UIA_NamePropertyId)
            return SetStringVariant(result,
                Utf8ToWide(node ? node->name : widget.name));
        else if (propertyId == UIA_AutomationIdPropertyId)
        {
            if (!node)
                return SetStringVariant(result, widget.widgetId);
            return SetStringVariant(result,
                Utf8ToWide(node->key.empty()
                    ? node->semanticId : node->key));
        }
        else if (propertyId == UIA_ClassNamePropertyId)
            return SetStringVariant(result, node
                ? L"SnowDesktopLuaElement" : L"SnowDesktopLuaWidget");
        else if (propertyId == UIA_FrameworkIdPropertyId)
            return SetStringVariant(result, L"SnowDesktop");
        else if (propertyId == UIA_IsControlElementPropertyId ||
                 propertyId == UIA_IsContentElementPropertyId)
            SetBoolVariant(result, true);
        else if (propertyId == UIA_IsEnabledPropertyId)
            SetBoolVariant(result, node ? node->enabled : true);
        else if (propertyId == UIA_IsKeyboardFocusablePropertyId)
            SetBoolVariant(result, node ? node->focusable : true);
        else if (propertyId == UIA_HasKeyboardFocusPropertyId)
            SetBoolVariant(result, node ? node->focused : false);
        else if (propertyId == UIA_IsOffscreenPropertyId)
            SetBoolVariant(result, node
                ? node->offscreen || WidgetOffscreen(state_->window, widget)
                : WidgetOffscreen(state_->window, widget));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
        IRawElementProviderSimple** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        return state_->connected ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }

    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
        IRawElementProviderFragment** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto resolved = ResolveElement(snapshots, reference_);
        if (!resolved) return UIA_E_ELEMENTNOTAVAILABLE;
        const auto& widget = snapshots[resolved->widgetIndex];

        if (reference_.kind == ElementKind::Widget)
        {
            if (direction == NavigateDirection_Parent)
                return root_->QueryInterface(
                    IID_IRawElementProviderFragment,
                    reinterpret_cast<void**>(result));
            if (direction == NavigateDirection_PreviousSibling ||
                direction == NavigateDirection_NextSibling)
            {
                const bool previous =
                    direction == NavigateDirection_PreviousSibling;
                const std::size_t index = resolved->widgetIndex;
                if ((previous && index == 0) ||
                    (!previous && index + 1 >= snapshots.size()))
                    return S_OK;
                const auto& sibling = snapshots[
                    previous ? index - 1 : index + 1];
                return CreateFragmentProvider(state_, root_.Get(),
                    WidgetReference(sibling), result);
            }
            if (direction == NavigateDirection_FirstChild ||
                direction == NavigateDirection_LastChild)
            {
                const auto topLevel = TopLevelNodes(widget);
                if (topLevel.empty()) return S_OK;
                const std::size_t index =
                    direction == NavigateDirection_FirstChild
                    ? topLevel.front() : topLevel.back();
                return CreateFragmentProvider(state_, root_.Get(),
                    NodeReference(widget, widget.nodes[index]), result);
            }
            return S_OK;
        }

        const std::size_t nodeIndex = *resolved->nodeIndex;
        const auto& node = widget.nodes[nodeIndex];
        if (direction == NavigateDirection_Parent)
        {
            if (node.parentIndex == AccessibilityNode::NoParent)
                return CreateFragmentProvider(state_, root_.Get(),
                    WidgetReference(widget), result);
            if (node.parentIndex >= widget.nodes.size())
                return UIA_E_ELEMENTNOTAVAILABLE;
            return CreateFragmentProvider(state_, root_.Get(),
                NodeReference(widget, widget.nodes[node.parentIndex]), result);
        }
        if (direction == NavigateDirection_FirstChild ||
            direction == NavigateDirection_LastChild)
        {
            if (node.children.empty()) return S_OK;
            const std::size_t childIndex =
                direction == NavigateDirection_FirstChild
                ? node.children.front() : node.children.back();
            if (childIndex >= widget.nodes.size())
                return UIA_E_ELEMENTNOTAVAILABLE;
            return CreateFragmentProvider(state_, root_.Get(),
                NodeReference(widget, widget.nodes[childIndex]), result);
        }
        if (direction == NavigateDirection_PreviousSibling ||
            direction == NavigateDirection_NextSibling)
        {
            const std::vector<std::size_t> siblings =
                node.parentIndex == AccessibilityNode::NoParent
                ? TopLevelNodes(widget)
                : (node.parentIndex < widget.nodes.size()
                    ? widget.nodes[node.parentIndex].children
                    : std::vector<std::size_t>{});
            const auto found = std::find(
                siblings.begin(), siblings.end(), nodeIndex);
            if (found == siblings.end())
                return UIA_E_ELEMENTNOTAVAILABLE;
            if (direction == NavigateDirection_PreviousSibling)
            {
                if (found == siblings.begin()) return S_OK;
                const std::size_t siblingIndex = *(found - 1);
                if (siblingIndex >= widget.nodes.size())
                    return UIA_E_ELEMENTNOTAVAILABLE;
                return CreateFragmentProvider(state_, root_.Get(),
                    NodeReference(widget, widget.nodes[siblingIndex]), result);
            }
            const auto next = found + 1;
            if (next == siblings.end()) return S_OK;
            if (*next >= widget.nodes.size())
                return UIA_E_ELEMENTNOTAVAILABLE;
            return CreateFragmentProvider(state_, root_.Get(),
                NodeReference(widget, widget.nodes[*next]), result);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override
    {
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots) ||
            !ResolveElement(snapshots, reference_))
        {
            if (result) *result = nullptr;
            return result ? UIA_E_ELEMENTNOTAVAILABLE : E_POINTER;
        }
        return CreateRuntimeId(reference_, result);
    }

    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(
        UiaRect* result) override
    {
        if (!result) return E_POINTER;
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto resolved = ResolveElement(snapshots, reference_);
        if (!resolved) return UIA_E_ELEMENTNOTAVAILABLE;
        const auto& widget = snapshots[resolved->widgetIndex];
        *result = resolved->nodeIndex
            ? NodeBounds(state_->window, widget,
                widget.nodes[*resolved->nodeIndex])
            : WidgetBounds(state_->window, widget);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(
        SAFEARRAY** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        return state_->connected ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }

    HRESULT STDMETHODCALLTYPE SetFocus() override
    {
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto resolved = ResolveElement(snapshots, reference_);
        if (!resolved) return UIA_E_ELEMENTNOTAVAILABLE;
        const auto& widget = snapshots[resolved->widgetIndex];
        std::string key;
        if (resolved->nodeIndex)
        {
            const auto& node = widget.nodes[*resolved->nodeIndex];
            if (!node.enabled || !node.focusable || node.key.empty())
                return UIA_E_NOTSUPPORTED;
            key = node.key;
        }
        if (!state_->focusProvider)
            return UIA_E_NOTSUPPORTED;
        try
        {
            return state_->focusProvider(widget.widgetId, key)
                ? S_OK : UIA_E_NOTSUPPORTED;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE get_FragmentRoot(
        IRawElementProviderFragmentRoot** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!state_->connected) return UIA_E_ELEMENTNOTAVAILABLE;
        return root_.CopyTo(result);
    }

    HRESULT STDMETHODCALLTYPE Invoke() override
    {
        return PerformAction(
            AccessibilityPattern::Invoke,
            LuaWidgetAccessibilityActionKind::Invoke);
    }

    HRESULT STDMETHODCALLTYPE Toggle() override
    {
        return PerformAction(
            AccessibilityPattern::Toggle,
            LuaWidgetAccessibilityActionKind::Toggle);
    }

    HRESULT STDMETHODCALLTYPE get_ToggleState(
        ToggleState* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Toggle, node);
        if (FAILED(hr)) return hr;
        *result = !node.checked
            ? ToggleState_Indeterminate
            : *node.checked ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetValue(double value) override
    {
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::RangeValue, node);
        if (FAILED(hr)) return hr;
        if (node.rangeValueReadOnly) return UIA_E_NOTSUPPORTED;
        if (!std::isfinite(value) || !node.minimum || !node.maximum ||
            value < *node.minimum || value > *node.maximum)
            return E_INVALIDARG;
        return PerformAction(
            AccessibilityPattern::RangeValue,
            LuaWidgetAccessibilityActionKind::SetRangeValue,
            value);
    }

    HRESULT STDMETHODCALLTYPE get_Value(double* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::RangeValue, node);
        if (FAILED(hr)) return hr;
        if (!node.value) return UIA_E_NOTSUPPORTED;
        *result = *node.value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* result) override
    {
        if (!result) return E_POINTER;
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto resolved = ResolveElement(snapshots, reference_);
        if (!resolved || !resolved->nodeIndex)
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto& node = snapshots[resolved->widgetIndex]
            .nodes[*resolved->nodeIndex];
        *result = node.valueReadOnly && node.rangeValueReadOnly;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_Maximum(double* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::RangeValue, node);
        if (FAILED(hr)) return hr;
        if (!node.maximum) return UIA_E_NOTSUPPORTED;
        *result = *node.maximum;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_Minimum(double* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::RangeValue, node);
        if (FAILED(hr)) return hr;
        if (!node.minimum) return UIA_E_NOTSUPPORTED;
        *result = *node.minimum;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_LargeChange(double* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::RangeValue, node);
        if (FAILED(hr)) return hr;
        if (!node.minimum || !node.maximum)
            return UIA_E_NOTSUPPORTED;
        const double span = std::max(
            0.0, static_cast<double>(*node.maximum - *node.minimum));
        const double step = node.step
            ? std::max(0.0, static_cast<double>(*node.step)) : 0.0;
        *result = std::max(step, span / 10.0);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_SmallChange(double* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::RangeValue, node);
        if (FAILED(hr)) return hr;
        *result = node.step
            ? std::max(0.0, static_cast<double>(*node.step)) : 0.0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override
    {
        if (!value) return E_INVALIDARG;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Value, node);
        if (FAILED(hr)) return hr;
        if (node.valueReadOnly) return UIA_E_NOTSUPPORTED;
        return PerformAction(
            AccessibilityPattern::Value,
            LuaWidgetAccessibilityActionKind::SetValue,
            0.0, value);
    }

    HRESULT STDMETHODCALLTYPE get_Value(BSTR* result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Value, node);
        if (FAILED(hr)) return hr;
        const std::wstring value = Utf8ToWide(node.valueText);
        *result = SysAllocStringLen(value.data(),
            static_cast<UINT>(value.size()));
        return *result || value.empty() ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE Expand() override
    {
        return PerformAction(
            AccessibilityPattern::ExpandCollapse,
            LuaWidgetAccessibilityActionKind::Expand);
    }

    HRESULT STDMETHODCALLTYPE Collapse() override
    {
        return PerformAction(
            AccessibilityPattern::ExpandCollapse,
            LuaWidgetAccessibilityActionKind::Collapse);
    }

    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(
        ExpandCollapseState* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::ExpandCollapse, node);
        if (FAILED(hr)) return hr;
        *result = !node.expanded
            ? ExpandCollapseState_LeafNode
            : *node.expanded
                ? ExpandCollapseState_Expanded
                : ExpandCollapseState_Collapsed;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Select() override
    {
        return PerformAction(
            AccessibilityPattern::SelectionItem,
            LuaWidgetAccessibilityActionKind::Select);
    }

    HRESULT STDMETHODCALLTYPE AddToSelection() override
    {
        return Select();
    }

    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override
    {
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::SelectionItem, node);
        return FAILED(hr) ? hr : UIA_E_INVALIDOPERATION;
    }

    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::SelectionItem, node);
        if (FAILED(hr)) return hr;
        *result = node.checked.value_or(false) ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_SelectionContainer(
        IRawElementProviderSimple** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::SelectionItem, node);
        if (FAILED(hr)) return hr;
        Microsoft::WRL::ComPtr<IRawElementProviderFragment> parent;
        const HRESULT navigation = Navigate(
            NavigateDirection_Parent, &parent);
        if (FAILED(navigation) || !parent) return navigation;
        return parent->QueryInterface(IID_PPV_ARGS(result));
    }

private:
    HRESULT ResolvePatternNode(AccessibilityPattern pattern,
        AccessibilityNode& node) const
    {
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto resolved = ResolveElement(snapshots, reference_);
        if (!resolved || !resolved->nodeIndex)
            return UIA_E_ELEMENTNOTAVAILABLE;
        node = snapshots[resolved->widgetIndex]
            .nodes[*resolved->nodeIndex];
        if (!SupportsPattern(node, pattern))
            return UIA_E_NOTSUPPORTED;
        return S_OK;
    }

    HRESULT PerformAction(AccessibilityPattern pattern,
        LuaWidgetAccessibilityActionKind kind,
        double numericValue = 0.0,
        std::wstring textValue = {}) const
    {
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto resolved = ResolveElement(snapshots, reference_);
        if (!resolved || !resolved->nodeIndex)
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto& widget = snapshots[resolved->widgetIndex];
        const auto& node = widget.nodes[*resolved->nodeIndex];
        if (!SupportsPattern(node, pattern))
            return UIA_E_NOTSUPPORTED;
        if (!node.enabled) return UIA_E_ELEMENTNOTENABLED;
        if (node.key.empty() || !state_->actionProvider)
            return UIA_E_NOTSUPPORTED;

        LuaWidgetAccessibilityActionRequest request;
        request.kind = kind;
        request.widgetId = widget.widgetId;
        request.nodeKey = node.key;
        request.numericValue = numericValue;
        request.textValue = std::move(textValue);
        try
        {
            return state_->actionProvider(request)
                ? S_OK : UIA_E_NOTSUPPORTED;
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    std::atomic<ULONG> references_{ 1 };
    std::shared_ptr<ProviderState> state_;
    Microsoft::WRL::ComPtr<IRawElementProviderFragmentRoot> root_;
    ElementReference reference_;
};

HRESULT CreateFragmentProvider(
    const std::shared_ptr<ProviderState>& state,
    IRawElementProviderFragmentRoot* root,
    ElementReference reference,
    IRawElementProviderFragment** result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    if (!state || !state->connected || !root)
        return UIA_E_ELEMENTNOTAVAILABLE;
    auto* provider = new (std::nothrow) FragmentProvider(
        state, root, std::move(reference));
    if (!provider) return E_OUTOFMEMORY;
    *result = static_cast<IRawElementProviderFragment*>(provider);
    return S_OK;
}
}

struct WidgetAccessibilityProviderHost::Impl
{
    SnapshotProvider snapshotProvider;
    FocusProvider focusProvider;
    ActionProvider actionProvider;
    std::shared_ptr<ProviderState> state;
    Microsoft::WRL::ComPtr<IRawElementProviderSimple> root;
};

WidgetAccessibilityProviderHost::WidgetAccessibilityProviderHost(
    SnapshotProvider snapshotProvider,
    FocusProvider focusProvider,
    ActionProvider actionProvider)
    : impl_(std::make_unique<Impl>())
{
    impl_->snapshotProvider = std::move(snapshotProvider);
    impl_->focusProvider = std::move(focusProvider);
    impl_->actionProvider = std::move(actionProvider);
}

WidgetAccessibilityProviderHost::~WidgetAccessibilityProviderHost()
{
    if (impl_ && impl_->state)
        impl_->state->connected = false;
}

bool WidgetAccessibilityProviderHost::AttachWindow(HWND window)
{
    if (!impl_ || !window || !IsWindow(window)) return false;
    if (impl_->state && impl_->state->window == window && impl_->root)
        return true;
    if (impl_->state)
    {
        if (impl_->state->window)
            UiaReturnRawElementProvider(
                impl_->state->window, 0, 0, nullptr);
        impl_->state->connected = false;
        impl_->root.Reset();
        impl_->state.reset();
    }
    auto state = std::make_shared<ProviderState>();
    state->window = window;
    state->snapshotProvider = impl_->snapshotProvider;
    state->focusProvider = impl_->focusProvider;
    state->actionProvider = impl_->actionProvider;
    auto* root = new (std::nothrow) RootAutomationProvider(state);
    if (!root) return false;
    impl_->state = std::move(state);
    impl_->root.Attach(static_cast<IRawElementProviderSimple*>(root));
    return true;
}

void WidgetAccessibilityProviderHost::DetachWindow(HWND window) noexcept
{
    if (!impl_ || !impl_->state || impl_->state->window != window)
        return;
    if (window)
        UiaReturnRawElementProvider(window, 0, 0, nullptr);
    impl_->state->connected = false;
    impl_->root.Reset();
    impl_->state.reset();
}

bool WidgetAccessibilityProviderHost::TryHandleGetObject(
    HWND window, WPARAM wParam, LPARAM lParam, LRESULT& result) const noexcept
{
    if (!impl_ || !impl_->state || !impl_->root ||
        impl_->state->window != window ||
        static_cast<LONG>(lParam) != UiaRootObjectId)
        return false;
    result = UiaReturnRawElementProvider(
        window, wParam, lParam, impl_->root.Get());
    return true;
}

IRawElementProviderSimple*
WidgetAccessibilityProviderHost::RootProvider() const noexcept
{
    return impl_ ? impl_->root.Get() : nullptr;
}
}
