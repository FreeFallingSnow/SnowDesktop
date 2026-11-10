#include "status_bar_application_menu_uia.h"

#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace snowdesktop::status_bar_application_menu
{
namespace
{
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int kMaxNodes = 96, kMaxDepth = 8;
constexpr UINT kMaxHeadings = 32;
constexpr auto kRefreshInterval = 2s;
constexpr auto kMissingMenuInterval = 5s;
constexpr auto kProbeBudget = 350ms;

enum class Action { Expand, Invoke };

struct Entry
{
    ComPtr<IUIAutomationElement> element;
    Heading heading;
    Action action;
    bool edgeButton;
};

bool IsEdge(DWORD processId)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;
    wchar_t path[1024]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool read = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!read) return false;
    const std::wstring_view image(path, length);
    const auto name = image.substr(image.find_last_of(L"\\/") + 1);
    constexpr std::wstring_view edge = L"msedge.exe";
    return CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
        edge.data(), static_cast<int>(edge.size()), TRUE) == CSTR_EQUAL;
}

std::wstring Label(IUIAutomationElement* element, bool edgeButton)
{
    BSTR raw = nullptr;
    const HRESULT result = element->get_CurrentName(&raw);
    const std::unique_ptr<OLECHAR, decltype(&SysFreeString)> name(raw, SysFreeString);
    if (FAILED(result) || !name) return {};
    std::wstring_view label(name.get(), SysStringLen(name.get()));
    constexpr std::wstring_view edgeShortcut = L" (Alt+F)";
    if (edgeButton && label.ends_with(edgeShortcut)) label.remove_suffix(edgeShortcut.size());
    return label.size() <= 256 ? std::wstring(label) : std::wstring{};
}

bool MatchesId(IUIAutomationElement* element, std::wstring_view expected)
{
    BSTR raw = nullptr;
    const HRESULT result = element->get_CurrentAutomationId(&raw);
    const std::unique_ptr<OLECHAR, decltype(&SysFreeString)> id(raw, SysFreeString);
    return SUCCEEDED(result) && id && std::wstring_view(id.get(), SysStringLen(id.get())) == expected;
}

bool VisibleInProcess(IUIAutomationElement* element, DWORD process)
{
    BOOL offscreen = TRUE;
    int owner = 0;
    return SUCCEEDED(element->get_CurrentProcessId(&owner)) && static_cast<DWORD>(owner) == process &&
        SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && !offscreen;
}

ComPtr<IUIAutomationExpandCollapsePattern> ExpandPattern(IUIAutomationElement* element, bool edgeButton)
{
    ComPtr<IUIAutomationExpandCollapsePattern> expand;
    if (FAILED(element->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&expand))) || !expand) return {};
    // The inspected Edge app-menu button exposes Expand while reporting LeafNode.
    if (edgeButton) return expand;
    ExpandCollapseState state = ExpandCollapseState_LeafNode;
    if (FAILED(expand->get_CurrentExpandCollapseState(&state)) || state == ExpandCollapseState_LeafNode) return {};
    return expand;
}

std::optional<Entry> ReadEntry(ComPtr<IUIAutomationElement> element, DWORD process, bool edgeButton)
{
    if (!VisibleInProcess(element.Get(), process)) return {};
    auto label = Label(element.Get(), edgeButton);
    if (label.empty()) return {};
    BOOL enabled = FALSE;
    if (FAILED(element->get_CurrentIsEnabled(&enabled))) return {};
    Action action = Action::Expand;
    if (!ExpandPattern(element.Get(), edgeButton))
    {
        ComPtr<IUIAutomationInvokePattern> invoke;
        if (FAILED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) || !invoke) return {};
        action = Action::Invoke;
    }
    return Entry{std::move(element), {0, 0, std::move(label), action == Action::Expand, enabled != FALSE},
        action, edgeButton};
}

template<class Continue>
std::vector<Entry> ReadMenuBar(IUIAutomationTreeWalker* walker, IUIAutomationElement* bar, DWORD process,
    const Continue& proceed)
{
    std::vector<Entry> entries;
    ComPtr<IUIAutomationElement> child;
    if (FAILED(walker->GetFirstChildElement(bar, &child))) return entries;
    for (UINT index = 0; child && index < kMaxHeadings && proceed(); ++index)
    {
        CONTROLTYPEID type = 0;
        if (SUCCEEDED(child->get_CurrentControlType(&type)) && type == UIA_MenuItemControlTypeId)
            if (auto entry = ReadEntry(child, process, false)) entries.push_back(std::move(*entry));
        ComPtr<IUIAutomationElement> next;
        if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) break;
        child = std::move(next);
    }
    return entries;
}

