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

int WidgetEngine::RuntimeGetScrollOffset(const std::wstring& widgetId,
    const std::string& id, std::string_view surface) const
{
    int index = FindWidget(widgetId);
    if (index < 0) return 0;
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    const auto& offsets = ScrollOffsetsForSurface(
        widgets_[index], surface);
    const auto it = offsets.find(id);
    return it == offsets.end() ? 0 : it->second;
}

bool WidgetEngine::RuntimeHasScrollOffset(const std::wstring& widgetId,
    const std::string& id, std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    const auto& offsets = ScrollOffsetsForSurface(widgets_[index], surface);
    return offsets.contains(id);
}

bool WidgetEngine::RuntimeComputeVariableVirtualRange(
    const std::wstring& widgetId, const std::string& id,
    std::size_t itemCount, float estimatedItemSize, float mainGap,
    float viewportExtent, bool horizontal, std::uint64_t layoutRevision,
    std::size_t overscan, std::size_t initialScrollIndex,
    snowdesktop::widget_runtime::ViewVirtualRange& range,
    std::string& error, std::string_view surface) const
{
    using snowdesktop::widget_runtime::
        ComputeViewVariableVirtualItemScrollOffset;
    using snowdesktop::widget_runtime::ComputeViewVariableVirtualRange;
    error.clear();
    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "host context is unavailable";
        return false;
    }
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    const auto& widget = widgets_[index];
    std::vector<snowdesktop::widget_runtime::ViewVirtualItemMeasurement>
        measurements;
    const auto& states = VariableVirtualStatesForSurface(widget, surface);
    const auto state = states.find(id);
    if (state != states.end() && VariableVirtualStateMatches(
            state->second, itemCount, estimatedItemSize, mainGap,
            horizontal, layoutRevision))
        measurements = VariableVirtualMeasurements(state->second);

    const auto& offsets = ScrollOffsetsForSurface(widget, surface);
    const auto stored = offsets.find(id);
    float requestedOffset = stored == offsets.end()
        ? 0.0f : static_cast<float>(stored->second);
    if (stored == offsets.end() && initialScrollIndex > 0)
    {
        if (!ComputeViewVariableVirtualItemScrollOffset(itemCount,
                estimatedItemSize, mainGap, viewportExtent, 0.0f,
                initialScrollIndex, "nearest", measurements,
                requestedOffset, error))
            return false;
    }
    return ComputeViewVariableVirtualRange(itemCount,
        estimatedItemSize, mainGap, viewportExtent, requestedOffset,
        overscan, measurements, range, error);
}

void WidgetEngine::RuntimeSetScrollOffset(
    const std::wstring& widgetId, const std::string& id,
    int offset, std::string_view surface)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || id.empty()) return;
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    auto& widget = widgets_[index];
    int maximum = 0;
    for (auto it = widget.hostControls.rbegin();
        it != widget.hostControls.rend(); ++it)
    {
        if (it->id != id ||
            !HostControlBelongsToSurface(*it, surface)) continue;
        if (it->type != LuaWidget::HostControl::Type::Scroll &&
            !(it->type == LuaWidget::HostControl::Type::Input &&
                it->multiline))
            continue;
        maximum = it->horizontal
            ? std::max(0, it->contentWidth - it->viewportWidth)
            : std::max(0, it->contentHeight - it->viewportHeight);
        break;
    }
    ScrollOffsetsForSurface(widget, surface)[id] =
        std::clamp(offset, 0, maximum);
}

