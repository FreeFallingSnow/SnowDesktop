#include "settings_automation.h"
#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace snowdesktop::settings_automation
{
namespace
{
using Microsoft::WRL::ComPtr;
using control::Element;
using control::Error;
using Clock = std::chrono::steady_clock;
std::uint64_t UnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
void Check(HRESULT result)
{
    if (FAILED(result)) throw Error("settings_provider_failure", "UI Automation provider call failed");
}
struct Apartment
{
    Apartment() { Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~Apartment() { CoUninitialize(); }
};
struct Target
{
    HWND window = nullptr;
    DWORD process = 0;
    std::uint64_t creation = 0;
    bool operator==(const Target&) const = default;
    std::string Key() const
    {
        return "settings/" + std::to_string(process) + "/" + std::to_string(creation) + "/" +
            std::to_string(reinterpret_cast<std::uintptr_t>(window));
    }
};
Target Identify(HWND window)
{
    Target result; result.window = window;
    if (!IsWindow(window) || !IsWindowVisible(window)) throw Error("settings_not_ready", "Settings window is absent or hidden");
    GetWindowThreadProcessId(window, &result.process);
    if (!result.process || result.process == GetCurrentProcessId())
        throw Error("settings_target_rejected", "Expected the owned settings child process");
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, result.process);
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool valid = process && GetProcessTimes(process, &created, &exited, &kernel, &user);
    if (process) CloseHandle(process);
    if (!valid) throw Error("settings_not_ready", "Cannot identify settings process lifetime");
    result.creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return result;
}
std::string RuntimeId(IUIAutomationElement* element)
{
    SAFEARRAY* raw = nullptr;
    Check(element->GetRuntimeId(&raw));
    if (!raw) throw Error("settings_identity_unavailable", "Provider supplied no runtime identity");
    struct Array { SAFEARRAY* value; ~Array() { SafeArrayDestroy(value); } } array{raw};
    LONG lower = 0, upper = -1;
    Check(SafeArrayGetLBound(raw, 1, &lower)); Check(SafeArrayGetUBound(raw, 1, &upper));
    if (SafeArrayGetDim(raw) != 1 || upper < lower || upper - lower > 64)
        throw Error("settings_identity_unavailable", "Invalid runtime identity");
    std::string id;
    for (LONG index = lower; index <= upper; ++index)
    {
        int part = 0; Check(SafeArrayGetElement(raw, &index, &part));
        id += "/" + std::to_string(part);
    }
    return id;
}
std::string Text(BSTR raw, std::size_t limit = 256)
{
    if (!raw) return {};
    std::wstring text(raw, std::min<std::size_t>(SysStringLen(raw), limit));
    SysFreeString(raw);
    // Never leave half a UTF-16 surrogate at the bounded text edge.
    if (!text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) text.pop_back();
    if (text.empty()) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw Error("settings_invalid_text", "Provider returned invalid UTF-16");
    std::string result(static_cast<std::size_t>(length), '\0');
    Check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr) == length ? S_OK : E_FAIL);
    return result;
}
template<class T> ComPtr<T> Pattern(IUIAutomationElement* element, PATTERNID pattern)
{
    ComPtr<T> result;
    if (FAILED(element->GetCachedPatternAs(pattern, __uuidof(T), reinterpret_cast<void**>(result.GetAddressOf())))) return {};
    return result;
}
template<class T> ComPtr<T> RequiredPattern(IUIAutomationElement* element, PATTERNID pattern)
{
    auto result = Pattern<T>(element, pattern);
    if (!result) throw Error("unsupported_action", "Settings pattern retired before dispatch");
    return result;
}
std::string Role(CONTROLTYPEID type)
{
    switch (type)
    {
    case UIA_ButtonControlTypeId: return "button";
    case UIA_CheckBoxControlTypeId: return "checkbox";
    case UIA_RadioButtonControlTypeId: return "radio";
    case UIA_EditControlTypeId: return "edit";
    case UIA_ComboBoxControlTypeId: return "combobox";
    case UIA_TabItemControlTypeId: return "tab";
    case UIA_ListItemControlTypeId: return "item";
    case UIA_SliderControlTypeId: return "slider";
    case UIA_TextControlTypeId: return "text";
    case UIA_WindowControlTypeId: return "window";
    default: return "group";
    }
}
struct Node { Element element; ComPtr<IUIAutomationElement> provider; };
struct Catalog { std::vector<Node> nodes; bool truncated = false; };
Catalog Read(IUIAutomation* automation, const Target& target, const std::atomic<bool>& stopping)
{
    if (!(Identify(target.window) == target)) throw Error("settings_target_changed", "Settings process changed");
    ComPtr<IUIAutomationCacheRequest> cache;
    Check(automation->CreateCacheRequest(&cache));
    for (const auto property : {UIA_ProcessIdPropertyId, UIA_ControlTypePropertyId, UIA_NamePropertyId,
        UIA_BoundingRectanglePropertyId, UIA_IsEnabledPropertyId, UIA_IsOffscreenPropertyId,
        UIA_HasKeyboardFocusPropertyId, UIA_IsKeyboardFocusablePropertyId, UIA_IsPasswordPropertyId})
        Check(cache->AddProperty(property));
    for (const auto pattern : {UIA_InvokePatternId, UIA_TogglePatternId, UIA_SelectionItemPatternId,
        UIA_ValuePatternId, UIA_RangeValuePatternId, UIA_ExpandCollapsePatternId, UIA_ScrollItemPatternId,
        UIA_WindowPatternId}) Check(cache->AddPattern(pattern));
    Check(cache->put_TreeScope(TreeScope_Element));
    ComPtr<IUIAutomationTreeWalker> walker;
    Check(automation->get_ControlViewWalker(&walker));
    ComPtr<IUIAutomationElement> root;
    Check(automation->ElementFromHandleBuildCache(target.window, cache.Get(), &root));
    RECT clip{}; GetWindowRect(target.window, &clip);
    struct Pending { ComPtr<IUIAutomationElement> element; std::string parent; unsigned depth; };
    std::vector<Pending> pending{{root, {}, 0}};
    Catalog result;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!pending.empty())
    {
        if (stopping.load()) throw Error("settings_stopped", "Settings automation is stopping");
        if (result.nodes.size() >= 256 || Clock::now() >= deadline) { result.truncated = true; break; }
        auto current = std::move(pending.back()); pending.pop_back();
        int process = 0; Check(current.element->get_CachedProcessId(&process));
        if (process != static_cast<int>(target.process)) continue;
        Node node; node.provider = current.element;
        auto& element = node.element;
        element.id = current.depth ? target.Key() + RuntimeId(current.element.Get()) : target.Key();
        element.parent = current.parent; element.surface = "settings";
        element.backend = control::ElementBackend::Settings; element.window = target.window;
        CONTROLTYPEID type = 0; Check(current.element->get_CachedControlType(&type)); element.role = Role(type);
        BSTR name = nullptr; Check(current.element->get_CachedName(&name)); element.name = Text(name);
        Check(current.element->get_CachedBoundingRectangle(&element.bounds)); element.clip = clip;
        BOOL flag = FALSE; Check(current.element->get_CachedIsEnabled(&flag)); element.enabled = flag != FALSE;
        Check(current.element->get_CachedIsOffscreen(&flag)); element.visible = flag == FALSE;
        Check(current.element->get_CachedHasKeyboardFocus(&flag)); element.focused = flag != FALSE;
        Check(current.element->get_CachedIsPassword(&flag)); element.password = flag != FALSE;
        Check(current.element->get_CachedIsKeyboardFocusable(&flag)); if (flag) element.actions.push_back("focus");
        if (Pattern<IUIAutomationInvokePattern>(current.element.Get(), UIA_InvokePatternId)) element.actions.push_back("invoke");
        if (auto pattern = Pattern<IUIAutomationTogglePattern>(current.element.Get(), UIA_TogglePatternId))
        {
            ToggleState state{}; Check(pattern->get_CurrentToggleState(&state));
            if (state != ToggleState_Indeterminate) element.checked = state == ToggleState_On;
            element.actions.push_back("toggle");
        }
        if (auto pattern = Pattern<IUIAutomationSelectionItemPattern>(current.element.Get(), UIA_SelectionItemPatternId))
        {
            Check(pattern->get_CurrentIsSelected(&flag)); element.selected = flag != FALSE; element.actions.push_back("select");
        }
        if (!element.password)
            if (auto pattern = Pattern<IUIAutomationValuePattern>(current.element.Get(), UIA_ValuePatternId))
            {
                BSTR value = nullptr; Check(pattern->get_CurrentValue(&value)); element.value = Text(value, 1024);
                Check(pattern->get_CurrentIsReadOnly(&flag)); if (!flag) element.actions.push_back("setValue");
            }
        if (auto pattern = Pattern<IUIAutomationRangeValuePattern>(current.element.Get(), UIA_RangeValuePatternId))
        {
            Check(pattern->get_CurrentIsReadOnly(&flag)); if (!flag) element.actions.push_back("setRangeValue");
        }
        if (auto pattern = Pattern<IUIAutomationExpandCollapsePattern>(current.element.Get(), UIA_ExpandCollapsePatternId))
        {
            ExpandCollapseState state{}; Check(pattern->get_CurrentExpandCollapseState(&state));
            if (state != ExpandCollapseState_LeafNode)
            {
                element.expanded = state != ExpandCollapseState_Collapsed;
                element.actions.push_back("expand"); element.actions.push_back("collapse");
            }
        }
        if (Pattern<IUIAutomationScrollItemPattern>(current.element.Get(), UIA_ScrollItemPatternId)) element.actions.push_back("scrollIntoView");
        if (!current.depth && Pattern<IUIAutomationWindowPattern>(current.element.Get(), UIA_WindowPatternId)) element.actions.push_back("close");
        const auto parent = element.id;
        result.nodes.push_back(std::move(node));
        if (current.depth >= 32) { result.truncated = true; continue; }
        ComPtr<IUIAutomationElement> child;
        Check(walker->GetFirstChildElementBuildCache(current.element.Get(), cache.Get(), &child));
        // Bound queued siblings too; a provider cannot allocate an unbounded tree.
        std::vector<ComPtr<IUIAutomationElement>> children;
        while (child)
        {
            if (children.size() + pending.size() + result.nodes.size() >= 256 || Clock::now() >= deadline || stopping.load())
            { result.truncated = true; break; }
            children.push_back(child);
            ComPtr<IUIAutomationElement> next;
            Check(walker->GetNextSiblingElementBuildCache(child.Get(), cache.Get(), &next)); child = std::move(next);
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) pending.push_back({*it, parent, current.depth + 1});
    }
    return result;
}
void Apply(Node& node, const std::string& action, const std::wstring& value, double number)
{
    // This node was freshly rediscovered inside the current owned root. Cached
    // catalog geometry alone never authorizes a semantic call.
    if (!node.element.enabled) throw Error("element_disabled", "Settings element is disabled");
    if (!control::Supports(node.element, action)) throw Error("unsupported_action", "Settings pattern is unavailable");
    const auto visible = control::VisibleBounds(node.element);
    if (action != "scrollIntoView" && (!node.element.visible || IsRectEmpty(&visible)))
        throw Error("element_not_visible", "Settings element is no longer visible");
    auto* element = node.provider.Get();
    if (action == "focus") Check(element->SetFocus());
    else if (action == "invoke") Check(RequiredPattern<IUIAutomationInvokePattern>(element, UIA_InvokePatternId)->Invoke());
    else if (action == "toggle") Check(RequiredPattern<IUIAutomationTogglePattern>(element, UIA_TogglePatternId)->Toggle());
    else if (action == "select") Check(RequiredPattern<IUIAutomationSelectionItemPattern>(element, UIA_SelectionItemPatternId)->Select());
    else if (action == "setValue")
    {
        struct String { BSTR value; ~String() { SysFreeString(value); } } text{SysAllocStringLen(value.data(), static_cast<UINT>(value.size()))};
        if (!text.value) throw Error("settings_allocation_failed", "Cannot allocate UIA text");
        Check(RequiredPattern<IUIAutomationValuePattern>(element, UIA_ValuePatternId)->SetValue(text.value));
    }
    else if (action == "setRangeValue") Check(RequiredPattern<IUIAutomationRangeValuePattern>(element, UIA_RangeValuePatternId)->SetValue(number));
    else if (action == "expand") Check(RequiredPattern<IUIAutomationExpandCollapsePattern>(element, UIA_ExpandCollapsePatternId)->Expand());
    else if (action == "collapse") Check(RequiredPattern<IUIAutomationExpandCollapsePattern>(element, UIA_ExpandCollapsePatternId)->Collapse());
    else if (action == "scrollIntoView") Check(RequiredPattern<IUIAutomationScrollItemPattern>(element, UIA_ScrollItemPatternId)->ScrollIntoView());
    else if (action == "close") Check(RequiredPattern<IUIAutomationWindowPattern>(element, UIA_WindowPatternId)->Close());
    else throw Error("unsupported_action", "Settings action has no adapter");
}
}

