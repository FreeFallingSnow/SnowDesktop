#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "app_control_runtime.h"
#include "platform/control/control_elements.h"
#include "ui/menu/menu_control_snapshot.h"
#include "widget/view/widget_accessibility_snapshot.h"
#include <cmath>
#include <algorithm>
#include <unordered_set>

namespace
{
using namespace snowdesktop::control;
std::string Key(const std::wstring& value) { return ElementKey(WideToUtf8(value)); }
std::string ItemKey(Item& item)
{
    if (const auto* icon = dynamic_cast<DesktopIcon*>(&item); icon && icon->GetDesktopItem())
        return "desktop/" + Key(icon->GetDesktopItem()->layoutKey);
    if (const auto* widget = dynamic_cast<Widget*>(&item); widget && widget->GetWidgetData())
        return "widget/" + Key(widget->GetWidgetData()->id);
    if (const auto* entry = dynamic_cast<DockEntryItem*>(&item)) return "entry/" + Key(entry->GetReference());
    if (const auto* running = dynamic_cast<DockRunningItem*>(&item)) return "running/" + Key(running->GetIdentityKey());
    return "path/" + Key(item.GetPath()) + "/" + Key(item.GetTitle());
}
RECT ViewBounds(const snowdesktop::widget_runtime::ViewRect& value, POINT origin)
{
    const double left = origin.x + static_cast<double>(value.x), top = origin.y + static_cast<double>(value.y);
    const double right = left + value.width, bottom = top + value.height;
    for (const double number : {left, top, right, bottom})
        if (!std::isfinite(number) || std::abs(number) > 1000000) return {};
    return {static_cast<LONG>(std::floor(left)), static_cast<LONG>(std::floor(top)),
        static_cast<LONG>(std::ceil(right)), static_cast<LONG>(std::ceil(bottom))};
}
}

