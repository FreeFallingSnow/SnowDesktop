#include "widget_host_input_support.h"

#include "common/constants.h"
#include "common/l10n.h"
#include "common/utils.h"
#include "data/data_paths.h"
#include "diagnostics/performance_trace.h"
#include "layout/logical_slot_keyboard_rules.h"
#include "layout/logical_slot_picker_rules.h"
#include "layout/logical_slot_pointer_rules.h"
#include "settings/animation_settings.h"
#include "ui/input/private_pointer.h"
#include "ui/input/text_input_accessibility.h"
#include "ui/render/app_font.h"
#include "ui/render/native_control_geometry.h"
#include "widget/api/widget_api_registry.h"
#include "widget/permissions/widget_permission_broker.h"
#include "widget/preview/widget_preview_context.h"
#include "widget/runtime/widget_host_control_projection.h"
#include "widget/storage/widget_storage_runtime.h"
#include "widget/view/widget_draw_geometry.h"
#include "widget/view/widget_interaction_lua_value.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_scroll_rules.h"
#include "widget/view/widget_surface_host.h"
#include "widget/view/widget_view_text.h"
#include "widgets/widget_chrome_rules.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace text_input = snowdesktop::text_input;
using snowdesktop::widget_runtime::ActiveStorage;
using snowdesktop::widget_runtime::ApplyViewTextLocaleAndDirection;
using snowdesktop::widget_runtime::BuildHostInputDisplayText;
using snowdesktop::widget_runtime::HostInputDisplayText;
using snowdesktop::widget_runtime::IsDeclarativeInputNode;
using snowdesktop::widget_runtime::PushInteractionValue;
using snowdesktop::widget_runtime::SaveStorageFile;
using snowdesktop::widget_runtime::SetViewTextLayoutAlignment;

