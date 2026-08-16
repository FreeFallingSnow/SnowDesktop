#include <ole2.h>
#include <UIAutomation.h>
#include <UIAutomationCoreApi.h>

#include "widget_accessibility_provider.h"
#include "widget_accessibility_events.h"

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

double ScrollMaximum(const AccessibilityNode& node) noexcept
{
    return std::max(0.0, static_cast<double>(
        node.scrollContentExtent - node.scrollViewportExtent));
}

bool Scrollable(const AccessibilityNode& node) noexcept
{
    return ScrollMaximum(node) > 0.0;
}

double ScrollPercent(const AccessibilityNode& node) noexcept
{
    const double maximum = ScrollMaximum(node);
    return maximum > 0.0
        ? std::clamp(static_cast<double>(node.scrollOffset) /
                maximum * 100.0, 0.0, 100.0)
        : UIA_ScrollPatternNoScroll;
}

double ScrollViewSize(const AccessibilityNode& node) noexcept
{
    return node.scrollContentExtent > 0.0f
        ? std::clamp(static_cast<double>(node.scrollViewportExtent) /
                static_cast<double>(node.scrollContentExtent) * 100.0,
            0.0, 100.0)
        : 100.0;
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
    if (patternId == UIA_SelectionPatternId)
        return AccessibilityPattern::Selection;
    if (patternId == UIA_ScrollPatternId)
        return AccessibilityPattern::Scroll;
    if (patternId == UIA_GridPatternId)
        return AccessibilityPattern::Grid;
    if (patternId == UIA_GridItemPatternId)
        return AccessibilityPattern::GridItem;
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
    if (propertyId == UIA_IsSelectionPatternAvailablePropertyId)
        return AccessibilityPattern::Selection;
    if (propertyId == UIA_IsScrollPatternAvailablePropertyId)
        return AccessibilityPattern::Scroll;
    if (propertyId == UIA_IsGridPatternAvailablePropertyId)
        return AccessibilityPattern::Grid;
    if (propertyId == UIA_IsGridItemPatternAvailablePropertyId)
        return AccessibilityPattern::GridItem;
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

void SetDoubleVariant(VARIANT* value, double number) noexcept
{
    value->vt = VT_R8;
    value->dblVal = number;
}

HRESULT SetRectVariant(VARIANT* value, const UiaRect& bounds)
{
    SAFEARRAY* array = SafeArrayCreateVector(VT_R8, 0, 4);
    if (!array) return E_OUTOFMEMORY;
    double* numbers = nullptr;
    HRESULT hr = SafeArrayAccessData(
        array, reinterpret_cast<void**>(&numbers));
    if (FAILED(hr))
    {
        SafeArrayDestroy(array);
        return hr;
    }
    numbers[0] = bounds.left;
    numbers[1] = bounds.top;
    numbers[2] = bounds.width;
    numbers[3] = bounds.height;
    hr = SafeArrayUnaccessData(array);
    if (FAILED(hr))
    {
        SafeArrayDestroy(array);
        return hr;
    }
    value->vt = VT_ARRAY | VT_R8;
    value->parray = array;
    return S_OK;
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
    if (type == "DataItem") return UIA_DataItemControlTypeId;
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

const LuaWidgetAccessibilitySnapshot* FindWidgetSnapshot(
    const std::vector<LuaWidgetAccessibilitySnapshot>& snapshots,
    std::wstring_view widgetId) noexcept
{
    const auto found = std::find_if(snapshots.begin(), snapshots.end(),
        [widgetId](const auto& widget) {
            return widget.widgetId == widgetId;
        });
    return found == snapshots.end() ? nullptr : &*found;
}

const AccessibilityNode* FindAccessibilityNode(
    const LuaWidgetAccessibilitySnapshot& widget,
    std::string_view semanticId) noexcept
{
    const auto found = std::find_if(widget.nodes.begin(),
        widget.nodes.end(), [semanticId](const auto& node) {
            return node.semanticId == semanticId;
        });
    return found == widget.nodes.end() ? nullptr : &*found;
}

ElementReference ChangeReference(
    const WidgetAccessibilityChange& change)
{
    return change.element == WidgetAccessibilityElementKind::Node
        ? ElementReference{ ElementKind::Node,
            change.widgetId, change.semanticId }
        : ElementReference{ ElementKind::Widget,
            change.widgetId, {} };
}

std::optional<PROPERTYID> ChangePropertyId(
    WidgetAccessibilityChangeKind kind) noexcept
{
    switch (kind)
    {
    case WidgetAccessibilityChangeKind::Name:
        return UIA_NamePropertyId;
    case WidgetAccessibilityChangeKind::AriaRole:
        return UIA_AriaRolePropertyId;
    case WidgetAccessibilityChangeKind::HelpText:
        return UIA_HelpTextPropertyId;
    case WidgetAccessibilityChangeKind::ItemStatus:
        return UIA_ItemStatusPropertyId;
    case WidgetAccessibilityChangeKind::HeadingLevel:
        return UIA_HeadingLevelPropertyId;
    case WidgetAccessibilityChangeKind::LiveSetting:
        return UIA_LiveSettingPropertyId;
    case WidgetAccessibilityChangeKind::PositionInSet:
        return UIA_PositionInSetPropertyId;
    case WidgetAccessibilityChangeKind::SizeOfSet:
        return UIA_SizeOfSetPropertyId;
    case WidgetAccessibilityChangeKind::Enabled:
        return UIA_IsEnabledPropertyId;
    case WidgetAccessibilityChangeKind::Required:
        return UIA_IsRequiredForFormPropertyId;
    case WidgetAccessibilityChangeKind::Offscreen:
        return UIA_IsOffscreenPropertyId;
    case WidgetAccessibilityChangeKind::Toggle:
        return UIA_ToggleToggleStatePropertyId;
    case WidgetAccessibilityChangeKind::SelectionItem:
        return UIA_SelectionItemIsSelectedPropertyId;
    case WidgetAccessibilityChangeKind::RangeValue:
        return UIA_RangeValueValuePropertyId;
    case WidgetAccessibilityChangeKind::Value:
        return UIA_ValueValuePropertyId;
    case WidgetAccessibilityChangeKind::ExpandCollapse:
        return UIA_ExpandCollapseExpandCollapseStatePropertyId;
    case WidgetAccessibilityChangeKind::Bounds:
        return UIA_BoundingRectanglePropertyId;
    case WidgetAccessibilityChangeKind::HorizontalScrollPercent:
        return UIA_ScrollHorizontalScrollPercentPropertyId;
    case WidgetAccessibilityChangeKind::HorizontalViewSize:
        return UIA_ScrollHorizontalViewSizePropertyId;
    case WidgetAccessibilityChangeKind::HorizontallyScrollable:
        return UIA_ScrollHorizontallyScrollablePropertyId;
    case WidgetAccessibilityChangeKind::VerticalScrollPercent:
        return UIA_ScrollVerticalScrollPercentPropertyId;
    case WidgetAccessibilityChangeKind::VerticalViewSize:
        return UIA_ScrollVerticalViewSizePropertyId;
    case WidgetAccessibilityChangeKind::VerticallyScrollable:
        return UIA_ScrollVerticallyScrollablePropertyId;
    default:
        return std::nullopt;
    }
}

HRESULT SetChangeVariant(VARIANT* result,
    WidgetAccessibilityChangeKind kind, HWND window,
    const std::vector<LuaWidgetAccessibilitySnapshot>& snapshots,
    const WidgetAccessibilityChange& change)
{
    if (!result) return E_POINTER;
    VariantInit(result);
    const auto* widget = FindWidgetSnapshot(
        snapshots, change.widgetId);
    if (!widget) return UIA_E_ELEMENTNOTAVAILABLE;
    const AccessibilityNode* node =
        change.element == WidgetAccessibilityElementKind::Node
            ? FindAccessibilityNode(*widget, change.semanticId)
            : nullptr;
    if (change.element == WidgetAccessibilityElementKind::Node && !node)
        return UIA_E_ELEMENTNOTAVAILABLE;

    switch (kind)
    {
    case WidgetAccessibilityChangeKind::Name:
        return SetStringVariant(result,
            Utf8ToWide(node ? node->name : widget->name));
    case WidgetAccessibilityChangeKind::AriaRole:
        return SetStringVariant(result, Utf8ToWide(node->role));
    case WidgetAccessibilityChangeKind::HelpText:
        return SetStringVariant(result, Utf8ToWide(node->helpText));
    case WidgetAccessibilityChangeKind::ItemStatus:
        return SetStringVariant(result, Utf8ToWide(node->valueText));
    case WidgetAccessibilityChangeKind::HeadingLevel:
        SetIntVariant(result, node->headingLevel == 0
            ? HeadingLevel_None
            : HeadingLevel_None + node->headingLevel);
        return S_OK;
    case WidgetAccessibilityChangeKind::LiveSetting:
        SetIntVariant(result, static_cast<LONG>(node->live));
        return S_OK;
    case WidgetAccessibilityChangeKind::PositionInSet:
        if (node->positionInSet)
            SetIntVariant(result, *node->positionInSet);
        return S_OK;
    case WidgetAccessibilityChangeKind::SizeOfSet:
        if (node->setSize) SetIntVariant(result, *node->setSize);
        return S_OK;
    case WidgetAccessibilityChangeKind::Enabled:
        SetBoolVariant(result, node ? node->enabled : true);
        return S_OK;
    case WidgetAccessibilityChangeKind::Required:
        SetBoolVariant(result, node && node->required);
        return S_OK;
    case WidgetAccessibilityChangeKind::Offscreen:
        SetBoolVariant(result, node
            ? node->offscreen || WidgetOffscreen(window, *widget)
            : WidgetOffscreen(window, *widget));
        return S_OK;
    case WidgetAccessibilityChangeKind::Toggle:
        SetIntVariant(result, !node->checked
            ? ToggleState_Indeterminate
            : *node->checked ? ToggleState_On : ToggleState_Off);
        return S_OK;
    case WidgetAccessibilityChangeKind::SelectionItem:
        SetBoolVariant(result, node->checked.value_or(false));
        return S_OK;
    case WidgetAccessibilityChangeKind::RangeValue:
        if (!node->value) return UIA_E_NOTSUPPORTED;
        SetDoubleVariant(result, *node->value);
        return S_OK;
    case WidgetAccessibilityChangeKind::Value:
        return SetStringVariant(result, Utf8ToWide(node->valueText));
    case WidgetAccessibilityChangeKind::ExpandCollapse:
        SetIntVariant(result, !node->expanded
            ? ExpandCollapseState_LeafNode
            : *node->expanded
                ? ExpandCollapseState_Expanded
                : ExpandCollapseState_Collapsed);
        return S_OK;
    case WidgetAccessibilityChangeKind::Bounds:
        return SetRectVariant(result, node
            ? NodeBounds(window, *widget, *node)
            : WidgetBounds(window, *widget));
    case WidgetAccessibilityChangeKind::HorizontalScrollPercent:
        SetDoubleVariant(result, node->scrollHorizontal
            ? ScrollPercent(*node) : UIA_ScrollPatternNoScroll);
        return S_OK;
    case WidgetAccessibilityChangeKind::HorizontalViewSize:
        SetDoubleVariant(result, node->scrollHorizontal
            ? ScrollViewSize(*node) : 100.0);
        return S_OK;
    case WidgetAccessibilityChangeKind::HorizontallyScrollable:
        SetBoolVariant(result,
            node->scrollHorizontal && Scrollable(*node));
        return S_OK;
    case WidgetAccessibilityChangeKind::VerticalScrollPercent:
        SetDoubleVariant(result, !node->scrollHorizontal
            ? ScrollPercent(*node) : UIA_ScrollPatternNoScroll);
        return S_OK;
    case WidgetAccessibilityChangeKind::VerticalViewSize:
        SetDoubleVariant(result, !node->scrollHorizontal
            ? ScrollViewSize(*node) : 100.0);
        return S_OK;
    case WidgetAccessibilityChangeKind::VerticallyScrollable:
        SetBoolVariant(result,
            !node->scrollHorizontal && Scrollable(*node));
        return S_OK;
    default:
        return UIA_E_NOTSUPPORTED;
    }
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
    public ISelectionItemProvider,
    public ISelectionProvider,
    public IScrollProvider,
    public IGridProvider,
    public IGridItemProvider
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
        else if (iid == IID_ISelectionProvider)
            *object = static_cast<ISelectionProvider*>(this);
        else if (iid == IID_IScrollProvider)
            *object = static_cast<IScrollProvider*>(this);
        else if (iid == IID_IGridProvider)
            *object = static_cast<IGridProvider*>(this);
        else if (iid == IID_IGridItemProvider)
            *object = static_cast<IGridItemProvider*>(this);
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
        if (patternId == UIA_SelectionPatternId)
            return QueryInterface(IID_ISelectionProvider,
                reinterpret_cast<void**>(result));
        if (patternId == UIA_ScrollPatternId)
            return QueryInterface(IID_IScrollProvider,
                reinterpret_cast<void**>(result));
        if (patternId == UIA_GridPatternId)
            return QueryInterface(IID_IGridProvider,
                reinterpret_cast<void**>(result));
        if (patternId == UIA_GridItemPatternId)
            return QueryInterface(IID_IGridItemProvider,
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
        const auto relatedProvider = [&](std::string_view semanticId,
            Microsoft::WRL::ComPtr<IRawElementProviderSimple>& provider) {
            provider.Reset();
            if (semanticId.empty()) return false;
            const auto* related = FindAccessibilityNode(
                widget, semanticId);
            if (!related) return false;
            Microsoft::WRL::ComPtr<IRawElementProviderFragment> fragment;
            if (FAILED(CreateFragmentProvider(state_, root_.Get(),
                    NodeReference(widget, *related), &fragment)) ||
                !fragment)
                return false;
            return SUCCEEDED(fragment.As(&provider)) && provider;
        };

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
        else if (propertyId == UIA_HelpTextPropertyId && node)
            return SetStringVariant(result, Utf8ToWide(node->helpText));
        else if (propertyId == UIA_ItemStatusPropertyId && node)
            return SetStringVariant(result, Utf8ToWide(node->valueText));
        else if (propertyId == UIA_AriaRolePropertyId && node)
            return SetStringVariant(result, Utf8ToWide(node->role));
        else if (propertyId == UIA_HeadingLevelPropertyId && node)
            SetIntVariant(result, node->headingLevel == 0
                ? HeadingLevel_None
                : HeadingLevel_None + node->headingLevel);
        else if (propertyId == UIA_LiveSettingPropertyId && node)
            SetIntVariant(result, static_cast<LONG>(node->live));
        else if (propertyId == UIA_PositionInSetPropertyId && node &&
                 node->positionInSet)
            SetIntVariant(result, *node->positionInSet);
        else if (propertyId == UIA_SizeOfSetPropertyId && node &&
                 node->setSize)
            SetIntVariant(result, *node->setSize);
        else if (propertyId == UIA_LabeledByPropertyId && node)
        {
            Microsoft::WRL::ComPtr<IRawElementProviderSimple> provider;
            if (!relatedProvider(node->labelledBySemanticId, provider))
                return S_OK;
            result->vt = VT_UNKNOWN;
            result->punkVal = provider.Detach();
            return S_OK;
        }
        else if (propertyId == UIA_DescribedByPropertyId && node)
        {
            Microsoft::WRL::ComPtr<IRawElementProviderSimple> provider;
            if (!relatedProvider(node->describedBySemanticId, provider))
                return S_OK;
            SAFEARRAYBOUND bounds{ 1, 0 };
            SAFEARRAY* values = SafeArrayCreate(VT_UNKNOWN, 1, &bounds);
            if (!values) return E_OUTOFMEMORY;
            LONG index = 0;
            const HRESULT inserted = SafeArrayPutElement(
                values, &index, provider.Get());
            if (FAILED(inserted))
            {
                SafeArrayDestroy(values);
                return inserted;
            }
            result->vt = VT_ARRAY | VT_UNKNOWN;
            result->parray = values;
            return S_OK;
        }
        else if (propertyId == UIA_AccessKeyPropertyId && node)
            return SetStringVariant(result, Utf8ToWide(node->accessKey));
        else if (propertyId == UIA_AcceleratorKeyPropertyId && node)
            return SetStringVariant(result,
                Utf8ToWide(node->acceleratorText));
        else if (propertyId == UIA_AriaPropertiesPropertyId && node)
            return SetStringVariant(result,
                node->busy ? L"busy=true" : L"");
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
        else if (propertyId == UIA_IsRequiredForFormPropertyId)
            SetBoolVariant(result, node && node->required);
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
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::SelectionItem, node);
        if (FAILED(hr)) return hr;
        return PerformAction(
            AccessibilityPattern::SelectionItem,
            node.canSelectMultiple
                ? LuaWidgetAccessibilityActionKind::AddToSelection
                : LuaWidgetAccessibilityActionKind::Select);
    }

    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override
    {
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::SelectionItem, node);
        if (FAILED(hr)) return hr;
        if (!node.canSelectMultiple) return UIA_E_INVALIDOPERATION;
        return PerformAction(
            AccessibilityPattern::SelectionItem,
            LuaWidgetAccessibilityActionKind::RemoveFromSelection);
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

    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto resolved = ResolveElement(snapshots, reference_);
        if (!resolved || !resolved->nodeIndex)
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto& widget = snapshots[resolved->widgetIndex];
        const auto& node = widget.nodes[*resolved->nodeIndex];
        if (!SupportsPattern(node, AccessibilityPattern::Selection))
            return UIA_E_NOTSUPPORTED;

        std::vector<Microsoft::WRL::ComPtr<IRawElementProviderSimple>>
            selected;
        for (const std::size_t childIndex : node.children)
        {
            if (childIndex >= widget.nodes.size())
                return UIA_E_ELEMENTNOTAVAILABLE;
            const auto& child = widget.nodes[childIndex];
            if (!child.checked.value_or(false)) continue;
            Microsoft::WRL::ComPtr<IRawElementProviderFragment> fragment;
            HRESULT hr = CreateFragmentProvider(state_, root_.Get(),
                NodeReference(widget, child), &fragment);
            if (FAILED(hr)) return hr;
            Microsoft::WRL::ComPtr<IRawElementProviderSimple> simple;
            hr = fragment.As(&simple);
            if (FAILED(hr)) return hr;
            selected.push_back(std::move(simple));
        }

        SAFEARRAY* array = SafeArrayCreateVector(VT_UNKNOWN, 0,
            static_cast<ULONG>(selected.size()));
        if (!array) return E_OUTOFMEMORY;
        for (LONG index = 0;
             index < static_cast<LONG>(selected.size()); ++index)
        {
            HRESULT hr = SafeArrayPutElement(
                array, &index, selected[index].Get());
            if (FAILED(hr))
            {
                SafeArrayDestroy(array);
                return hr;
            }
        }
        *result = array;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Selection, node);
        if (FAILED(hr)) return hr;
        *result = node.canSelectMultiple ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Selection, node);
        if (FAILED(hr)) return hr;
        *result = node.selectionRequired ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Scroll(ScrollAmount horizontalAmount,
        ScrollAmount verticalAmount) override
    {
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Scroll, node);
        if (FAILED(hr)) return hr;
        const ScrollAmount requested = node.scrollHorizontal
            ? horizontalAmount : verticalAmount;
        const ScrollAmount unsupported = node.scrollHorizontal
            ? verticalAmount : horizontalAmount;
        if (unsupported != ScrollAmount_NoAmount)
            return UIA_E_INVALIDOPERATION;
        if (requested == ScrollAmount_NoAmount) return S_OK;

        double delta = 0.0;
        const double viewport = std::max(
            1.0, static_cast<double>(node.scrollViewportExtent));
        const double smallChange = std::max(16.0, viewport / 10.0);
        switch (requested)
        {
        case ScrollAmount_LargeDecrement: delta = -viewport; break;
        case ScrollAmount_SmallDecrement: delta = -smallChange; break;
        case ScrollAmount_LargeIncrement: delta = viewport; break;
        case ScrollAmount_SmallIncrement: delta = smallChange; break;
        default: return E_INVALIDARG;
        }
        const double target = std::clamp(
            static_cast<double>(node.scrollOffset) + delta,
            0.0, ScrollMaximum(node));
        return PerformAction(AccessibilityPattern::Scroll,
            LuaWidgetAccessibilityActionKind::SetScrollOffset, target);
    }

    HRESULT STDMETHODCALLTYPE SetScrollPercent(double horizontalPercent,
        double verticalPercent) override
    {
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Scroll, node);
        if (FAILED(hr)) return hr;
        const double requested = node.scrollHorizontal
            ? horizontalPercent : verticalPercent;
        const double unsupported = node.scrollHorizontal
            ? verticalPercent : horizontalPercent;
        if (unsupported != UIA_ScrollPatternNoScroll)
            return UIA_E_INVALIDOPERATION;
        if (requested == UIA_ScrollPatternNoScroll) return S_OK;
        if (!std::isfinite(requested) || requested < 0.0 ||
            requested > 100.0)
            return E_INVALIDARG;
        if (!Scrollable(node)) return UIA_E_INVALIDOPERATION;
        return PerformAction(AccessibilityPattern::Scroll,
            LuaWidgetAccessibilityActionKind::SetScrollOffset,
            ScrollMaximum(node) * requested / 100.0);
    }

    HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(
        double* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Scroll, node);
        if (FAILED(hr)) return hr;
        *result = node.scrollHorizontal
            ? ScrollPercent(node) : UIA_ScrollPatternNoScroll;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(
        double* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Scroll, node);
        if (FAILED(hr)) return hr;
        *result = node.scrollHorizontal ? ScrollViewSize(node) : 100.0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(
        BOOL* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Scroll, node);
        if (FAILED(hr)) return hr;
        *result = node.scrollHorizontal && Scrollable(node)
            ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(
        double* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Scroll, node);
        if (FAILED(hr)) return hr;
        *result = !node.scrollHorizontal
            ? ScrollPercent(node) : UIA_ScrollPatternNoScroll;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_VerticalViewSize(
        double* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Scroll, node);
        if (FAILED(hr)) return hr;
        *result = !node.scrollHorizontal ? ScrollViewSize(node) : 100.0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(
        BOOL* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Scroll, node);
        if (FAILED(hr)) return hr;
        *result = !node.scrollHorizontal && Scrollable(node)
            ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_RowCount(int* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Grid, node);
        if (FAILED(hr)) return hr;
        if (!node.gridRowCount) return UIA_E_NOTSUPPORTED;
        *result = *node.gridRowCount;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_ColumnCount(int* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::Grid, node);
        if (FAILED(hr)) return hr;
        if (!node.gridColumnCount) return UIA_E_NOTSUPPORTED;
        *result = *node.gridColumnCount;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetItem(int row, int column,
        IRawElementProviderSimple** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        std::vector<LuaWidgetAccessibilitySnapshot> snapshots;
        if (!state_->Capture(snapshots))
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto resolved = ResolveElement(snapshots, reference_);
        if (!resolved || !resolved->nodeIndex)
            return UIA_E_ELEMENTNOTAVAILABLE;
        const auto& widget = snapshots[resolved->widgetIndex];
        const auto& node = widget.nodes[*resolved->nodeIndex];
        if (!SupportsPattern(node, AccessibilityPattern::Grid))
            return UIA_E_NOTSUPPORTED;
        if (row < 0 || column < 0 || !node.gridRowCount ||
            !node.gridColumnCount || row >= *node.gridRowCount ||
            column >= *node.gridColumnCount)
            return E_INVALIDARG;
        const auto child = std::find_if(node.children.begin(),
            node.children.end(), [&](std::size_t childIndex) {
                if (childIndex >= widget.nodes.size()) return false;
                const auto& candidate = widget.nodes[childIndex];
                return candidate.gridRow == row &&
                    candidate.gridColumn == column;
            });
        if (child == node.children.end()) return S_OK;
        Microsoft::WRL::ComPtr<IRawElementProviderFragment> fragment;
        HRESULT create = CreateFragmentProvider(state_, root_.Get(),
            NodeReference(widget, widget.nodes[*child]), &fragment);
        if (FAILED(create)) return create;
        return fragment->QueryInterface(IID_PPV_ARGS(result));
    }

    HRESULT STDMETHODCALLTYPE get_Row(int* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::GridItem, node);
        if (FAILED(hr)) return hr;
        if (!node.gridRow) return UIA_E_NOTSUPPORTED;
        *result = *node.gridRow;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_Column(int* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::GridItem, node);
        if (FAILED(hr)) return hr;
        if (!node.gridColumn) return UIA_E_NOTSUPPORTED;
        *result = *node.gridColumn;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_RowSpan(int* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::GridItem, node);
        if (FAILED(hr)) return hr;
        *result = node.gridRowSpan.value_or(1);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_ColumnSpan(int* result) override
    {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::GridItem, node);
        if (FAILED(hr)) return hr;
        *result = node.gridColumnSpan.value_or(1);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_ContainingGrid(
        IRawElementProviderSimple** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        AccessibilityNode node;
        const HRESULT hr = ResolvePatternNode(
            AccessibilityPattern::GridItem, node);
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
    std::vector<LuaWidgetAccessibilitySnapshot> lastSnapshots;
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
    impl_->lastSnapshots.clear();
    (void)impl_->state->Capture(impl_->lastSnapshots);
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
    impl_->lastSnapshots.clear();
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

void WidgetAccessibilityProviderHost::RefreshEvents() noexcept
{
    if (!impl_ || !impl_->state || !impl_->root) return;
    std::vector<LuaWidgetAccessibilitySnapshot> current;
    if (!impl_->state->Capture(current)) return;
    std::vector<LuaWidgetAccessibilitySnapshot> previous =
        std::move(impl_->lastSnapshots);
    impl_->lastSnapshots = current;
    const auto changes = DiffWidgetAccessibilitySnapshots(
        previous, impl_->lastSnapshots);
    if (changes.empty() || !UiaClientsAreListening()) return;

    Microsoft::WRL::ComPtr<IRawElementProviderFragmentRoot> root;
    if (FAILED(impl_->root.As(&root)) || !root) return;
    const auto providerFor = [&](const WidgetAccessibilityChange& change,
        Microsoft::WRL::ComPtr<IRawElementProviderSimple>& provider) {
        provider.Reset();
        if (change.element == WidgetAccessibilityElementKind::Root)
            return SUCCEEDED(impl_->root.CopyTo(
                provider.GetAddressOf()));
        Microsoft::WRL::ComPtr<IRawElementProviderFragment> fragment;
        if (FAILED(CreateFragmentProvider(impl_->state, root.Get(),
                ChangeReference(change), &fragment)) || !fragment)
            return false;
        return SUCCEEDED(fragment.As(&provider)) && provider;
    };

    for (const auto& change : changes)
    {
        if (change.kind == WidgetAccessibilityChangeKind::Structure)
        {
            (void)UiaRaiseStructureChangedEvent(impl_->root.Get(),
                StructureChangeType_ChildrenInvalidated, nullptr, 0);
            continue;
        }
        Microsoft::WRL::ComPtr<IRawElementProviderSimple> provider;
        if (!providerFor(change, provider)) continue;
        if (change.kind == WidgetAccessibilityChangeKind::Focus)
        {
            (void)UiaRaiseAutomationEvent(
                provider.Get(), UIA_AutomationFocusChangedEventId);
            continue;
        }
        if (change.kind == WidgetAccessibilityChangeKind::LiveRegion)
        {
            (void)UiaRaiseAutomationEvent(
                provider.Get(), UIA_LiveRegionChangedEventId);
            continue;
        }
        const auto propertyId = ChangePropertyId(change.kind);
        if (!propertyId) continue;
        VARIANT oldValue;
        VARIANT newValue;
        VariantInit(&oldValue);
        VariantInit(&newValue);
        if (SUCCEEDED(SetChangeVariant(&oldValue, change.kind,
                impl_->state->window, previous, change)) &&
            SUCCEEDED(SetChangeVariant(&newValue, change.kind,
                impl_->state->window, impl_->lastSnapshots, change)))
        {
            (void)UiaRaiseAutomationPropertyChangedEvent(
                provider.Get(), *propertyId, oldValue, newValue);
        }
        VariantClear(&oldValue);
        VariantClear(&newValue);
    }
}

IRawElementProviderSimple*
WidgetAccessibilityProviderHost::RootProvider() const noexcept
{
    return impl_ ? impl_->root.Get() : nullptr;
}
}