snowdesktop::control::ElementSnapshot DesktopApp::ApplicationControlElements()
{
    using namespace snowdesktop::control;
    ElementSnapshot result;
    result.coveredSurfaces = {"desktop-items", "native-widget-frames", "native-widget-members",
        "native-widget-search", "native-widget-buttons-tabs-and-details", "dock-items", "quick-navigation", "lua-accessibility", "modern-menus"};
    result.pendingSurfaces = {"native-search-result-extra-controls", "popup-extra-controls", "native-status-bar-and-system-panels",
        "legacy-system-menus", "settings-floating-popups", "system-dialogs-and-external-applications"};
    const auto screen = [&](RECT rect) { OffsetRect(&rect, virtualLeft_, virtualTop_); return rect; };
    const RECT desktop = screen({0, 0, virtualWidth_, virtualHeight_});
    const auto add = [&](std::string id, std::string parent, std::string surface, std::string role,
        std::wstring name, RECT rect, RECT clip, HWND window, std::vector<std::string> actions) -> Element& {
        Element element;
        element.id = std::move(id); element.parent = std::move(parent); element.surface = std::move(surface);
        element.role = std::move(role); element.name = WideToUtf8(name); element.bounds = screen(rect);
        element.clip = screen(clip); element.window = window; element.actions = std::move(actions);
        element.visible = IsWindowVisible(window) != FALSE;
        result.elements.push_back(std::move(element)); return result.elements.back();
    };
    if (generalSettings_.softwareDesktopEnabled && customDesktopVisible_)
    {
        for (const auto& page : gridPages_)
            add("desktop/" + Key(page.id), {}, "desktop", "pane", L"Desktop", page.bounds, page.bounds,
                hwnd_, {"hover", "contextMenu", "drop"});
        for (const auto& item : items_oo_)
        {
            if (dynamic_cast<DesktopIcon*>(item.get()))
            {
                auto& element = add("item/" + ItemKey(*item), {}, "desktop", "item", item->GetTitle(),
                    item->GetBounds(), {0, 0, virtualWidth_, virtualHeight_}, hwnd_,
                    {"click", "doubleClick", "contextMenu", "hover", "drag", "drop"});
                element.selected = item->IsSelected();
            }
        }
        for (const auto& container : containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            if (!widget || !widget->GetWidgetData() || IsGroupedWidget(*widget->GetWidgetData())) continue;
            const auto* data = widget->GetWidgetData();
            const std::string root = "widget/" + Key(data->id);
            const RECT frame = widget->GetFrameRect();
            const auto window = widget->IsPopupHosted() && IsWindow(floatingPopupHwnd_) ? floatingPopupHwnd_ : hwnd_;
            auto& parent = add(root, {}, "widget", "group", widget->GetTitle(), frame, frame, window,
                {"hover", "contextMenu", "scroll", "drop"});
            parent.widgetId = data->id; parent.selected = data->selected; parent.expanded = !widget->IsCollapsed();
            for (const auto& control : widget->GetControlRegions())
            {
                std::vector<std::string> actions{"click", "hover"};
                if (control.hit == WidgetHit::MoveHandle) actions = {"drag", "hover", "contextMenu"};
                if (control.hit == WidgetHit::ResizeHandle || control.role == "separator") actions = {"drag", "hover"};
                auto& child = add(root + "/" + Key(control.key), root, "widget", control.role, control.name,
                    control.bounds, frame, window, std::move(actions));
                child.widgetId = data->id; child.nativeControlKey = WideToUtf8(control.key);
                child.checked = control.checked; child.selected = control.selected;
                if (control.hit == WidgetHit::SearchBox)
                {
                    auto* scrolling = dynamic_cast<ScrollingItemWidget*>(widget);
                    if (!scrolling) continue;
                    child.actions.push_back("setValue"); child.backend = ElementBackend::WidgetSearch;
                    child.value = WideToUtf8(scrolling->GetSearchText()); child.focused = scrolling->IsSearchFocused();
                }
            }
            std::unordered_map<std::string, unsigned> occurrences;
            for (const auto& slot : widget->GetSlots())
            {
                auto* item = slot->GetItem(); if (!item) continue;
                const auto key = ItemKey(*item);
                const auto ordinal = occurrences[key]++;
                auto& child = add(root + "/member/" + key + "/" + std::to_string(ordinal), root, "widget", "item",
                    item->GetTitle(), slot->GetBounds(), widget->GetContentViewportRect(), window,
                    {"click", "doubleClick", "contextMenu", "hover", "drag", "drop"});
                child.selected = item->IsSelected(); child.widgetId = data->id;
            }
        }
    }
    POINT pointer{}; snowdesktop::private_pointer::CursorPosition(&pointer); pointer.x -= virtualLeft_; pointer.y -= virtualTop_;
    for (const auto& host : persistentDockHosts_)
    {
        auto* dock = host->container;
        if (!dock || !IsWindowVisible(host->hwnd) || !IsDockContainerInteractionVisible(dock)) continue;
        MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(host->monitor, &monitor)) continue;
        const auto root = "dock/" + Key(monitor.szDevice);
        const auto frame = dock->GetInteractiveBounds();
        add(root, {}, "dock", "toolbar", L"Dock", frame, frame, host->hwnd, {"hover", "scroll", "drop"});
        for (const auto& entry : std::vector<std::pair<std::string, RECT>>{{"start", dock->GetWindowsButtonRect()}, {"search", dock->GetSearchRect()}})
            if (!IsRectEmpty(&entry.second))
                add(root + "/" + entry.first, root, "dock", "button", entry.first == "start" ? L"Start" : L"Search",
                    dock->GetElementVisualRect(entry.second, pointer), frame, host->hwnd, {"click", "hover"});
        std::unordered_map<std::string, unsigned> occurrences;
        for (const auto& slot : dock->GetSlots())
        {
            auto* item = slot->GetItem(); if (!item) continue;
            const auto key = ItemKey(*item); const auto ordinal = occurrences[key]++;
            add(root + "/" + key + "/" + std::to_string(ordinal), root, "dock", "button", item->GetTitle(),
                dock->GetElementVisualRect(slot->GetBounds(), pointer), frame, host->hwnd,
                {"click", "doubleClick", "contextMenu", "hover", "drag", "drop"});
        }
    }
    if (quickNavigationOpen_ && IsWindowVisible(quickNavigationHwnd_))
    {
        const auto frame = quickNavigationRect_;
        add("navigation", {}, "navigation", "pane", L"Quick Navigation", frame, frame, quickNavigationHwnd_, {"hover", "scroll"});
        auto& edit = add("navigation/search", "navigation", "navigation", "edit", L"Search",
            GetQuickNavigationInputRect(frame), frame, quickNavigationHwnd_, {"click", "hover", "setValue"});
        edit.backend = ElementBackend::NavigationSearch;
        edit.value = WideToUtf8(snowdesktop::text_input::DisplayText(quickNavigationSearchEdit_));
        for (int button = 0; button < 3; ++button)
            add("navigation/toolbar/" + std::to_string(button), "navigation", "navigation", "button",
                button == 0 ? L"Search scope" : button == 1 ? L"Settings" : L"Expand or collapse",
                GetQuickNavigationToolbarRect(frame, button), frame, quickNavigationHwnd_, {"click", "hover"});
        if (UseQuickNavigationList())
        {
            const auto rows = BuildQuickNavigationListRows();
            std::unordered_map<std::string, unsigned> occurrences;
            for (size_t index = 0; index < rows.size(); ++index)
            {
                const auto& row = rows[index];
                const auto key = std::to_string(static_cast<int>(row.kind)) + "/" + Key(row.title) + "/" + Key(row.value);
                auto& element = add("navigation/row/" + key + "/" + std::to_string(occurrences[key]++), "navigation", "navigation", "item",
                    row.title, GetQuickNavigationListRowRect(index), GetQuickNavigationContentRect(frame), quickNavigationHwnd_, {"click", "hover", "contextMenu"});
                element.enabled = row.enabled && row.kind != QuickNavigationListRow::Kind::Header && row.kind != QuickNavigationListRow::Kind::Notice;
            }
        }
        else
        {
            const auto entries = GetQuickNavigationEntries();
            std::unordered_map<std::string, unsigned> occurrences;
            for (const auto& region : quickNavigationHoverRegions_)
            {
                const auto& target = region.target;
                auto key = std::to_string(static_cast<int>(target.kind)) + "/" + std::to_string(target.index);
                std::wstring name = L"Navigation control";
                if (target.kind == QuickNavigationPointerTargetKind::Item && target.index < entries.size())
                {
                    const auto& entry = entries[target.index]; name = entry.name;
                    if (entry.kind == QuickNavigationEntry::Kind::DesktopItem && entry.itemIndex < items_.size())
                        key = "item/desktop/" + Key(items_[entry.itemIndex].layoutKey);
                    else if (entry.widgetIndex < widgets_.size())
                        key = "item/widget/" + Key(widgets_[entry.widgetIndex].id) + "/" + Key(entry.path);
                    else key = "item/path/" + Key(entry.path);
                }
                // A result may have several painted hit regions. They share
                // its model identity but need distinct addressable references.
                add("navigation/control/" + key + "/" + std::to_string(occurrences[key]++), "navigation", "navigation", "button", name,
                    region.bounds, frame, quickNavigationHwnd_, {"click", "hover", "contextMenu"});
            }
        }
    }
    if (widgetEngine_ && generalSettings_.softwareDesktopEnabled && customDesktopVisible_)
    {
        using namespace snowdesktop::widget_runtime;
        for (const auto& widget : widgetEngine_->RuntimeAccessibilitySnapshots())
        {
            if (!widget.error.empty()) continue;
            const auto root = "lua/" + Key(widget.widgetId);
            add(root, {}, "lua", "group", Utf8ToWide(widget.name), widget.bounds, widget.bounds, hwnd_, {"hover", "contextMenu"});
            const POINT origin{widget.bounds.left + virtualLeft_, widget.bounds.top + virtualTop_};
            for (const auto& node : widget.nodes)
            {
                Element element;
                element.id = root + "/" + ElementKey(node.semanticId);
                element.parent = node.parentIndex < widget.nodes.size() ? root + "/" + ElementKey(widget.nodes[node.parentIndex].semanticId) : root;
                element.surface = "lua"; element.role = node.role; element.name = node.name; element.value = node.valueText;
                element.bounds = ViewBounds(node.bounds, origin); element.clip = screen(widget.bounds);
                if (node.clip) { const auto clip = ViewBounds(*node.clip, origin); IntersectRect(&element.clip, &element.clip, &clip); }
                IntersectRect(&element.clip, &element.clip, &desktop);
                element.window = hwnd_; element.enabled = node.enabled && !node.busy; element.visible = !node.offscreen;
                element.focused = node.focused; element.checked = node.checked; element.expanded = node.expanded;
                element.password = node.password; element.backend = ElementBackend::Lua; element.widgetId = widget.widgetId; element.nodeKey = node.key;
                element.actions = {"hover"};
                const auto pattern = [&](ViewAccessibilityPattern value) { return HasViewAccessibilityPattern(node.patterns, value); };
                if (node.focusable) element.actions.push_back("focus");
                if (!node.key.empty())
                {
                    if (pattern(ViewAccessibilityPattern::Invoke)) element.actions.push_back("invoke");
                    if (pattern(ViewAccessibilityPattern::Toggle)) element.actions.push_back("toggle");
                    if (pattern(ViewAccessibilityPattern::SelectionItem)) element.actions.push_back("select");
                    if (pattern(ViewAccessibilityPattern::Value) && !node.valueReadOnly && !node.password) element.actions.push_back("setValue");
                    if (pattern(ViewAccessibilityPattern::RangeValue) && !node.rangeValueReadOnly) element.actions.push_back("setRangeValue");
                    if (pattern(ViewAccessibilityPattern::ExpandCollapse)) { element.actions.push_back("expand"); element.actions.push_back("collapse"); }
                    if (pattern(ViewAccessibilityPattern::Scroll)) element.actions.push_back("setScrollOffset");
                    if (element.actions.size() > 1) { element.actions.push_back("click"); element.actions.push_back("contextMenu"); }
                }
                result.elements.push_back(std::move(element));
            }
        }
    }
    for (const auto& control : snowdesktop::modern_menu::ActiveControls())
    {
        Element element;
        element.id = control.id; element.parent = control.parent; element.surface = "menu";
        element.role = control.pane ? "menu" : control.textInput ? "edit" : "menuitem";
        element.name = WideToUtf8(control.name); element.value = WideToUtf8(control.value);
        element.bounds = control.bounds; element.clip = control.clip; element.window = control.window;
        element.enabled = control.enabled; element.visible = IsWindowVisible(control.window) != FALSE;
        element.focused = control.focused; element.selected = control.selected;
        element.actions = control.pane ? std::vector<std::string>{"hover", "scroll"} : std::vector<std::string>{"hover", "click"};
        if (!control.pane) element.checked = control.checked;
        if (control.hasChildren) element.expanded = control.expanded;
        if (control.textInput)
        {
            element.backend = ElementBackend::MenuText; element.nodeKey = control.id;
            element.actions.push_back("setValue");
        }
        result.elements.push_back(std::move(element));
    }
    if (const HWND companion = snowdesktop::modern_menu::ActiveCompanionWindow())
    {
        RECT bounds{};
        if (GetWindowRect(companion, &bounds))
        {
            Element element;
            element.id = "component-preview/active";
            element.surface = "component-preview"; element.role = "pane";
            element.name = "Component preview"; element.bounds = element.clip = bounds;
            element.window = companion; element.visible = IsWindowVisible(companion) != FALSE;
            element.actions = {"hover", "click"};
            result.elements.push_back(std::move(element));
            result.coveredSurfaces.push_back("product-component-preview-window");
            result.pendingSurfaces.push_back("component-preview-button-catalog");
        }
    }
    const auto settings = applicationControlRuntime_->settings.Elements(settingsWindow_ ? settingsWindow_->Window() : nullptr);
    if (!settings.empty())
    {
        result.coveredSurfaces.push_back("settings-uia-cached");
        result.elements.insert(result.elements.end(), settings.begin(), settings.end());
    }
    else result.pendingSurfaces.push_back("settings-window-needs-refresh");
    if (applicationControlRuntime_->settings.Current().truncated)
        result.pendingSurfaces.push_back("settings-traversal-truncated");
    // The unreleased semantic action dispatcher has been removed. Catalogs
    // describe pointer affordances; clients dispatch real coordinate events.
    for (auto& element : result.elements)
        std::erase_if(element.actions, [](const auto& action) {
            return action != "hover" && action != "click" && action != "doubleClick" &&
                action != "contextMenu" && action != "drag" && action != "drop" && action != "scroll";
        });
    result.Finish(); return result;
}