void WidgetEngine::DispatchInteractionAction(LuaWidget& widget,
    const std::string& targetKey, const char* eventName,
    int x, int y, int button, int delta, int clickCount,
    bool includeRetired, const char* source, int keyboardStepDirection,
    std::optional<float> requestedControlValue,
    std::optional<std::vector<std::string>> requestedSelectedKeys,
    std::string_view surface)
{
    if (targetKey.empty() || !eventName || !*eventName) return;
    snowdesktop::widget_runtime::InteractionAction action;
    std::string resolvedEventName(eventName);
    std::optional<bool> previousChecked;
    std::optional<bool> checked;
    std::optional<bool> previousIndeterminate;
    std::optional<bool> indeterminate;
    std::optional<float> previousControlValue;
    std::optional<float> controlValue;
    std::optional<std::string> previousSelection;
    std::optional<std::string> selection;
    std::optional<std::vector<std::string>> previousSelectedKeys;
    std::optional<std::vector<std::string>> selectedKeys;
    std::optional<bool> previousExpanded;
    std::optional<bool> expanded;
    bool trustedGesture = trustedGestureState_.Active();
    const std::string eventSource = source && *source ? source : "pointer";
    const std::string eventSurface =
        NormalizeWidgetSurface(surface);
    auto& interactionRegions =
        InteractionRegionsForSurface(widget, eventSurface);
    if (requestedSelectedKeys)
    {
        const auto* region = interactionRegions.Find(targetKey);
        const auto* actionPointer = interactionRegions.FindAction(
            targetKey, "change");
        if (!region || !actionPointer ||
            (region->controlKind != snowdesktop::widget_runtime::
                    InteractionControlKind::SelectionSingle &&
                region->controlKind != snowdesktop::widget_runtime::
                    InteractionControlKind::SelectionMultiple))
            return;
        action = *actionPointer;
        resolvedEventName = "change";
        previousSelectedKeys = region->currentSelectedKeys;
        selectedKeys = std::move(requestedSelectedKeys);
    }
    else if (includeRetired)
    {
        const auto* actionPointer =
            interactionRegions.FindTransitionAction(
                targetKey, eventName);
        if (!actionPointer) return;
        action = *actionPointer;
    }
    else
    {
        const auto resolved = requestedControlValue
            ? interactionRegions.ResolveRangeValue(
                targetKey, *requestedControlValue)
            : keyboardStepDirection == 0
                ? interactionRegions.ResolveAction(
                    targetKey, eventName, static_cast<float>(x),
                    static_cast<float>(y), button)
                : interactionRegions.ResolveKeyboardStep(
                    targetKey, keyboardStepDirection);
        if (!resolved) return;
        action = resolved->action;
        resolvedEventName = resolved->eventName;
        previousChecked = resolved->previousChecked;
        checked = resolved->checked;
        previousIndeterminate = resolved->previousIndeterminate;
        indeterminate = resolved->indeterminate;
        previousControlValue = resolved->previousControlValue;
        controlValue = resolved->controlValue;
        previousSelection = resolved->previousSelection;
        selection = resolved->selection;
        previousSelectedKeys = resolved->previousSelectedKeys;
        selectedKeys = resolved->selectedKeys;
        previousExpanded = resolved->previousExpanded;
        expanded = resolved->expanded;
    }
    // Only a host-owned pressed slider extends the initiating pointer gesture.
    // Merely moving over an element never grants task privileges.
    if (!trustedGesture && resolvedEventName == "change" && eventSource == "pointer" &&
        interactionRegions.PressedKey() == targetKey)
    {
        const auto* range = interactionRegions.Find(targetKey);
        trustedGesture = range && range->controlKind ==
            snowdesktop::widget_runtime::InteractionControlKind::Slider;
    }
    snowdesktop::widget_runtime::WidgetTrustedGestureScope actionGesture(
        trustedGestureState_, trustedGesture);
    const std::string popupOpener = trustedGesture && eventSurface == "desktop"
        ? targetKey : std::string{};
    WidgetSurfaceScope surfaceScope(d2dState_, eventSurface.c_str(), false, popupOpener);
    (void)InvokeLifecycleEvent(widget, "action",
        [&action, &targetKey, &resolvedEventName,
            previousChecked, checked, previousIndeterminate, indeterminate,
            previousControlValue, controlValue,
            previousSelection, selection,
            previousSelectedKeys, selectedKeys,
            previousExpanded, expanded,
            trustedGesture, &eventSource, &eventSurface, x, y, button,
            delta, clickCount](lua_State* eventState) {
            lua_pushlstring(eventState, action.id.data(), action.id.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, action.value);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, targetKey.data(), targetKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushlstring(eventState, resolvedEventName.data(),
                resolvedEventName.size());
            lua_setfield(eventState, -2, "action");
            if (previousChecked.has_value() && checked.has_value())
            {
                lua_pushboolean(eventState, *previousChecked ? 1 : 0);
                lua_setfield(eventState, -2, "previousChecked");
                lua_pushboolean(eventState, *checked ? 1 : 0);
                lua_setfield(eventState, -2, "checked");
            }
            if (previousIndeterminate.has_value() &&
                indeterminate.has_value())
            {
                lua_pushboolean(eventState,
                    *previousIndeterminate ? 1 : 0);
                lua_setfield(eventState, -2, "previousIndeterminate");
                lua_pushboolean(eventState, *indeterminate ? 1 : 0);
                lua_setfield(eventState, -2, "indeterminate");
            }
            if (previousControlValue.has_value() &&
                controlValue.has_value())
            {
                lua_pushnumber(eventState, *previousControlValue);
                lua_setfield(eventState, -2, "previousControlValue");
                lua_pushnumber(eventState, *controlValue);
                lua_setfield(eventState, -2, "controlValue");
            }
            if (previousSelection.has_value() && selection.has_value())
            {
                lua_pushlstring(eventState, previousSelection->data(),
                    previousSelection->size());
                lua_setfield(eventState, -2, "previousSelection");
                lua_pushlstring(eventState, selection->data(),
                    selection->size());
                lua_setfield(eventState, -2, "selection");
            }
            if (previousSelectedKeys.has_value() &&
                selectedKeys.has_value())
            {
                const auto pushKeys = [eventState](
                    const std::vector<std::string>& keys) {
                    lua_createtable(eventState,
                        static_cast<int>(keys.size()), 0);
                    for (std::size_t index = 0; index < keys.size(); ++index)
                    {
                        lua_pushlstring(eventState, keys[index].data(),
                            keys[index].size());
                        lua_rawseti(eventState, -2,
                            static_cast<lua_Integer>(index + 1));
                    }
                };
                pushKeys(*previousSelectedKeys);
                lua_setfield(eventState, -2, "previousSelectedKeys");
                pushKeys(*selectedKeys);
                lua_setfield(eventState, -2, "selectedKeys");
            }
            if (previousExpanded.has_value() && expanded.has_value())
            {
                lua_pushboolean(eventState, *previousExpanded ? 1 : 0);
                lua_setfield(eventState, -2, "previousExpanded");
                lua_pushboolean(eventState, *expanded ? 1 : 0);
                lua_setfield(eventState, -2, "expanded");
            }
            lua_pushboolean(eventState, trustedGesture ? 1 : 0);
            lua_setfield(eventState, -2, "trustedGesture");
            lua_pushlstring(eventState, eventSource.data(), eventSource.size());
            lua_setfield(eventState, -2, "source");
            lua_pushlstring(eventState,
                eventSurface.data(), eventSurface.size());
            lua_setfield(eventState, -2, "surface");
            lua_pushinteger(eventState, x);
            lua_setfield(eventState, -2, "x");
            lua_pushinteger(eventState, y);
            lua_setfield(eventState, -2, "y");
            lua_pushinteger(eventState, button);
            lua_setfield(eventState, -2, "button");
            lua_pushinteger(eventState, delta);
            lua_setfield(eventState, -2, "delta");
            lua_pushinteger(eventState, clickCount);
            lua_setfield(eventState, -2, "clickCount");
        });
}