template<class Continue>
std::vector<Entry> Discover(IUIAutomation* automation, IUIAutomationTreeWalker* walker,
    const Source& source, bool edge, const Continue& proceed)
{
    ComPtr<IUIAutomationElement> root;
    if (!proceed() || FAILED(automation->ElementFromHandle(source.window, &root)) || !root) return {};
    struct Node { ComPtr<IUIAutomationElement> element; int depth; bool edgeToolbar; };
    std::deque<Node> queue;
    queue.push_back({std::move(root), 0, false});
    std::optional<Entry> edgeMenu;
    int added = 1;
    while (!queue.empty() && proceed())
    {
        auto node = std::move(queue.front());
        queue.pop_front();
        auto* element = node.element.Get();
        CONTROLTYPEID type = 0;
        if (FAILED(element->get_CurrentControlType(&type))) continue;
        if (node.depth && (type == UIA_DocumentControlTypeId || type == UIA_TitleBarControlTypeId ||
            type == UIA_WindowControlTypeId || type == UIA_MenuControlTypeId)) continue;
        if (type == UIA_MenuBarControlTypeId)
        {
            auto entries = ReadMenuBar(walker, element, source.process, proceed);
            if (!entries.empty()) return entries;
            continue;
        }
        if (node.edgeToolbar && type == UIA_ButtonControlTypeId && MatchesId(element, L"view_1011"))
            edgeMenu = ReadEntry(node.element, source.process, true);
        if (node.depth >= kMaxDepth || added >= kMaxNodes || !proceed()) continue;
        const bool edgeToolbar = edge && type == UIA_ToolBarControlTypeId && MatchesId(element, L"view_1000");
        ComPtr<IUIAutomationElement> child;
        if (FAILED(walker->GetFirstChildElement(element, &child))) continue;
        while (child && added < kMaxNodes && proceed())
        {
            queue.push_back({child, node.depth + 1, edgeToolbar});
            ++added;
            ComPtr<IUIAutomationElement> next;
            if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) break;
            child = std::move(next);
        }
    }
    std::vector<Entry> entries;
    if (edgeMenu) entries.push_back(std::move(*edgeMenu));
    return entries;
}

template<class Continue>
void InvokeEntry(const Entry& entry, const Source& source, const Continue& proceed)
{
    auto* element = entry.element.Get();
    BOOL enabled = FALSE;
    if (!proceed() || !VisibleInProcess(element, source.process) ||
        FAILED(element->get_CurrentIsEnabled(&enabled)) || !enabled ||
        Label(element, entry.edgeButton) != entry.heading.label) return;
    if (entry.action == Action::Expand)
    {
        const auto expand = ExpandPattern(element, entry.edgeButton);
        ExpandCollapseState state = ExpandCollapseState_LeafNode;
        if (expand && SUCCEEDED(expand->get_CurrentExpandCollapseState(&state)) &&
            state != ExpandCollapseState_Expanded && proceed())
            (void)expand->Expand();
    }
    else
    {
        ComPtr<IUIAutomationInvokePattern> invoke;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) &&
            invoke && proceed()) (void)invoke->Invoke();
    }
}
}

struct AutomationMenu::Impl
{
    explicit Impl(UINT message) : readyMessage(message), worker([this](std::stop_token stop) { Work(stop); }) {}

    const UINT readyMessage;
    std::mutex mutex;
    std::condition_variable_any ready;
    std::uint64_t revision = 0;
    Source desired;
    HWND receiver = nullptr;
    Snapshot snapshot;
    std::optional<Target> pending;
    std::jthread worker;

