#include "app/app.h"
#include "app_control_runtime.h"
#include "platform/control/control_elements.h"
#include "widget/view/widget_accessibility_snapshot.h"
#include <cmath>

namespace
{
using namespace snowdesktop::control;
bool Flag(const Value& params, const char* key, bool fallback = false)
{
    const auto* value = params.Find(key);
    if (!value) return fallback;
    if (!value->IsBoolean()) throw Error("invalid_params", std::string(key) + " must be boolean", -32602);
    return value->boolean;
}
std::wstring InputText(const Value& params)
{
    const auto* value = params.Find("value");
    if (!value || !value->IsString() || value->string.size() > 4096 || value->string.find('\0') != std::string::npos)
        throw Error("invalid_params", "value must be a string of at most 4096 UTF-8 bytes", -32602);
    return Utf8ToWide(value->string);
}
double InputNumber(const Value& params)
{
    const auto* value = params.Find("number");
    if (!value || !value->IsNumber() || !std::isfinite(value->number))
        throw Error("invalid_params", "number must be finite", -32602);
    return value->number;
}
}

snowdesktop::control::Value DesktopApp::ApplicationControlPointerState(bool includeTrace) const
{
    using namespace snowdesktop::control;
    const auto& state = applicationControlRuntime_->pointer.Current();
    const auto sample = [](const snowdesktop::controlled_pointer::Sample& value) {
        return Object({{"timeMs", Number(value.milliseconds)}, {"x", Number(value.point.x)}, {"y", Number(value.point.y)},
            {"buttons", Number(value.buttons)}, {"phase", String(value.phase)}});
    };
    auto result = Object({{"operationId", String(std::to_string(state.operation))}, {"status", String(state.status)},
        {"error", String(state.error)}, {"elementId", String(state.plan.sourceId)},
        {"targetId", String(state.plan.destinationId)}, {"cursor", sample(state.current)}});
    if (includeTrace)
    {
        auto trace = Array(); for (const auto& entry : state.trace) trace.array.push_back(sample(entry));
        result.object.emplace("trace", std::move(trace));
    }
    if (state.interruption)
    {
        const auto& event = *state.interruption;
        result.object.emplace("interruption", Object({{"device", String(event.device)},
            {"message", Number(event.message)}, {"flags", Number(event.flags)},
            {"injected", Boolean(event.injected)}, {"lowerIntegrity", Boolean(event.lowerIntegrity)},
            {"extraInfo", String(std::to_string(event.extraInfo))},
            {"expectedExtraInfo", String(std::to_string(event.expectedExtraInfo))},
            {"x", Number(event.point.x)}, {"y", Number(event.point.y)},
            {"virtualKey", Number(event.virtualKey)}}));
    }
    return result;
}