void WidgetEngine::DispatchHostInputChange(const std::wstring& widgetId,
    const std::string& targetKey,
    const snowdesktop::widget_runtime::InteractionAction& action,
    const std::wstring& previousText, const std::wstring& text,
    bool numeric, float minimum, float maximum,
    bool committed, bool cancelled, const char* source)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || action.id.empty()) return;
    auto parseNumber = [minimum, maximum](const std::wstring& value)
        -> std::optional<double> {
        if (value.empty()) return std::nullopt;
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(value.c_str(), &end);
        if (!end || end != value.c_str() + value.size() ||
            !std::isfinite(parsed) || parsed < minimum || parsed > maximum)
            return std::nullopt;
        return parsed;
    };
    const auto previousNumber = numeric
        ? parseNumber(previousText) : std::optional<double>{};
    const auto proposedNumber = numeric
        ? parseNumber(text) : std::optional<double>{};
    const std::string previousUtf8 = WideToUtf8(previousText);
    const std::string proposedUtf8 = WideToUtf8(text);
    std::optional<snowdesktop::widget_runtime::ViewTextSelection>
        proposedSelection;
    if (focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == targetKey &&
        focusedHostInput_.controlledSelection)
    {
        proposedSelection = snowdesktop::widget_runtime::ViewTextSelection{
            snowdesktop::widget_runtime::Utf8ByteOffsetForHostTextOffset(
                focusedHostInput_.text,
                std::min(focusedHostInput_.selectionAnchor,
                    focusedHostInput_.cursor)),
            snowdesktop::widget_runtime::Utf8ByteOffsetForHostTextOffset(
                focusedHostInput_.text,
                std::max(focusedHostInput_.selectionAnchor,
                    focusedHostInput_.cursor)),
        };
    }
    const bool trustedGesture = trustedGestureState_.Active();
    const std::string eventSource = source && *source ? source : "keyboard";
    const std::string eventSurface = focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId &&
            focusedHostInput_.id == targetKey
        ? focusedHostInput_.surface
        : std::string(CurrentWidgetSurface(d2dState_));
    WidgetSurfaceScope surfaceScope(d2dState_, eventSurface.c_str());
    (void)InvokeLifecycleEvent(widgets_[index], "action",
        [&action, &targetKey, &previousUtf8, &proposedUtf8,
            numeric, previousNumber, proposedNumber, committed,
            cancelled, proposedSelection, trustedGesture,
            &eventSource, &eventSurface](lua_State* eventState) {
            lua_pushlstring(eventState, action.id.data(), action.id.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, action.value);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, targetKey.data(), targetKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushliteral(eventState, "change");
            lua_setfield(eventState, -2, "action");
            lua_pushlstring(eventState, previousUtf8.data(),
                previousUtf8.size());
            lua_setfield(eventState, -2, "previousText");
            lua_pushlstring(eventState, proposedUtf8.data(),
                proposedUtf8.size());
            lua_setfield(eventState, -2, "text");
            if (proposedSelection)
            {
                lua_createtable(eventState, 0, 2);
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(proposedSelection->start));
                lua_setfield(eventState, -2, "start");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(proposedSelection->finish));
                lua_setfield(eventState, -2, "finish");
                lua_setfield(eventState, -2, "selection");
            }
            if (numeric)
            {
                lua_pushboolean(eventState, proposedNumber.has_value());
                lua_setfield(eventState, -2, "numberValid");
                if (previousNumber)
                {
                    lua_pushnumber(eventState, *previousNumber);
                    lua_setfield(eventState, -2, "previousControlValue");
                }
                if (proposedNumber)
                {
                    lua_pushnumber(eventState, *proposedNumber);
                    lua_setfield(eventState, -2, "controlValue");
                }
            }
            lua_pushboolean(eventState, committed ? 1 : 0);
            lua_setfield(eventState, -2, "committed");
            lua_pushboolean(eventState, cancelled ? 1 : 0);
            lua_setfield(eventState, -2, "cancelled");
            lua_pushboolean(eventState, trustedGesture ? 1 : 0);
            lua_setfield(eventState, -2, "trustedGesture");
            lua_pushlstring(eventState, eventSource.data(),
                eventSource.size());
            lua_setfield(eventState, -2, "source");
            lua_pushlstring(eventState,
                eventSurface.data(), eventSurface.size());
            lua_setfield(eventState, -2, "surface");
        });
}