bool WidgetEngine::RuntimeScrollView(const std::wstring& widgetId,
    const std::string& id, int value, bool relative,
    int& offset, int& maximum, bool& changed, std::string& error,
    std::string_view surface)
{
    offset = 0;
    maximum = 0;
    changed = false;
    error.clear();
    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "hostUnavailable";
        return false;
    }
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    auto& widget = widgets_[index];
    const auto control = std::find_if(widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& candidate) {
            return candidate.type == LuaWidget::HostControl::Type::Scroll &&
                candidate.id == id &&
                HostControlBelongsToSurface(candidate, surface);
        });
    if (control == widget.hostControls.rend())
    {
        error = "scrollTargetNotFound";
        return false;
    }
    maximum = control->horizontal
        ? std::max(0, control->contentWidth - control->viewportWidth)
        : std::max(0, control->contentHeight - control->viewportHeight);
    auto& offsets = ScrollOffsetsForSurface(widget, surface);
    const int previous = std::clamp(offsets[id], 0, maximum);
    const std::int64_t requested = relative
        ? static_cast<std::int64_t>(previous) + value : value;
    offset = static_cast<int>(std::clamp<std::int64_t>(
        requested, 0, maximum));
    changed = offset != previous;
    offsets[id] = offset;
    if (changed) RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeScrollViewToIndex(
    const std::wstring& widgetId, const std::string& id,
    std::size_t itemIndex, std::string_view alignment,
    int& offset, int& maximum, bool& changed, std::string& error,
    std::string_view surface)
{
    const int widgetIndex = FindWidget(widgetId);
    if (widgetIndex < 0)
    {
        error = "hostUnavailable";
        return false;
    }
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    const auto& widget = widgets_[widgetIndex];
    const auto* tree = DeclarativeViewForSurface(widget, surface);
    const auto* node = tree ? FindViewNodeByKey(*tree, id) : nullptr;
    if (!node || (node->type != snowdesktop::widget_runtime::
            ViewNodeType::VirtualList &&
            node->type != snowdesktop::widget_runtime::
                ViewNodeType::VirtualGrid))
    {
        error = "virtualScrollTargetNotFound";
        return false;
    }
    const auto control = std::find_if(widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& candidate) {
            return candidate.type == LuaWidget::HostControl::Type::Scroll &&
                candidate.id == id &&
                HostControlBelongsToSurface(candidate, surface);
        });
    if (control == widget.hostControls.rend())
    {
        error = "scrollTargetNotFound";
        return false;
    }
    const int current = RuntimeGetScrollOffset(widgetId, id, surface);
    float requested = 0.0f;
    std::string rangeError;
    const bool horizontal = node->orientation ==
        snowdesktop::widget_runtime::ViewOrientation::Horizontal;
    const float viewportExtent = static_cast<float>(horizontal
        ? control->viewportWidth : control->viewportHeight);
    const float mainGap = horizontal
        ? node->columnGap.value_or(node->gap)
        : node->rowGap.value_or(node->gap);
    const bool positioned = node->estimatedItemSize
        ? snowdesktop::widget_runtime::
            ComputeViewVariableVirtualItemScrollOffset(
                node->itemCount, *node->estimatedItemSize,
                mainGap, viewportExtent,
                static_cast<float>(current), itemIndex, alignment,
                node->virtualMeasurements, requested, rangeError)
        : snowdesktop::widget_runtime::ComputeViewVirtualItemScrollOffset(
            node->itemCount, node->itemExtent,
            node->type == snowdesktop::widget_runtime::
                    ViewNodeType::VirtualGrid ? node->columns : 1,
            mainGap, viewportExtent,
            static_cast<float>(current), itemIndex, alignment,
            requested, rangeError);
    if (!positioned)
    {
        error = itemIndex == 0 || itemIndex > node->itemCount
            ? "indexOutOfRange" : "invalidVirtualCollection";
        return false;
    }
    return RuntimeScrollView(widgetId, id,
        static_cast<int>(std::lround(requested)), false,
        offset, maximum, changed, error, surface);
}

std::vector<LuaWidget::HostControl> WidgetEngine::GetScrollControls(
    const std::wstring& widgetId, std::string_view surface) const
{
    int index = FindWidget(widgetId);
    if (index < 0) return {};
    std::vector<LuaWidget::HostControl> results;
    for (const auto& ctrl : widgets_[index].hostControls)
    {
        if (HostControlBelongsToSurface(ctrl, surface) &&
            (ctrl.type == LuaWidget::HostControl::Type::Scroll ||
            (ctrl.type == LuaWidget::HostControl::Type::Input &&
                ctrl.multiline)))
            results.push_back(ctrl);
    }
    return results;
}

bool WidgetEngine::IsHostScrollbarDragging(
    const std::wstring& widgetId, std::string_view surface) const
{
    return hostScrollbarDrag_.active &&
        hostScrollbarDrag_.widgetId == widgetId &&
        hostScrollbarDrag_.surface == NormalizeWidgetSurface(surface);
}