snowdesktop::control::Value DesktopApp::PerformApplicationControlElement(const snowdesktop::control::Value& params)
{
    using namespace snowdesktop::control;
    namespace pointer = snowdesktop::controlled_pointer;
    Fields(params, {"elementId", "action", "ifSnapshotRevision", "targetId", "value", "number", "durationMs", "showCursor"});
    const auto action = Text(params, "action");
    const auto id = Text(params, "elementId");
    if (applicationControlRuntime_->pointer.Current().status == "running" || mouseDown_ || dragSession_.IsActive() ||
        widgetAction_ != WidgetAction::None)
        throw Error("busy", "Another pointer or widget operation is active");
    if (desktopStartupPresentationPending_ || popupAnimation_.IsAnimating() || quickNavigationAnimation_.IsAnimating())
        throw Error("not_ready", "Wait for presentation and opening animations to finish");
    auto snapshot = ApplicationControlElements();
    if (Text(params, "ifSnapshotRevision") != snapshot.revision)
        throw Error("stale_snapshot", "Query the visible elements again before performing this action");
    const auto element = snapshot.Resolve(id, action);
    if (params.Find("value") && action != "setValue") throw Error("invalid_params", "value is only valid for setValue", -32602);
    if (params.Find("number") && action != "scroll" && action != "setRangeValue" && action != "setScrollOffset")
        throw Error("invalid_params", "number is not used by this action", -32602);
    if (params.Find("targetId") && action != "drag") throw Error("invalid_params", "targetId is only valid for drag", -32602);
    const bool pointerAction = action == "hover" || action == "click" || action == "doubleClick" ||
        action == "contextMenu" || action == "drag" || action == "scroll";
    if (!pointerAction && (params.Find("durationMs") || params.Find("showCursor")))
        throw Error("invalid_params", "durationMs and showCursor require a pointer action", -32602);
    if (pointerAction)
    {
        pointer::Plan plan;
        plan.sourceId = id; plan.showCursor = Flag(params, "showCursor");
        if (params.Find("durationMs")) plan.durationMilliseconds = Integer(params, "durationMs", 100, 10000);
        if (action == "click") plan.action = pointer::Action::Click;
        else if (action == "doubleClick") plan.action = pointer::Action::DoubleClick;
        else if (action == "contextMenu") plan.action = pointer::Action::ContextMenu;
        else if (action == "drag")
        {
            plan.action = pointer::Action::Drag; plan.destinationId = Text(params, "targetId");
            (void)snapshot.Resolve(plan.destinationId, "drop");
        }
        else if (action == "scroll")
        {
            plan.action = pointer::Action::Scroll; plan.wheelDelta = Integer(params, "number", -12000, 12000);
            if (!plan.wheelDelta) throw Error("invalid_params", "scroll number cannot be zero", -32602);
        }
        std::string error;
        const auto destination = plan.destinationId;
        if (!applicationControlRuntime_->pointer.Start(std::move(plan), [this, destination, action](const std::string& target) {
            auto current = ApplicationControlElements();
            const auto& node = current.Resolve(target, target == destination ? "drop" : action);
            return pointer::Target{VisibleBounds(node), node.window};
        }, [this](const pointer::State& state) {
            if (state.status != "running" || !state.plan.showCursor) { applicationControlRuntime_->cursor.Hide(); return; }
            snowdesktop::demo_cursor::Pose pose;
            pose.x = static_cast<float>(state.current.point.x); pose.y = static_cast<float>(state.current.point.y);
            pose.pressed = state.current.buttons != 0;
            for (const auto& entry : state.trace)
                if (state.current.milliseconds - entry.milliseconds <= 180) pose.trail.push_back(entry.point);
            for (auto it = state.trace.rbegin(); it != state.trace.rend(); ++it)
                if (it->phase == "released") { pose.clickAgeMilliseconds = static_cast<float>(state.current.milliseconds - it->milliseconds); break; }
            if (!applicationControlRuntime_->cursor.Show(pose)) throw std::runtime_error("cursor_overlay_unavailable");
        }, error)) throw Error(error.empty() ? "pointer_unavailable" : error, "Controlled pointer could not start");
        return ApplicationControlPointerState(false);
    }
    if (HasActiveContextMenuSession()) throw Error("busy", "A context menu is active");
    if (action == "setValue" && element.backend == ElementBackend::NavigationSearch)
    {
        const auto text = InputText(params);
        if (!IsWindow(quickNavigationSearchEdit_)) throw Error("not_ready", "Navigation editor is unavailable");
        if (!SendMessageW(quickNavigationSearchEdit_, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str())))
            throw Error("action_rejected", "Navigation editor rejected the text");
    }
    else if (action == "setValue" && element.backend == ElementBackend::WidgetSearch)
    {
        const auto text = InputText(params);
        ScrollingItemWidget* target = nullptr;
        for (const auto& container : containers_)
            if (auto* widget = dynamic_cast<ScrollingItemWidget*>(container.get()); widget &&
                widget->GetWidgetData() && widget->GetWidgetData()->id == element.widgetId) { target = widget; break; }
        if (!target) throw Error("element_not_found", "Widget search editor is no longer present");
        target->SetSearchText(text); target->InvalidateSlots(); InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (element.backend == ElementBackend::Lua && widgetEngine_)
    {
        const auto index = FindWidgetIndexById(element.widgetId);
        if (index >= widgets_.size()) throw Error("element_not_found", "Lua widget is no longer present");
        LuaWidgetAccessibilityActionRequest request;
        request.widgetId = element.widgetId; request.nodeKey = element.nodeKey;
        if (action == "invoke") request.kind = LuaWidgetAccessibilityActionKind::Invoke;
        else if (action == "toggle") request.kind = LuaWidgetAccessibilityActionKind::Toggle;
        else if (action == "select") request.kind = LuaWidgetAccessibilityActionKind::Select;
        else if (action == "setValue") { request.kind = LuaWidgetAccessibilityActionKind::SetValue; request.textValue = InputText(params); }
        else if (action == "setRangeValue") { request.kind = LuaWidgetAccessibilityActionKind::SetRangeValue; request.numericValue = InputNumber(params); }
        else if (action == "setScrollOffset") { request.kind = LuaWidgetAccessibilityActionKind::SetScrollOffset; request.numericValue = InputNumber(params); }
        else if (action == "expand") request.kind = LuaWidgetAccessibilityActionKind::Expand;
        else if (action == "collapse") request.kind = LuaWidgetAccessibilityActionKind::Collapse;
        else if (action != "focus") throw Error("unsupported_action", "Unsupported Lua element action");
        SelectWidgetOnly(index); SetFocus(hwnd_);
        const bool applied = action == "focus" ? widgetEngine_->RuntimeSetAccessibilityFocus(element.widgetId, element.nodeKey)
            : widgetEngine_->RuntimePerformAccessibilityAction(request);
        if (!applied) throw Error("action_rejected", "Lua runtime rejected the element action");
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else throw Error("unsupported_action", "No semantic adapter exists for this element action");
    return Object({{"status", String("applied")}});
}