struct Service::Impl
{
    mutable std::mutex mutex;
    State state;
    Target target;
    std::vector<Element> elements;
    std::atomic<bool> stopping{false};
    std::thread worker;
    std::uint64_t sequence = 0;
};
Service::Service() : impl_(std::make_unique<Impl>()) {}
Service::~Service() { Stop(); }
State Service::Current() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
std::vector<Element> Service::Elements(HWND window) const
{
    try
    {
        const auto target = Identify(window);
        std::lock_guard lock(impl_->mutex);
        return target == impl_->target ? impl_->elements : std::vector<Element>{};
    }
    catch (...) { return {}; }
}
bool Service::Refresh(HWND window, std::string& error) { return Start(window, nullptr, {}, {}, 0, error); }
bool Service::Perform(HWND window, const Element& element, std::string action, std::wstring value, double number, std::string& error)
{
    if (element.backend != control::ElementBackend::Settings || !control::Supports(element, action) ||
        (element.password && action == "setValue") || !std::isfinite(number) || value.size() > 4096)
    { error = "invalid_settings_action"; return false; }
    return Start(window, &element, std::move(action), std::move(value), number, error);
}
bool Service::Start(HWND window, const Element* element, std::string action, std::wstring value, double number, std::string& error)
{
    error.clear();
    if (Current().status == "running") { error = "settings_busy"; return false; }
    Target target;
    try { target = Identify(window); }
    catch (const Error& failure) { error = failure.code; return false; }
    if (element && element->id != target.Key() && element->id.rfind(target.Key() + "/", 0) != 0)
    { error = "settings_target_changed"; return false; }
    if (impl_->worker.joinable()) impl_->worker.join();
    State state; state.id = std::to_string(++impl_->sequence); state.status = "running";
    state.kind = element ? "action" : "refresh"; state.elementId = element ? element->id : "";
    state.action = std::move(action); state.startedUnixMs = UnixMs();
    { std::lock_guard lock(impl_->mutex); impl_->state = state; impl_->elements.clear(); impl_->target = target; }
    impl_->stopping.store(false);
    try
    {
        impl_->worker = std::thread([this, target, state, expected = element ? *element : Element{}, value = std::move(value), number]() mutable {
            std::vector<Element> elements;
            try
            {
                Apartment apartment;
                ComPtr<IUIAutomation> automation;
                Check(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)));
                ComPtr<IUIAutomation2> options; Check(automation.As(&options));
                Check(options->put_ConnectionTimeout(1000)); Check(options->put_TransactionTimeout(1500));
                Check(options->put_AutoSetFocus(FALSE));
                auto catalog = Read(automation.Get(), target, impl_->stopping);
                state.truncated = catalog.truncated; state.elementCount = catalog.nodes.size();
                if (state.kind == "action")
                {
                    const auto found = std::find_if(catalog.nodes.begin(), catalog.nodes.end(), [&](const Node& node) { return node.element.id == state.elementId; });
                    if (found == catalog.nodes.end()) throw Error("element_not_found", "Settings element retired or was outside the bounded tree");
                    if (impl_->stopping.load() || !(Identify(target.window) == target)) throw Error("settings_target_changed", "Settings target retired before action");
                    if (control::Serialize(control::ElementJson(found->element)) != control::Serialize(control::ElementJson(expected)))
                        throw Error("stale_settings_element", "Settings element changed; refresh before retrying");
                    Apply(*found, state.action, value, number);
                }
                else for (auto& node : catalog.nodes) elements.push_back(std::move(node.element));
                state.status = "completed";
            }
            catch (const Error& failure) { state.status = "failed"; state.error = failure.code; }
            catch (...) { state.status = "failed"; state.error = "settings_automation_failure"; }
            if (impl_->stopping.load()) { state.status = "stopped"; state.error = "settings_stopped"; elements.clear(); }
            state.finishedUnixMs = UnixMs();
            std::lock_guard lock(impl_->mutex); impl_->state = std::move(state); impl_->elements = std::move(elements);
        });
    }
    catch (...) { std::lock_guard lock(impl_->mutex); impl_->state.status = "failed"; impl_->state.error = "settings_worker_failed"; error = impl_->state.error; return false; }
    return true;
}
void Service::Stop() noexcept
{
    impl_->stopping.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard lock(impl_->mutex); impl_->elements.clear();
}
}
