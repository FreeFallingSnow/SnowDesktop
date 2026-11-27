#include "widget/runtime/widget_engine.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_surface_host.h"
#include "widget/view/widget_host_input_support.h"
#include "widget/preview/widget_preview_context.h"
#include "common/utils.h"
#include "widget/permissions/widget_permission_broker.h"
#include "widget/tasks/widget_filesystem_task_dispatch.h"
#include "diagnostics/performance_trace.h"
#include "ui/input/private_pointer.h"
#include "widget/runtime/widget_file_drop_delivery.h"
#include "widget/runtime/widget_host_control_projection.h"
#include "widget/tasks/widget_filesystem_drop.h"
#include "layout/logical_slot_pointer_rules.h"
#include "widget/view/widget_interaction_lua_value.h"

#include <algorithm>
#include <cmath>
#include <utility>

using snowdesktop::widget_runtime::kFilesystemReadPermission;
using snowdesktop::widget_runtime::PushInteractionValue;

void WidgetEngine::InvokeOpen(
    const std::wstring& widgetId, bool trustedGesture)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, trustedGesture);
    InvokeSimpleCallback(widgets_[idx], "onOpen");
}

void WidgetEngine::InvokeSelected(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    InvokeSimpleCallback(widgets_[idx], "onSelected");
}

void WidgetEngine::InvokeClick(const std::wstring& widgetId, int x, int y)
{
    InvokeMouseEvent(widgetId, "onClick", x, y, 1, 0);
}

bool WidgetEngine::HasFileDropTarget(const std::wstring& widgetId, int x, int y) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto& widget = widgets_[index];
    return widget.state && !widget.preview && filesystemHandleStore_ &&
        snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(
            widget.permissions, kFilesystemReadPermission) &&
        widget.interactionRegions.FileDropActionAt(
            static_cast<float>(x), static_cast<float>(y));
}

std::optional<WidgetEngine::FileDropTarget> WidgetEngine::CaptureFileDropTarget(
    const std::wstring& widgetId, int x, int y) const
{
    if (!HasFileDropTarget(widgetId, x, y)) return std::nullopt;
    const auto& widget = widgets_[FindWidget(widgetId)];
    FileDropTarget target;
    target.widgetId = widgetId;
    target.packageId = widget.packageId;
    target.runtimeToken = widget.runtimeToken;
    target.action = *widget.interactionRegions.FileDropActionAt(
        static_cast<float>(x), static_cast<float>(y), &target.targetKey);
    return target;
}

bool WidgetEngine::InvokeFileDrop(const std::wstring& widgetId, int x, int y,
    const std::vector<std::wstring>& paths)
{
    const auto target = CaptureFileDropTarget(widgetId, x, y);
    return target && InvokeFileDrop(*target, paths);
}

bool WidgetEngine::InvokeFileDrop(const FileDropTarget& target,
    const std::vector<std::wstring>& paths)
{
    using namespace snowdesktop::widget_runtime;
    const int index = FindWidget(target.widgetId);
    if (index < 0 || !filesystemHandleStore_) return false;
    auto& widget = widgets_[index];
    const WidgetFileDropDeliveryState current{widget.state != nullptr, widget.preview,
        widget.runtimeToken, widget.permissions, widget.interactionRegions};
    const WidgetFilesystemHandleOwner owner{WideToUtf8(target.widgetId), widget.packageId};
    return DeliverWidgetFileDrop(target, current, *filesystemHandleStore_, owner,
        trustedGestureState_, paths, [&](const auto& items) {
            return InvokeLifecycleEvent(widget, "action", [&](lua_State* state) {
                PushWidgetFileDropFields(state, target, items, PushInteractionValue, WideToUtf8);
            });
        });
}