bool WidgetEngine::OwnsDesktopWheel(const std::wstring& widgetId, int x, int y, int delta) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto& widget = widgets_[index];
    const POINT point{x, y};
    const auto canScroll = [&](const LuaWidget::HostControl& control) {
        const int maximum = control.horizontal && !control.selectPopup
            ? std::max(0, control.contentWidth - control.viewportWidth)
            : std::max(0, control.contentHeight - control.viewportHeight);
        return delta == 0 || snowdesktop::widget_scroll_rules::ApplyWheelDelta(
            RuntimeGetScrollOffset(widgetId, control.id, "desktop"), maximum, delta).moved;
    };
    // Match HandleHostUiPointer's popup-first routing, even if an enclosing
    // scroll container is still able to move behind the expanded list.
    for (auto it = widget.hostControls.rbegin(); it != widget.hostControls.rend(); ++it)
    {
        if (!it->enabled || !HostControlBelongsToSurface(*it, "desktop") || !HostControlContainsPoint(*it, point)) continue;
        if (it->selectPopup) return canScroll(*it);
    }
    std::string actionKey;
    const bool customAction = widget.interactionRegions.ActionAt(
        static_cast<float>(x), static_cast<float>(y), "wheel", &actionKey) != nullptr;
    bool actionBelongsToScrollControl = false;
    for (auto it = widget.hostControls.rbegin(); it != widget.hostControls.rend(); ++it)
    {
        if (!it->enabled || !HostControlBelongsToSurface(*it, "desktop") || !HostControlContainsPoint(*it, point)) continue;
        if (it->type != LuaWidget::HostControl::Type::Scroll &&
            !(it->type == LuaWidget::HostControl::Type::Input && it->multiline)) continue;
        if (canScroll(*it)) return true;
        actionBelongsToScrollControl = actionBelongsToScrollControl || actionKey == it->id;
    }
    // A custom region has no host-owned range to inspect. Keep its explicit
    // event ownership; known controls can hand off at their directional limit.
    return customAction && !actionBelongsToScrollControl;
}