void WidgetEngine::DispatchHostInputSelectionChange(
    const std::wstring& widgetId, const std::string& targetKey,
    const snowdesktop::widget_runtime::InteractionAction& action,
    const std::wstring& text, size_t previousAnchor,
    size_t previousCursor, size_t anchor, size_t cursor,
    const char* source)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || action.id.empty()) return;
    const auto makeSelection = [&text](size_t left, size_t right) {
        return snowdesktop::widget_runtime::ViewTextSelection{
            snowdesktop::widget_runtime::Utf8ByteOffsetForHostTextOffset(
                text, std::min(left, right)),
            snowdesktop::widget_runtime::Utf8ByteOffsetForHostTextOffset(
                text, std::max(left, right)),
        };
    };
    const auto previous = makeSelection(previousAnchor, previousCursor);
    const auto proposed = makeSelection(anchor, cursor);
    if (previous == proposed) return;
    const bool trustedGesture = trustedGestureState_.Active();
    const std::string eventSource = source && *source ? source : "keyboard";
    const std::string eventSurface = focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId &&
            focusedHostInput_.id == targetKey
        ? focusedHostInput_.surface
        : std::string(CurrentWidgetSurface(d2dState_));
    WidgetSurfaceScope surfaceScope(d2dState_, eventSurface.c_str());
    (void)InvokeLifecycleEvent(widgets_[index], "action",
        [&action, &targetKey, previous, proposed, trustedGesture,
            &eventSource, &eventSurface](lua_State* eventState) {
            const auto pushSelection = [eventState](
                const snowdesktop::widget_runtime::ViewTextSelection& value) {
                lua_createtable(eventState, 0, 2);
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(value.start));
                lua_setfield(eventState, -2, "start");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(value.finish));
                lua_setfield(eventState, -2, "finish");
            };
            lua_pushlstring(eventState, action.id.data(), action.id.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, action.value);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, targetKey.data(), targetKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushliteral(eventState, "selectionChange");
            lua_setfield(eventState, -2, "action");
            pushSelection(previous);
            lua_setfield(eventState, -2, "previousSelection");
            pushSelection(proposed);
            lua_setfield(eventState, -2, "selection");
            lua_pushboolean(eventState, trustedGesture ? 1 : 0);
            lua_setfield(eventState, -2, "trustedGesture");
            lua_pushlstring(eventState, eventSource.data(),
                eventSource.size());
            lua_setfield(eventState, -2, "source");
            lua_pushlstring(eventState,
                eventSurface.data(), eventSurface.size());
            lua_setfield(eventState, -2, "surface");
        });
}