    void Work(std::stop_token stop) noexcept try
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        const struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
        ComPtr<IUIAutomation> automation;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) return;
        ComPtr<IUIAutomation2> timeouts;
        if (FAILED(automation.As(&timeouts)) ||
            FAILED(timeouts->put_ConnectionTimeout(250)) || FAILED(timeouts->put_TransactionTimeout(250))) return;
        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(automation->get_ControlViewWalker(&walker))) return;

        Source source;
        bool edge = false;
        std::uint64_t currentRevision = 0;
        UINT nextCommand = 0;
        std::vector<Entry> entries;
        auto nextScan = Clock::time_point::max();
        while (!stop.stop_requested())
        {
            std::optional<Target> activation;
            bool sourceChanged = false;
            {
                std::unique_lock lock(mutex);
                const auto hasWork = [&] { return revision != currentRevision || pending.has_value(); };
                if (nextScan == Clock::time_point::max()) ready.wait(lock, stop, hasWork);
                else ready.wait_until(lock, stop, nextScan, hasWork);
                if (stop.stop_requested()) return;
                sourceChanged = revision != currentRevision;
                if (sourceChanged)
                {
                    currentRevision = revision;
                    source = desired;
                }
                activation = std::exchange(pending, std::nullopt);
            }
            if (sourceChanged)
            {
                entries.clear();
                edge = source.window && IsEdge(source.process);
                nextScan = source.window ? Clock::now() : Clock::time_point::max();
            }
            const auto deadline = Clock::now() + kProbeBudget;
            const auto sourceCurrent = [&] {
                if (stop.stop_requested() || Clock::now() >= deadline || !source.Live()) return false;
                std::lock_guard lock(mutex);
                return revision == currentRevision;
            };
            if (activation && activation->source == source)
            {
                const auto canInvoke = [&] {
                    return GetForegroundWindow() == source.window && IsWindowVisible(source.window) &&
                        !IsIconic(source.window) && sourceCurrent();
                };
                const auto it = std::find_if(entries.begin(), entries.end(),
                    [&](const Entry& entry) { return entry.heading == activation->heading && entry.heading.enabled; });
                if (it != entries.end()) InvokeEntry(*it, source, canInvoke);
            }
            if (!source.window || Clock::now() < nextScan) continue;
            auto found = Discover(automation.Get(), walker.Get(), source, edge, sourceCurrent);
            // Some providers hide menu controls while inactive. Keep the known
            // entries for the selected window and recheck them on activation.
            const bool retain = found.empty() && !entries.empty() && GetForegroundWindow() != source.window;
            nextScan = Clock::now() + (found.empty() && !retain ? kMissingMenuInterval : kRefreshInterval);
            if (retain || !sourceCurrent()) continue;
            Snapshot result;
            if (!found.empty()) result.source = source;
            for (std::size_t i = 0; i < found.size(); ++i)
            {
                auto& entry = found[i];
                entry.heading.position = static_cast<UINT>(i);
                BOOL same = FALSE;
                if (i < entries.size() && entry.heading.label == entries[i].heading.label &&
                    entry.action == entries[i].action &&
                    SUCCEEDED(automation->CompareElements(entry.element.Get(), entries[i].element.Get(), &same)) && same)
                    entry.heading.command = entries[i].heading.command;
                else entry.heading.command = ++nextCommand;
                result.headings.push_back(entry.heading);
            }
            entries = std::move(found);
            if (stop.stop_requested()) return;
            std::lock_guard lock(mutex);
            if (revision != currentRevision || result == snapshot) continue;
            snapshot = std::move(result);
            DWORD process = 0;
            if (receiver && GetWindowThreadProcessId(receiver, &process) && process == GetCurrentProcessId())
                PostMessageW(receiver, readyMessage, TRUE, 0);
        }
    }
    catch (...) { /* An unavailable automation provider leaves the native menu path usable. */ }
};

AutomationMenu::AutomationMenu(UINT readyMessage) : impl_(std::make_unique<Impl>(readyMessage)) {}
AutomationMenu::~AutomationMenu() = default;

Snapshot AutomationMenu::Refresh(HWND window, HWND receiver)
{
    Source source;
    source.window = window;
    if (window) source.thread = GetWindowThreadProcessId(window, &source.process);
    if (!source.thread || source.process == GetCurrentProcessId()) source = {};
    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    if (receiver || !source.window) impl.receiver = receiver;
    if (source != impl.desired)
    {
        impl.desired = source;
        ++impl.revision;
        impl.snapshot = {};
        impl.pending.reset();
        impl.ready.notify_one();
    }
    return impl.snapshot;
}

void AutomationMenu::Activate(const Target& target)
{
    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    if (target.source != impl.snapshot.source || !target.heading.enabled ||
        std::find(impl.snapshot.headings.begin(), impl.snapshot.headings.end(), target.heading) == impl.snapshot.headings.end()) return;
    impl.pending = target;
    impl.ready.notify_one();
}
}