bool WidgetEngine::HandleHostUiPointer(const std::wstring& widgetId, int x, int y,
    int delta, bool wheel, std::string_view surface)
{
    int index = FindWidget(widgetId);
    if (index < 0) return false;
    auto& widget = widgets_[index];
    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);
    WidgetSurfaceScope surfaceScope(d2dState_, normalizedSurface.c_str());
    POINT point{ x, y };
    for (auto it = widget.hostControls.rbegin();
        it != widget.hostControls.rend(); ++it)
    {
        if (!it->selectPopup || !it->enabled ||
            !HostControlBelongsToSurface(*it, normalizedSurface) ||
            !HostControlContainsPoint(*it, point)) continue;
        if (!wheel)
        {
            const float scale = CalculateWidgetCellScale(
                widget.layoutMetrics.gridCellWidth,
                widget.layoutMetrics.gridCellHeight);
            const int offset = RuntimeGetScrollOffset(
                widgetId, it->id, normalizedSurface);
            const auto geometry = snowdesktop::widget_scroll_rules::
                ResolveScrollbarAxisGeometry(it->rect.top,
                    it->rect.bottom, it->contentHeight,
                    it->viewportHeight, offset, scale);
            if (snowdesktop::widget_scroll_rules::ScrollbarThumbHit(
                    geometry, y, x, it->rect.right, scale))
            {
                hostScrollbarDrag_ = { true, widgetId, it->id,
                    normalizedSurface, false, y, offset };
                RuntimeInvalidateHost(widgetId);
                return true;
            }
            return false;
        }
        const int maximum = std::max(0,
            it->contentHeight - it->viewportHeight);
        int& offset = ScrollOffsetsForSurface(
            widget, normalizedSurface)[it->id];
        const auto result = snowdesktop::widget_scroll_rules::
            ApplyWheelDelta(offset, maximum, delta);
        if (result.moved)
        {
            offset = result.offset;
            RuntimeInvalidateHost(widgetId);
        }
        return true;
    }
    for (auto it = widget.hostControls.rbegin(); it != widget.hostControls.rend(); ++it)
    {
        if (!HostControlBelongsToSurface(*it, normalizedSurface) ||
            !it->enabled || !HostControlContainsPoint(*it, point)) continue;
        if (wheel &&
            (it->type == LuaWidget::HostControl::Type::Scroll ||
                (it->type ==
                    LuaWidget::HostControl::Type::Input &&
                    it->multiline)))
        {
            const int maximum = it->horizontal
                ? std::max(0, it->contentWidth - it->viewportWidth)
                : std::max(0, it->contentHeight - it->viewportHeight);
            int& offset = ScrollOffsetsForSurface(
                widget, normalizedSurface)[it->id];
            const auto result =
                snowdesktop::widget_scroll_rules::ApplyWheelDelta(
                    offset, maximum, delta);
            if (!result.moved)
                continue;
            offset = result.offset;
            if (focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId &&
                focusedHostInput_.id == it->id &&
                focusedHostInput_.surface == normalizedSurface)
            {
                focusedHostInput_.caretVisibility.PreserveManualScroll();
            }
            const std::string targetKey = it->id;
            {
                snowdesktop::widget_runtime::WidgetTrustedGestureScope
                    gestureScope(trustedGestureState_, true);
                DispatchInteractionAction(widget, targetKey, "wheel",
                    x, y, 0, delta, 0, false, "pointer", 0,
                    std::nullopt, std::nullopt, normalizedSurface);
            }
            if (result.reachedEnd)
                DispatchInteractionAction(widget, targetKey, "scrollEnd",
                    x, y, 0, delta, 0, false, "pointer", 0,
                    std::nullopt, std::nullopt, normalizedSurface);
            RuntimeInvalidateHost(widgetId);
            return true;
        }
        if (wheel) continue;
        if (it->type == LuaWidget::HostControl::Type::Scroll ||
            (it->type == LuaWidget::HostControl::Type::Input &&
                it->multiline))
        {
            const int contentExtent = it->horizontal
                ? it->contentWidth : it->contentHeight;
            const int visibleExtent = it->horizontal
                ? it->viewportWidth : it->viewportHeight;
            const int viewportStart = it->horizontal
                ? it->rect.left : it->rect.top;
            const int viewportEnd = it->horizontal
                ? it->rect.right : it->rect.bottom;
            const int viewportCrossEnd = it->horizontal
                ? it->rect.bottom : it->rect.right;
            const float scale = CalculateWidgetCellScale(
                widget.layoutMetrics.gridCellWidth,
                widget.layoutMetrics.gridCellHeight);
            const int offset = RuntimeGetScrollOffset(
                widgetId, it->id, normalizedSurface);
            const auto geometry = snowdesktop::widget_scroll_rules::
                ResolveScrollbarAxisGeometry(
                    viewportStart, viewportEnd, contentExtent,
                    visibleExtent, offset, scale);
            const int primary = it->horizontal ? x : y;
            const int cross = it->horizontal ? y : x;
            if (snowdesktop::widget_scroll_rules::ScrollbarThumbHit(
                    geometry, primary, cross, viewportCrossEnd, scale))
            {
                hostScrollbarDrag_ = {
                    true, widgetId, it->id, normalizedSurface,
                    it->horizontal, primary, offset
                };
                if (focusedHostInput_.active &&
                    focusedHostInput_.widgetId == widgetId &&
                    focusedHostInput_.id == it->id &&
                    focusedHostInput_.surface == normalizedSurface)
                {
                    focusedHostInput_.caretVisibility.PreserveManualScroll();
                }
                RuntimeInvalidateHost(widgetId);
                return true;
            }
        }
        if (it->type == LuaWidget::HostControl::Type::Input)
        {
            const std::string inputId = it->id;
            const bool alreadyFocused = focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId &&
                focusedHostInput_.id == inputId &&
                focusedHostInput_.surface == normalizedSurface;
            if (RuntimeFocusHostInput(widgetId, inputId) &&
                focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId &&
                focusedHostInput_.id == inputId)
            {
                // A focus callback can repaint and replace the host-control list.
                const int currentIndex = FindWidget(widgetId);
                if (currentIndex < 0) return true;
                const auto& currentWidget = widgets_[currentIndex];
                const auto control = std::find_if(currentWidget.hostControls.rbegin(),
                    currentWidget.hostControls.rend(), [&](const auto& candidate) {
                        return candidate.type == LuaWidget::HostControl::Type::Input &&
                            candidate.id == inputId &&
                            HostControlBelongsToSurface(candidate, normalizedSurface);
                    });
                if (control == currentWidget.hostControls.rend()) return true;
                focusedHostInput_.pointerSelectionStartCursor =
                    focusedHostInput_.cursor;
                focusedHostInput_.pointerSelectionStartAnchor =
                    focusedHostInput_.selectionAnchor;
                const auto start = std::min(focusedHostInput_.cursor, focusedHostInput_.selectionAnchor);
                const auto end = std::max(focusedHostInput_.cursor, focusedHostInput_.selectionAnchor);
                std::optional<size_t> hitCharacter;
                if (alreadyFocused && start != end && !focusedHostInput_.composing &&
                    !snowdesktop::private_pointer::Active() &&
                    !(snowdesktop::private_pointer::KeyState(VK_SHIFT) & 0x8000))
                    (void)ResolveHostInputTextDropPosition(currentWidget, *control,
                        {x, y}, focusedHostInput_.text, nullptr, &hitCharacter);
                focusedHostInput_.selectionDragArmed = hitCharacter &&
                    *hitCharacter >= start && *hitCharacter < end;
                if (!focusedHostInput_.selectionDragArmed)
                {
                    focusedHostInput_.cursor = HitTestHostInputPosition(*control, widgetId, x, y);
                    focusedHostInput_.selectionAnchor = focusedHostInput_.cursor;
                }
                focusedHostInput_.pointerSelecting = true;
                focusedHostInput_.caretVisibility.Request();
                RuntimeInvalidateHost(widgetId);
            }
            return true;
        }
        if (it->type == LuaWidget::HostControl::Type::Button ||
            it->type == LuaWidget::HostControl::Type::Toggle)
        {
            lua_State* state = widget.state;
            if (!state) return false;
            const int widgetRef = widget.ref;
            const RECT bounds = BoundsForSurface(
                widget, normalizedSurface);
            const std::string controlId = it->id;
            const bool controlValue =
                it->type == LuaWidget::HostControl::Type::Toggle
                    ? !it->value : true;
            snowdesktop::widget_runtime::WidgetTrustedGestureScope
                gestureScope(trustedGestureState_, true);
            WidgetExecutionContextGuard contextGuard(
                d2dState_, widgetId);
            snowdesktop::lua_runtime::StackGuard stackGuard(state);
            SetWidgetRectContext(d2dState_, bounds);
            lua_rawgeti(state, LUA_REGISTRYINDEX, widgetRef);
            if (lua_istable(state, -1))
            {
                lua_getfield(state, -1, "onUiAction");
                if (lua_isfunction(state, -1))
                {
                    lua_pushstring(state, controlId.c_str());
                    lua_pushboolean(state, controlValue);
                    if (snowdesktop::lua_runtime::ProtectedCall(state, 2, 0) != LUA_OK)
                    {
                        const char* error = lua_tostring(state, -1);
                        RuntimeRecordError(widgetId, error ? error : "(onUiAction error)");
                        lua_pop(state, 1);
                    }
                }
                else
                    lua_pop(state, 1);
            }
            lua_pop(state, 1);
            RuntimeInvalidateHost(widgetId);
            return true;
        }
    }
    if (!wheel)
    {
        if (IsPanelSurface(normalizedSurface))
        {
            const std::string pointerTarget =
                widget.panelInteractionRegions.TargetAt(
                    static_cast<float>(x), static_cast<float>(y));
            widget.panelViewKeyboardFocusKey =
                widget.panelInteractionRegions.IsKeyboardFocusable(
                    pointerTarget)
                ? pointerTarget : std::string{};
            widget.panelViewFocusCueVisible = false;
        }
        else
            BeginHostLogicalSlotPointer(widget, x, y);
    }
    return false;
}