void WidgetEngine::InvokeMouseEvent(const std::wstring& widgetId, const char* callbackName, int x, int y,
    int button, int delta)
{
    if (!callbackName || !*callbackName) return;
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
    lua_State* state = w.state;
    if (!state) return;
    const bool trustedGesture = snowdesktop::widget_runtime::
        IsTrustedWidgetGestureCallback(callbackName);
    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, trustedGesture);
    const std::string eventSurface =
            std::strstr(callbackName, "Dialog") != nullptr
                ? "dialog"
                : (std::strstr(callbackName, "Popover") != nullptr
                    ? "popover"
                    : (std::strstr(callbackName, "Panel") != nullptr
                        ? "panel" : "desktop"));
        WidgetSurfaceScope surfaceScope(
            d2dState_, eventSurface.c_str(),
            std::strcmp(callbackName, "onPanelOpened") == 0 ||
                std::strcmp(callbackName, "onDialogOpened") == 0 ||
                std::strcmp(callbackName, "onPopoverOpened") == 0);
        const char* kind = "pointer";
        const char* action = callbackName;
        if (std::strcmp(callbackName, "onClick") == 0 ||
            std::strcmp(callbackName, "onPanelClick") == 0 ||
            std::strcmp(callbackName, "onDialogClick") == 0 ||
            std::strcmp(callbackName, "onPopoverClick") == 0)
            action = "click";
        else if (std::strcmp(callbackName, "onDoubleClick") == 0 ||
            std::strcmp(callbackName, "onPanelDoubleClick") == 0 ||
            std::strcmp(callbackName, "onDialogDoubleClick") == 0 ||
            std::strcmp(callbackName, "onPopoverDoubleClick") == 0)
            action = "doubleClick";
        else if (std::strcmp(callbackName, "onMouseDown") == 0 ||
            std::strcmp(callbackName, "onPanelMouseDown") == 0 ||
            std::strcmp(callbackName, "onDialogMouseDown") == 0 ||
            std::strcmp(callbackName, "onPopoverMouseDown") == 0)
            action = "pointerDown";
        else if (std::strcmp(callbackName, "onMouseMove") == 0 ||
            std::strcmp(callbackName, "onPanelMouseMove") == 0 ||
            std::strcmp(callbackName, "onDialogMouseMove") == 0 ||
            std::strcmp(callbackName, "onPopoverMouseMove") == 0)
            action = "pointerMove";
        else if (std::strcmp(callbackName, "onMouseUp") == 0 ||
            std::strcmp(callbackName, "onPanelMouseUp") == 0 ||
            std::strcmp(callbackName, "onDialogMouseUp") == 0 ||
            std::strcmp(callbackName, "onPopoverMouseUp") == 0)
            action = "pointerUp";
        else if (std::strcmp(callbackName, "onWheel") == 0 ||
            std::strcmp(callbackName, "onPanelWheel") == 0 ||
            std::strcmp(callbackName, "onDialogWheel") == 0 ||
            std::strcmp(callbackName, "onPopoverWheel") == 0)
            action = "wheel";
        else if (std::strcmp(callbackName, "onPanelOpened") == 0)
        {
            kind = "panel";
            action = "opened";
        }
        else if (std::strcmp(callbackName, "onPanelClosed") == 0)
        {
            kind = "panel";
            action = "closed";
        }
        else if (std::strcmp(callbackName, "onDialogOpened") == 0)
        {
            kind = "dialog";
            action = "opened";
        }
        else if (std::strcmp(callbackName, "onDialogClosed") == 0)
        {
            kind = "dialog";
            action = "closed";
        }
        else if (std::strcmp(callbackName, "onPopoverOpened") == 0)
        {
            kind = "popover";
            action = "opened";
        }
        else if (std::strcmp(callbackName, "onPopoverClosed") == 0)
        {
            kind = "popover";
            action = "closed";
        }
        std::string targetKey;
        auto& interactionRegions = InteractionRegionsForSurface(
            w, eventSurface);
        const bool declarativeSurface =
            HasDeclarativeViewForSurface(w, eventSurface);
        if (std::strcmp(kind, "pointer") == 0)
        {
            bool declarativeVisualChanged = false;
            bool visualChangeRequiresLua = false;
            const auto transition = interactionRegions.UpdateHover(
                static_cast<float>(x), static_cast<float>(y));
            if (transition.Changed())
            {
                declarativeVisualChanged = true;
                visualChangeRequiresLua =
                    interactionRegions.HasTransitionAction(transition);
                DispatchInteractionTransition(w, transition, x, y,
                    eventSurface);
            }
            if (std::strcmp(action, "pointerDown") == 0)
            {
                const std::string previousPressed =
                    interactionRegions.PressedKey();
                targetKey = interactionRegions.PointerDown(
                    static_cast<float>(x), static_cast<float>(y), button).
                    targetKey;
                if (previousPressed != targetKey)
                {
                    declarativeVisualChanged = true;
                    visualChangeRequiresLua = visualChangeRequiresLua ||
                        interactionRegions.FindAction(
                            targetKey, "pointerDown") != nullptr;
                }
            }
            else if (std::strcmp(action, "pointerUp") == 0)
            {
                const std::string previousPressed =
                    interactionRegions.PressedKey();
                targetKey = interactionRegions.PointerUp(
                    static_cast<float>(x), static_cast<float>(y), button).
                    targetKey;
                if (!previousPressed.empty())
                {
                    declarativeVisualChanged = true;
                    visualChangeRequiresLua = visualChangeRequiresLua ||
                        interactionRegions.FindAction(
                            targetKey, "pointerUp") != nullptr;
                }
            }
            else if (std::strcmp(action, "click") == 0)
            {
                targetKey = interactionRegions.ConsumeClickTarget(
                    static_cast<float>(x), static_cast<float>(y));
            }
            else if (std::strcmp(action, "pointerMove") == 0)
            {
                targetKey = interactionRegions.PointerMoveTarget(
                    static_cast<float>(x), static_cast<float>(y));
            }
            else
            {
                targetKey = interactionRegions.TargetAt(
                    static_cast<float>(x), static_cast<float>(y));
            }
            DispatchInteractionAction(w, targetKey, action, x, y,
                button, delta,
                std::strcmp(action, "doubleClick") == 0 ? 2 :
                    (std::strcmp(action, "click") == 0 ? 1 : 0),
                false, "pointer", 0, std::nullopt, std::nullopt,
                eventSurface);
            if (declarativeVisualChanged)
            {
                if (declarativeSurface && !visualChangeRequiresLua)
                    QueueDeclarativeVisualFrame(w, eventSurface);
                RuntimeInvalidateHost(widgetId);
            }
        }
        if (std::strcmp(kind, "pointer") != 0 || !declarativeSurface)
        {
            (void)InvokeLifecycleEvent(w, kind,
                [action, eventSurface, x, y, button, delta, trustedGesture,
                    &targetKey](lua_State* eventState) {
                    lua_pushstring(eventState, action);
                    lua_setfield(eventState, -2, "action");
                    lua_pushlstring(eventState, eventSurface.data(),
                        eventSurface.size());
                    lua_setfield(eventState, -2, "surface");
                    lua_pushinteger(eventState, x);
                    lua_setfield(eventState, -2, "x");
                    lua_pushinteger(eventState, y);
                    lua_setfield(eventState, -2, "y");
                    lua_pushinteger(eventState, button);
                    lua_setfield(eventState, -2, "button");
                    lua_pushinteger(eventState, delta);
                    lua_setfield(eventState, -2, "delta");
                    if (!targetKey.empty())
                    {
                        lua_pushlstring(eventState, targetKey.data(),
                            targetKey.size());
                        lua_setfield(eventState, -2, "targetKey");
                    }
                    lua_pushboolean(eventState, trustedGesture ? 1 : 0);
                    lua_setfield(eventState, -2, "trustedGesture");
                });
        }
}