void WidgetEngine::DispatchHostInputAction(const std::wstring& widgetId,
    const std::string& targetKey,
    const snowdesktop::widget_runtime::InteractionAction& action,
    const char* eventName, const std::wstring& text,
    bool cancelled, const char* source)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || action.id.empty() || !eventName || !*eventName)
        return;
    const std::string utf8 = WideToUtf8(text);
    const bool trustedGesture = trustedGestureState_.Active();
    const std::string name(eventName);
    const std::string eventSource = source && *source ? source : "keyboard";
    const std::string eventSurface = focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId &&
            focusedHostInput_.id == targetKey
        ? focusedHostInput_.surface
        : std::string(CurrentWidgetSurface(d2dState_));
    WidgetSurfaceScope surfaceScope(d2dState_, eventSurface.c_str());
    (void)InvokeLifecycleEvent(widgets_[index], "action",
        [&action, &targetKey, &utf8, &name, cancelled,
            trustedGesture, &eventSource,
            &eventSurface](lua_State* eventState) {
            lua_pushlstring(eventState, action.id.data(), action.id.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, action.value);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, targetKey.data(), targetKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushlstring(eventState, name.data(), name.size());
            lua_setfield(eventState, -2, "action");
            lua_pushlstring(eventState, utf8.data(), utf8.size());
            lua_setfield(eventState, -2, "text");
            lua_pushboolean(eventState, cancelled ? 1 : 0);
            lua_setfield(eventState, -2, "cancelled");
            lua_pushboolean(eventState, trustedGesture ? 1 : 0);
            lua_setfield(eventState, -2, "trustedGesture");
            lua_pushlstring(eventState, eventSource.data(),
                eventSource.size());
            lua_setfield(eventState, -2, "source");
            lua_pushlstring(eventState,
                eventSurface.data(), eventSurface.size());
            lua_setfield(eventState, -2, "surface");
        });
}

void WidgetEngine::DispatchInteractionTransition(LuaWidget& widget,
    const snowdesktop::widget_runtime::InteractionHoverTransition& transition,
    int x, int y, std::string_view surface)
{
    if (!transition.leftKey.empty() &&
        transition.leftKey != transition.enteredKey)
    {
        DispatchInteractionAction(widget, transition.leftKey,
            "pointerLeave", x, y, 0, 0, 0, true, "pointer", 0,
            std::nullopt, std::nullopt, surface);
    }
    if (!transition.enteredKey.empty() &&
        transition.enteredKey != transition.leftKey)
    {
        DispatchInteractionAction(widget, transition.enteredKey,
            "pointerEnter", x, y, 0, 0, 0, false, "pointer", 0,
            std::nullopt, std::nullopt, surface);
    }
}

void WidgetEngine::UpdateInteractionHover(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface)
{
    for (auto& widget : widgets_)
    {
        auto& interactionRegions =
            InteractionRegionsForSurface(widget, surface);
        const auto transition = widget.widgetId == widgetId
            ? interactionRegions.UpdateHover(
                static_cast<float>(x), static_cast<float>(y))
            : interactionRegions.ClearHover();
        if (!transition.Changed()) continue;
        const bool transitionHasAction =
            interactionRegions.HasTransitionAction(transition);
        DispatchInteractionTransition(widget, transition, x, y, surface);
        if (HasDeclarativeViewForSurface(widget, surface) &&
            !transitionHasAction)
            QueueDeclarativeVisualFrame(widget, surface);
        RuntimeInvalidateHost(widget.widgetId);
    }
}

void WidgetEngine::ClearInteractionHover(std::string_view surface)
{
    for (auto& widget : widgets_)
    {
        const auto clearSurface = [&](std::string_view targetSurface) {
            auto& regions = InteractionRegionsForSurface(
                widget, targetSurface);
            const auto transition = regions.ClearHover();
            if (!transition.Changed()) return;
            const bool transitionHasAction =
                regions.HasTransitionAction(transition);
            DispatchInteractionTransition(
                widget, transition, 0, 0, targetSurface);
            if (HasDeclarativeViewForSurface(widget, targetSurface) &&
                !transitionHasAction)
                QueueDeclarativeVisualFrame(widget, targetSurface);
            RuntimeInvalidateHost(widget.widgetId);
        };
        if (surface.empty())
        {
            clearSurface("desktop");
            if (widget.panelActive)
                clearSurface(widget.panelSurface);
        }
        else
            clearSurface(surface);
    }
}

std::string WidgetEngine::InteractionCursorAt(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return {};
    return InteractionRegionsForSurface(widgets_[index], surface).CursorAt(
        static_cast<float>(x), static_cast<float>(y));
}