bool WidgetEngine::HasInteractionPointerCapture(
    const std::wstring& widgetId, std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 &&
        InteractionRegionsForSurface(widgets_[index], surface).
            HasPointerCapture();
}

bool WidgetEngine::OwnsSurfaceDrag(const std::wstring& widgetId, int x, int y, std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto& regions = InteractionRegionsForSurface(widgets_[index], surface);
    const auto* region = regions.Find(regions.TargetAt(static_cast<float>(x), static_cast<float>(y)));
    return region && region->enabled && !region->dragImage.empty();
}

bool WidgetEngine::IsSurfaceSliderAt(const std::wstring& widgetId, int x, int y, std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto& regions = InteractionRegionsForSurface(widgets_[index], surface);
    const auto* region = regions.Find(regions.TargetAt(static_cast<float>(x), static_cast<float>(y)));
    return region && region->controlKind == snowdesktop::widget_runtime::InteractionControlKind::Slider;
}

std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels>
WidgetEngine::TakeInteractionImageDrag(const std::wstring& widgetId, std::string_view surface)
{
    if (snowdesktop::private_pointer::Active()) return {};
    const int index = FindWidget(widgetId);
    if (index < 0 || !widgets_[index].valid || widgets_[index].preview) return {};
    auto& regions = InteractionRegionsForSurface(widgets_[index], surface);
    const auto token = regions.PressedDragImage();
    if (token.empty()) return {};
    auto image = ResolveTaskImage(widgetId, std::string(token));
    if (image)
    {
        regions.CancelPointerPress();
        RuntimeInvalidateHost(widgetId);
    }
    return image;
}

void WidgetEngine::CancelInteractionPointerPress(std::string_view surface)
{
    CancelHostInputPointerState(surface);
    for (auto& widget : widgets_)
    {
        bool changed = false;
        if (surface.empty())
        {
            changed = !widget.interactionRegions.PressedKey().empty() ||
                !widget.panelInteractionRegions.PressedKey().empty();
            widget.interactionRegions.CancelPointerPress();
            widget.panelInteractionRegions.CancelPointerPress();
        }
        else
        {
            auto& regions = InteractionRegionsForSurface(widget, surface);
            changed = !regions.PressedKey().empty();
            regions.CancelPointerPress();
        }
        if (changed) RuntimeInvalidateHost(widget.widgetId);
    }
}

void WidgetEngine::RuntimeBeginInlineTextEdit(const LuaInlineTextEditRequest& request)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    if (inlineTextEditCallback_)
        inlineTextEditCallback_(request);
}
