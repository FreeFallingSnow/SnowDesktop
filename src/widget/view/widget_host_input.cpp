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
#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_host_control_projection.h"
#include "widget/storage/widget_storage_runtime.h"
#include "widget/view/widget_draw_geometry.h"
#include "widget/view/widget_interaction_lua_value.h"
#include "widget/view/widget_interaction_region.h"
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
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace text_input = snowdesktop::text_input;
using snowdesktop::widget_runtime::ActiveStorage;
using snowdesktop::widget_runtime::ApplyViewTextLocaleAndDirection;
using snowdesktop::widget_runtime::BuildHostInputDisplayText;
using snowdesktop::widget_runtime::HostInputDisplayText;
using snowdesktop::widget_runtime::IsDeclarativeInputNode;
using snowdesktop::widget_runtime::PushInteractionValue;
using snowdesktop::widget_runtime::SaveStorageFile;
using snowdesktop::widget_runtime::SetViewTextLayoutAlignment;

bool HasDeclarativeViewForSurface(
    const LuaWidget& widget, std::string_view surface) noexcept
{
    return IsPanelSurface(surface)
        ? widget.panelViewTree.has_value() : widget.viewTree.has_value();
}

const snowdesktop::widget_runtime::ViewNode*
DeclarativeViewForSurface(
    const LuaWidget& widget, std::string_view surface) noexcept
{
    const auto& tree = IsPanelSurface(surface)
        ? widget.panelViewTree : widget.viewTree;
    return tree ? &*tree : nullptr;
}

const snowdesktop::widget_runtime::ViewNode* FindViewNodeByKey(
    const snowdesktop::widget_runtime::ViewNode& node,
    std::string_view key) noexcept
{
    if (node.key == key) return &node;
    for (const auto& child : node.children)
    {
        if (const auto* found = FindViewNodeByKey(child, key))
            return found;
    }
    return nullptr;
}

void QueueDeclarativeVisualFrame(
    LuaWidget& widget, std::string_view surface) noexcept
{
    if (IsPanelSurface(surface))
        widget.panelViewTransitionFramePending = true;
    else
        widget.viewTransitionFramePending = true;
}

RECT BoundsForSurface(
    const LuaWidget& widget, std::string_view surface) noexcept
{
    return IsPanelSurface(surface) ? widget.panelBounds : widget.lastBounds;
}

std::string ViewVirtualKeyName(WPARAM key)
{
    if (key >= '0' && key <= '9')
        return std::string(1, static_cast<char>(key));
    if (key >= 'A' && key <= 'Z')
        return std::string(1, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F24)
        return "F" + std::to_string(key - VK_F1 + 1);
    switch (key)
    {
    case VK_BACK: return "Backspace";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Control";
    case VK_MENU: return "Alt";
    case VK_PAUSE: return "Pause";
    case VK_CAPITAL: return "CapsLock";
    case VK_ESCAPE: return "Escape";
    case VK_SPACE: return "Space";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_END: return "End";
    case VK_HOME: return "Home";
    case VK_LEFT: return "ArrowLeft";
    case VK_UP: return "ArrowUp";
    case VK_RIGHT: return "ArrowRight";
    case VK_DOWN: return "ArrowDown";
    case VK_SNAPSHOT: return "PrintScreen";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_LWIN: return "MetaLeft";
    case VK_RWIN: return "MetaRight";
    case VK_APPS: return "ContextMenu";
    case VK_NUMLOCK: return "NumLock";
    case VK_SCROLL: return "ScrollLock";
    default: return "Unidentified";
    }
}

void WidgetEngine::RuntimeRegisterHostControl(const std::wstring& widgetId,
    LuaWidget::HostControl control)
{
    int index = FindWidget(widgetId);
    if (index < 0) return;
    const std::string surface(CurrentWidgetSurface(d2dState_));
    control.surface = surface;
    const auto surfaceControlCount = std::count_if(
        widgets_[index].hostControls.begin(),
        widgets_[index].hostControls.end(), [&](const auto& candidate) {
            return HostControlBelongsToSurface(candidate, surface);
        });
    if (surfaceControlCount >= 128) return;
    if (control.type == LuaWidget::HostControl::Type::Scroll ||
        (control.type == LuaWidget::HostControl::Type::Input &&
            control.multiline))
    {
        const int maximum = control.horizontal
            ? std::max(0, control.contentWidth - control.viewportWidth)
            : std::max(0, control.contentHeight - control.viewportHeight);
        int& offset = ScrollOffsetsForSurface(
            widgets_[index], surface).try_emplace(
                control.id, control.initialScrollOffset).first->second;
        offset = std::clamp(offset, 0, maximum);
    }
    if (control.type == LuaWidget::HostControl::Type::Input &&
        focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == control.id &&
        focusedHostInput_.surface == surface)
    {
        focusedHostInput_.controlled = control.controlled;
        focusedHostInput_.readOnly = control.readOnly;
        focusedHostInput_.numeric = control.numeric;
        focusedHostInput_.changeAction = control.changeAction;
        focusedHostInput_.selectionChangeAction =
            control.selectionChangeAction;
        focusedHostInput_.focusAction = control.focusAction;
        focusedHostInput_.blurAction = control.blurAction;
        focusedHostInput_.submitAction = control.submitAction;
        focusedHostInput_.liveUpdate = control.liveUpdate;
        focusedHostInput_.multiline = control.multiline;
        focusedHostInput_.maximumUtf8Bytes =
            control.maximumUtf8Bytes;
        focusedHostInput_.minimum = control.minimum;
        focusedHostInput_.maximum = control.maximum;
        focusedHostInput_.step = control.step;
        const std::wstring modelText=Utf8ToWide(control.controlled?control.controlledText:
            RuntimeGetStorageValue(widgetId,control.storageKey));
        if(modelText!=focusedHostInput_.modelText&&(control.controlled||control.liveUpdate))
        {
            focusedHostInput_.modelText=modelText;
            if(modelText==focusedHostInput_.text)focusedHostInput_.deferredModelText.reset();
            else if(focusedHostInput_.composing)focusedHostInput_.deferredModelText=modelText;
            else
            {
                focusedHostInput_.text=modelText;focusedHostInput_.originalText=modelText;
                focusedHostInput_.cursor=text_input::SnapBoundary(modelText,focusedHostInput_.cursor);
                focusedHostInput_.selectionAnchor=text_input::SnapBoundary(modelText,focusedHostInput_.selectionAnchor);
                focusedHostInput_.history.Clear();focusedHostInput_.caretVisibility.Request();
            }
        }
        const bool selectionChanged =
            focusedHostInput_.controlledSelection != control.selection;
        focusedHostInput_.controlledSelection = control.selection;
        if (selectionChanged && control.selection && focusedHostInput_.composing)
            focusedHostInput_.deferredSelection=true;
        else if (selectionChanged && control.selection)
        {
            const auto anchor = snowdesktop::widget_runtime::
                HostTextOffsetFromUtf8ByteOffset(
                    focusedHostInput_.text, control.selection->start);
            const auto cursor = snowdesktop::widget_runtime::
                HostTextOffsetFromUtf8ByteOffset(
                    focusedHostInput_.text, control.selection->finish);
            if (anchor && cursor)
            {
                focusedHostInput_.selectionAnchor = *anchor;
                focusedHostInput_.cursor = *cursor;
                focusedHostInput_.compositionText.clear();
                focusedHostInput_.compositionCursor = 0;
                focusedHostInput_.caretVisibility.Request();
            }
        }
    }
    widgets_[index].hostControls.push_back(std::move(control));
}

bool HostControlContainsPoint(
    const LuaWidget::HostControl& control, POINT point) noexcept
{
    return PtInRect(&control.rect, point) != FALSE &&
        (!control.clipRect ||
            PtInRect(&*control.clipRect, point) != FALSE);
}

bool WidgetEngine::RuntimeRegisterV2HostControl(
    const std::wstring& widgetId, LuaWidget::HostControl control,
    std::string& error)
{
    error.clear();
    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "widget instance is unavailable";
        return false;
    }
    auto& widget = widgets_[index];
    const std::string surface(CurrentWidgetSurface(d2dState_));
    control.surface = surface;
    auto& interactionRegions =
        InteractionRegionsForSurface(widget, surface);
    if (!interactionRegions.FrameOpen() && !widget.panelFrameOpen)
    {
        error = "host text controls may only be submitted during render or panel";
        return false;
    }
    if (std::count_if(widget.hostControls.begin(),
            widget.hostControls.end(), [&](const auto& candidate) {
                return HostControlBelongsToSurface(candidate, surface);
            }) >= 128)
    {
        error = "host control limit exceeded (128)";
        return false;
    }
    if (std::any_of(widget.hostControls.begin(),
            widget.hostControls.end(), [&](const auto& candidate) {
                return HostControlBelongsToSurface(candidate, surface) &&
                    candidate.id == control.id;
            }))
    {
        error = "duplicate host control key: " + control.id;
        return false;
    }
    if (control.type == LuaWidget::HostControl::Type::Input &&
        interactionRegions.FrameOpen())
    {
        snowdesktop::widget_runtime::InteractionRegion region;
        region.key = control.id;
        region.shape.type =
            snowdesktop::widget_runtime::InteractionShapeType::Rect;
        region.shape.x = static_cast<float>(control.rect.left);
        region.shape.y = static_cast<float>(control.rect.top);
        region.shape.width = static_cast<float>(
            control.rect.right - control.rect.left);
        region.shape.height = static_cast<float>(
            control.rect.bottom - control.rect.top);
        if (control.clipRect)
        {
            region.clip = snowdesktop::widget_runtime::InteractionClipRect{
                static_cast<float>(control.clipRect->left),
                static_cast<float>(control.clipRect->top),
                static_cast<float>(control.clipRect->right -
                    control.clipRect->left),
                static_cast<float>(control.clipRect->bottom -
                    control.clipRect->top) };
        }
        region.cursor = "text";
        region.accessibilityRole = "textbox";
        region.accessibilityLabel = control.placeholder.empty()
            ? control.id : control.placeholder;
        region.enabled = control.enabled;
        if (!interactionRegions.Submit(std::move(region), error))
            return false;
    }
    RuntimeRegisterHostControl(widgetId, std::move(control));
    return true;
}

bool WidgetEngine::RuntimeFocusHostInput(const std::wstring& widgetId,
    const std::string& id, const char* source)
{
    int index = FindWidget(widgetId);
    if (index < 0 || id.empty()) return false;
    const std::string surface(CurrentWidgetSurface(d2dState_));
    const auto findInput = [&](int widgetIndex) -> const LuaWidget::HostControl* {
        if (widgetIndex < 0) return nullptr;
        const auto& controls = widgets_[widgetIndex].hostControls;
        const auto found = std::find_if(controls.rbegin(), controls.rend(), [&](const auto& control) {
            return control.type == LuaWidget::HostControl::Type::Input &&
                control.id == id && HostControlBelongsToSurface(control, surface);
        });
        if (found == controls.rend() || !found->enabled || !found->focusable ||
            (!found->controlled && found->storageKey.empty())) return nullptr;
        return &*found;
    };
    const auto* found = findInput(index);
    if (!found) return false;

    if (focusedHostInput_.active && focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == id &&
        focusedHostInput_.surface == surface)
    {
        ViewFocusForSurface(widgets_[index], surface) = id;
        ViewFocusCueForSurface(widgets_[index], surface) =
            snowdesktop::widget_runtime::ShouldShowInteractionFocusCue(
                source ? source : "pointer", true);
        if (!IsPanelSurface(surface))
            widgets_[index].logicalSlotFocus.reset();
        if (!found->controlled && found->liveUpdate)
        {
            const std::wstring storedText = Utf8ToWide(
                RuntimeGetStorageValue(widgetId, found->storageKey));
            if (storedText != focusedHostInput_.text)
            {
                focusedHostInput_.text = storedText;
                focusedHostInput_.history.Clear();
                focusedHostInput_.originalText = storedText;
                focusedHostInput_.cursor = storedText.size();
                focusedHostInput_.selectionAnchor =
                    found->selectAll ? 0 : storedText.size();
                focusedHostInput_.compositionText.clear();
                focusedHostInput_.compositionCursor = 0;
                focusedHostInput_.caretVisibility.Request();
            }
        }
        focusedHostInput_.multiline = found->multiline;
        focusedHostInput_.controlled = found->controlled;
        focusedHostInput_.readOnly = found->readOnly;
        focusedHostInput_.numeric = found->numeric;
        focusedHostInput_.changeAction = found->changeAction;
        focusedHostInput_.selectionChangeAction =
            found->selectionChangeAction;
        focusedHostInput_.focusAction = found->focusAction;
        focusedHostInput_.blurAction = found->blurAction;
        focusedHostInput_.submitAction = found->submitAction;
        focusedHostInput_.minimum = found->minimum;
        focusedHostInput_.maximum = found->maximum;
        focusedHostInput_.step = found->step;
        focusedHostInput_.maximumUtf8Bytes =
            found->maximumUtf8Bytes;
        const bool selectionChanged =
            focusedHostInput_.controlledSelection != found->selection;
        focusedHostInput_.controlledSelection = found->selection;
        if (selectionChanged && found->selection)
        {
            const auto anchor = snowdesktop::widget_runtime::
                HostTextOffsetFromUtf8ByteOffset(
                    focusedHostInput_.text, found->selection->start);
            const auto cursor = snowdesktop::widget_runtime::
                HostTextOffsetFromUtf8ByteOffset(
                    focusedHostInput_.text, found->selection->finish);
            if (anchor && cursor)
            {
                focusedHostInput_.selectionAnchor = *anchor;
                focusedHostInput_.cursor = *cursor;
                focusedHostInput_.caretVisibility.Request();
            }
        }
        focusedHostInput_.caretVisibility.Request();
        if (hostInputFocusCallback_)
            hostInputFocusCallback_();
        return true;
    }

    BlurHostInput(false);
    index = FindWidget(widgetId);
    found = findInput(index);
    if (!found) return false;
    ViewFocusForSurface(widgets_[index], surface) = id;
    ViewFocusCueForSurface(widgets_[index], surface) =
        snowdesktop::widget_runtime::ShouldShowInteractionFocusCue(
            source ? source : "pointer", true);
    if (!IsPanelSurface(surface))
        widgets_[index].logicalSlotFocus.reset();
    focusedHostInput_.active = true;
    focusedHostInput_.widgetId = widgetId;
    focusedHostInput_.id = id;
    focusedHostInput_.surface = surface;
    focusedHostInput_.storageKey = found->storageKey;
    focusedHostInput_.text = Utf8ToWide(found->controlled
        ? found->controlledText
        : RuntimeGetStorageValue(widgetId, found->storageKey));
    focusedHostInput_.originalText = focusedHostInput_.text;
    focusedHostInput_.modelText = focusedHostInput_.text;
    focusedHostInput_.cursor = focusedHostInput_.text.size();
    focusedHostInput_.selectionAnchor = found->selectAll
        ? 0 : focusedHostInput_.cursor;
    focusedHostInput_.controlledSelection = found->selection;
    if (found->selection)
    {
        const auto anchor = snowdesktop::widget_runtime::
            HostTextOffsetFromUtf8ByteOffset(
                focusedHostInput_.text, found->selection->start);
        const auto cursor = snowdesktop::widget_runtime::
            HostTextOffsetFromUtf8ByteOffset(
                focusedHostInput_.text, found->selection->finish);
        if (anchor && cursor)
        {
            focusedHostInput_.selectionAnchor = *anchor;
            focusedHostInput_.cursor = *cursor;
        }
    }
    focusedHostInput_.liveUpdate = found->liveUpdate;
    focusedHostInput_.multiline = found->multiline;
    focusedHostInput_.controlled = found->controlled;
    focusedHostInput_.readOnly = found->readOnly;
    focusedHostInput_.numeric = found->numeric;
    focusedHostInput_.changeAction = found->changeAction;
    focusedHostInput_.selectionChangeAction =
        found->selectionChangeAction;
    focusedHostInput_.focusAction = found->focusAction;
    focusedHostInput_.blurAction = found->blurAction;
    focusedHostInput_.submitAction = found->submitAction;
    focusedHostInput_.minimum = found->minimum;
    focusedHostInput_.maximum = found->maximum;
    focusedHostInput_.step = found->step;
    focusedHostInput_.maximumUtf8Bytes =
        found->maximumUtf8Bytes;
    focusedHostInput_.caretVisibility.Request();
    DispatchHostInputAction(widgetId, id,
        focusedHostInput_.focusAction, "focus",
        focusedHostInput_.text, false,
        source && *source ? source : "pointer");
    if (hostInputFocusCallback_)
        hostInputFocusCallback_();
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeGetFocusedHostInput(const std::wstring& widgetId,
    const std::string& id, std::wstring& text, size_t& cursor,
    size_t& selectionAnchor, std::wstring& compositionText,
    size_t& compositionCursor) const
{
    if (!focusedHostInput_.active || focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.id != id ||
        focusedHostInput_.surface != CurrentWidgetSurface(d2dState_))
        return false;
    text = focusedHostInput_.text;
    cursor = focusedHostInput_.cursor;
    selectionAnchor = focusedHostInput_.selectionAnchor;
    compositionText = focusedHostInput_.compositionText;
    compositionCursor = focusedHostInput_.compositionCursor;
    return true;
}

size_t WidgetEngine::HitTestHostInputPosition(
    const LuaWidget::HostControl& control,
    const std::wstring& widgetId, int x, int y) const
{
    if (!d2dState_ || control.type !=
        LuaWidget::HostControl::Type::Input)
        return 0;
    const std::wstring& text = focusedHostInput_.text;
    const float width = static_cast<float>(
        control.rect.right - control.rect.left);
    const float height = static_cast<float>(
        control.rect.bottom - control.rect.top);
    ComPtr<IDWriteTextLayout> layout;
    float localX = static_cast<float>(
        x - control.rect.left) - control.padding.left;
    float localY = static_cast<float>(
        y - control.rect.top) - control.padding.top;
    if (control.multiline)
    {
        const float scrollbarReserve = 8.0f;
        const float innerWidth = std::max(1.0f,
            width - control.padding.left - control.padding.right -
                scrollbarReserve);
        layout = CreateHostMultilineTextLayout(d2dState_,
            text, control.fontSize, innerWidth);
        localY += RuntimeGetScrollOffset(
            widgetId, control.id, control.surface);
    }
    else
    {
        const float innerWidth = std::max(1.0f,
            width - control.padding.left - control.padding.right);
        layout = CreateHostSingleLineTextLayout(d2dState_,
            text, control.fontSize, innerWidth,
            std::max(1.0f, height - control.padding.top -
                control.padding.bottom));
    }
    if (!layout)
        return std::min(focusedHostInput_.cursor,
            focusedHostInput_.text.size());

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestPoint(localX, localY,
        &trailing, &inside, &metrics)))
        return std::min(focusedHostInput_.cursor,
            focusedHostInput_.text.size());
    const size_t position =
        static_cast<size_t>(metrics.textPosition) +
        (trailing
            ? static_cast<size_t>(metrics.length) : 0);
    return std::min(position, text.size());
}

bool WidgetEngine::IsFocusedHostInputAt(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface) const
{
    if (!focusedHostInput_.active ||
        focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.surface != surface)
        return false;
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const POINT point{ x, y };
    for (const auto& control : widgets_[index].hostControls)
        if (control.selectPopup && control.enabled &&
            HostControlBelongsToSurface(control, surface) &&
            HostControlContainsPoint(control, point))
            return false;
    for (auto it = widgets_[index].hostControls.rbegin();
        it != widgets_[index].hostControls.rend(); ++it)
    {
        if (it->type == LuaWidget::HostControl::Type::Input &&
            it->id == focusedHostInput_.id &&
            HostControlBelongsToSurface(*it, surface))
            return HostControlContainsPoint(*it, point);
    }
    return false;
}

namespace
{
struct HostLogicalSlotKeyboardItem
{
    std::string slotId;
    std::string itemId;
    snowdesktop::widget_runtime::LogicalSlotKind kind =
        snowdesktop::widget_runtime::LogicalSlotKind::Binding;
    std::size_t modelIndex = 0;
    std::size_t surfaceIndex = 0;
    std::size_t itemCount = 0;
    bool allowClear = true;
    RECT bounds{};
};

std::vector<HostLogicalSlotKeyboardItem> CollectHostLogicalSlotKeyboardItems(
    WidgetEngine& engine, const LuaWidget& widget)
{
    std::vector<HostLogicalSlotKeyboardItem> result;
    for (const auto& snapshot : widget.logicalSlots.Snapshots())
    {
        const auto surface = engine.RuntimeLogicalSlotSurface(
            widget.widgetId, snapshot.id);
        if (!surface) continue;
        for (std::size_t surfaceIndex = 0;
            surfaceIndex < surface->items.size(); ++surfaceIndex)
        {
            const auto& region = surface->items[surfaceIndex];
            const auto modelItem = std::find_if(snapshot.items.begin(),
                snapshot.items.end(), [&](const auto& candidate) {
                    return candidate.id == region.itemId;
                });
            if (modelItem == snapshot.items.end()) continue;
            result.push_back({ snapshot.id, region.itemId, snapshot.kind,
                static_cast<std::size_t>(std::distance(
                    snapshot.items.begin(), modelItem)),
                surfaceIndex, snapshot.items.size(), surface->allowClear,
                region.bounds });
        }
    }
    std::stable_sort(result.begin(), result.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.bounds.top, left.bounds.left,
                       left.bounds.bottom, left.bounds.right,
                       left.slotId, left.itemId) <
                std::tie(right.bounds.top, right.bounds.left,
                       right.bounds.bottom, right.bounds.right,
                       right.slotId, right.itemId);
        });
    return result;
}
}

void WidgetEngine::RevealHostViewKeyboardFocus(
    LuaWidget& widget, std::string_view surface)
{
    const auto& regions = InteractionRegionsForSurface(widget, surface);
    const auto* region = regions.Find(ViewFocusForSurface(widget, surface));
    const auto& tree = IsPanelSurface(surface) ? widget.panelViewTree : widget.viewTree;
    if (!region || !tree) return;
    const auto requests = snowdesktop::widget_runtime::ComputeViewKeyboardScrollOffsets(*tree, *region);
    auto& offsets = ScrollOffsetsForSurface(widget, surface);
    for (const auto& [id, value] : requests)
        offsets[id] = static_cast<int>(std::lround(value));
    if (!requests.empty()) RuntimeInvalidateHost(widget.widgetId);
}

void WidgetEngine::ReconcileHostViewKeyboardFocus(
    LuaWidget& widget, std::string_view surface)
{
    auto& regions = InteractionRegionsForSurface(widget, surface);
    auto& focus = ViewFocusForSurface(widget, surface);
    if (ViewFocusCueForSurface(widget, surface))
    {
        const auto target = regions.ReconcileKeyboardFocus(focus);
        if (target != focus)
        {
            focus.clear();
            if (!IsPanelSurface(surface)) widget.logicalSlotFocus.reset();
            if (!target.empty()) RuntimeFocusViewTarget(widget.widgetId, target, "keyboard");
        }
    }
    else if (!focus.empty() && !regions.IsKeyboardFocusable(focus))
        focus.clear();
}

bool WidgetEngine::RuntimeFocusViewTarget(const std::wstring& widgetId,
    const std::string& id, const char* source)
{
    int index = FindWidget(widgetId);
    if (index < 0 || id.empty()) return false;
    auto& widget = widgets_[index];
    const std::string surface(CurrentWidgetSurface(d2dState_));
    auto& interactionRegions =
        InteractionRegionsForSurface(widget, surface);
    if (!widget.valid || widget.preview ||
        (IsPanelSurface(surface)
            ? !widget.panelActive
            : !RuntimeIsWidgetSelected(widgetId)))
        return false;
    if (RuntimeFocusHostInput(widgetId, id, source))
    {
        RevealHostViewKeyboardFocus(widget, surface);
        return true;
    }
    if (!interactionRegions.IsKeyboardFocusable(id)) return false;

    if (focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId)
        BlurHostInput(false);
    index = FindWidget(widgetId);
    if (index < 0) return false;
    auto& current = widgets_[index];
    auto& currentRegions =
        InteractionRegionsForSurface(current, surface);
    if (!current.valid || current.preview ||
        (IsPanelSurface(surface)
            ? !current.panelActive
            : !RuntimeIsWidgetSelected(widgetId)) ||
        !currentRegions.IsKeyboardFocusable(id))
        return false;
    ViewFocusForSurface(current, surface) = id;
    ViewFocusCueForSurface(current, surface) =
        snowdesktop::widget_runtime::ShouldShowInteractionFocusCue(
            source ? source : "programmatic");
    if (!IsPanelSurface(surface))
    {
        current.logicalSlotFocus.reset();
        const auto slotItems =
            CollectHostLogicalSlotKeyboardItems(*this, current);
        const auto slot = std::find_if(slotItems.begin(), slotItems.end(),
            [&id](const auto& item) { return item.itemId == id; });
        if (slot != slotItems.end())
        {
            current.logicalSlotFocus = LuaWidget::LogicalSlotFocus{
                slot->slotId, slot->itemId };
        }
    }
    RevealHostViewKeyboardFocus(current, surface);
    if (hostInputFocusCallback_) hostInputFocusCallback_();
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeConsumeHostInputCaretVisibilityRequest(
    const std::wstring& widgetId, const std::string& id)
{
    if (!focusedHostInput_.active ||
        focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.id != id ||
        focusedHostInput_.surface != CurrentWidgetSurface(d2dState_))
        return false;
    return focusedHostInput_.caretVisibility.Consume();
}

void WidgetEngine::BeginHostLogicalSlotPointer(
    LuaWidget& widget, int x, int y)
{
    widget.logicalSlotPointerDrag.reset();
    widget.viewFocusCueVisible = false;
    const std::string previousViewFocus = widget.viewKeyboardFocusKey;
    const auto previousSlotFocus = widget.logicalSlotFocus;
    if (widget.preview || !widget.valid)
    {
        widget.viewKeyboardFocusKey.clear();
        widget.logicalSlotFocus.reset();
        return;
    }
    const std::string pointerTarget =
        widget.interactionRegions.TargetAt(
            static_cast<float>(x), static_cast<float>(y));
    widget.viewKeyboardFocusKey =
        widget.interactionRegions.IsKeyboardFocusable(pointerTarget)
        ? pointerTarget : std::string{};
    if (!widget.viewTree)
    {
        widget.logicalSlotFocus.reset();
        if (previousViewFocus != widget.viewKeyboardFocusKey ||
            previousSlotFocus.has_value())
            RuntimeInvalidateHost(widget.widgetId);
        return;
    }
    const POINT point{ widget.lastBounds.left + x,
        widget.lastBounds.top + y };
    const auto items = CollectHostLogicalSlotKeyboardItems(*this, widget);
    const auto hit = std::find_if(items.rbegin(), items.rend(),
        [&](const auto& candidate) {
            return PtInRect(&candidate.bounds, point) != FALSE;
        });
    if (hit == items.rend())
    {
        widget.logicalSlotFocus.reset();
        if (previousViewFocus != widget.viewKeyboardFocusKey ||
            previousSlotFocus.has_value())
            RuntimeInvalidateHost(widget.widgetId);
        return;
    }
    widget.logicalSlotFocus = LuaWidget::LogicalSlotFocus{
        hit->slotId, hit->itemId };
    if (widget.interactionRegions.IsKeyboardFocusable(hit->itemId))
        widget.viewKeyboardFocusKey = hit->itemId;
    if (previousViewFocus != widget.viewKeyboardFocusKey ||
        !previousSlotFocus || previousSlotFocus->slotId != hit->slotId ||
        previousSlotFocus->itemId != hit->itemId)
        RuntimeInvalidateHost(widget.widgetId);
    if (hit->kind !=
            snowdesktop::widget_runtime::LogicalSlotKind::Collection)
        return;
    widget.logicalSlotPointerDrag = LuaWidget::LogicalSlotPointerDrag{
        hit->slotId, hit->slotId, hit->itemId, hit->modelIndex,
        hit->modelIndex, POINT{ x, y }, hit->bounds, RECT{}, false };
}

bool WidgetEngine::UpdateHostLogicalSlotPointer(
    LuaWidget& widget, int x, int y)
{
    if (!widget.logicalSlotPointerDrag) return false;
    auto& drag = *widget.logicalSlotPointerDrag;
    if (!drag.moved)
    {
        const int thresholdX = std::max(4, GetSystemMetrics(SM_CXDRAG));
        const int thresholdY = std::max(4, GetSystemMetrics(SM_CYDRAG));
        if (std::abs(x - drag.start.x) <= thresholdX &&
            std::abs(y - drag.start.y) <= thresholdY)
            return false;
    }
    const POINT point{ widget.lastBounds.left + x,
        widget.lastBounds.top + y };

    const auto* sourceSnapshot =
        widget.logicalSlots.Find(drag.sourceSlotId);
    if (!sourceSnapshot)
    {
        widget.logicalSlotPointerDrag.reset();
        return false;
    }
    const auto sourceItem = std::find_if(sourceSnapshot->items.begin(),
        sourceSnapshot->items.end(), [&drag](const auto& candidate) {
            return candidate.id == drag.itemId;
        });
    if (sourceItem == sourceSnapshot->items.end())
    {
        widget.logicalSlotPointerDrag.reset();
        return false;
    }

    std::string targetSlotId;
    std::uint64_t targetRevision = 0;
    std::optional<snowdesktop::widget_runtime::
        LogicalSlotPointerTarget> target;
    RECT sourceBounds = drag.sourceBounds;
    const auto sourceIndex = static_cast<std::size_t>(sourceItem - sourceSnapshot->items.begin());
    for (const auto& snapshot : widget.logicalSlots.Snapshots())
    {
        if (snapshot.kind !=
            snowdesktop::widget_runtime::LogicalSlotKind::Collection)
            continue;
        const auto surface = RuntimeLogicalSlotSurface(
            widget.widgetId, snapshot.id);
        if (!surface || !PtInRect(&surface->bounds, point)) continue;

        std::vector<RECT> itemBounds;
        std::vector<std::string_view> visibleIds;
        itemBounds.reserve(surface->items.size());
        visibleIds.reserve(surface->items.size());
        for (std::size_t index = 0; index < surface->items.size(); ++index)
        {
            itemBounds.push_back(surface->items[index].bounds);
            visibleIds.push_back(surface->items[index].itemId);
            if (snapshot.id == drag.sourceSlotId &&
                surface->items[index].itemId == drag.itemId)
            {
                sourceBounds = surface->items[index].bounds;
            }
        }

        if (snapshot.id != drag.sourceSlotId)
        {
            const auto declaration =
                widget.logicalSlots.Declarations().find(snapshot.id);
            const bool accepts = declaration !=
                    widget.logicalSlots.Declarations().end() &&
                std::find(declaration->second.accepts.begin(),
                    declaration->second.accepts.end(), sourceItem->kind) !=
                    declaration->second.accepts.end();
            const bool duplicate = std::any_of(snapshot.items.begin(),
                snapshot.items.end(), [&sourceItem](const auto& candidate) {
                    return candidate.kind == sourceItem->kind &&
                        candidate.target == sourceItem->target;
                });
            if (!accepts || duplicate ||
                snapshot.items.size() >= snapshot.capacity)
                continue;
        }
        target = snowdesktop::widget_runtime::
            ResolveLogicalSlotModelPointerTarget(itemBounds, visibleIds,
                snapshot, snapshot.id == drag.sourceSlotId
                    ? std::string_view(drag.itemId) : std::string_view{},
                point, surface->bounds);
        if (target)
        {
            targetSlotId = snapshot.id;
            targetRevision = snapshot.revision;
            break;
        }
    }

    if (!drag.moved)
        widget.interactionRegions.CancelPointerPress();
    const bool changed = !drag.moved ||
        drag.targetSlotId != targetSlotId ||
        (target && (drag.targetIndex != target->targetIndex ||
            !EqualRect(&drag.indicatorBounds, &target->indicator))) ||
        (!target && !IsRectEmpty(&drag.indicatorBounds)) ||
        !EqualRect(&drag.sourceBounds, &sourceBounds);
    drag.moved = true;
    drag.targetRevision = targetRevision;
    drag.sourceIndex = sourceIndex;
    drag.targetSlotId = std::move(targetSlotId);
    drag.targetIndex = target ? target->targetIndex : 0;
    drag.sourceBounds = sourceBounds;
    drag.indicatorBounds = target ? target->indicator : RECT{};
    if (changed) RuntimeInvalidateHost(widget.widgetId);
    return true;
}

bool WidgetEngine::EndHostLogicalSlotPointer(
    const std::wstring& widgetId)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || !widgets_[index].logicalSlotPointerDrag)
        return false;
    const LuaWidget::LogicalSlotPointerDrag drag =
        *widgets_[index].logicalSlotPointerDrag;
    widgets_[index].logicalSlotPointerDrag.reset();
    RuntimeInvalidateHost(widgetId);
    if (!drag.moved) return false;
    if (drag.targetSlotId.empty()) return true;
    const auto* targetSnapshot = widgets_[index].logicalSlots.Find(drag.targetSlotId);
    if (!targetSnapshot || targetSnapshot->revision != drag.targetRevision)
    {
        RuntimeRecordError(widgetId, "logical slot pointer target changed during drag");
        return true;
    }

    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    const bool committed = drag.targetSlotId == drag.sourceSlotId
        ? RuntimeMoveHostLogicalSlotItem(widgetId, drag.sourceSlotId,
            drag.itemId, drag.targetIndex, change, error,
            "host.pointer")
        : RuntimeTransferHostLogicalSlotItem(widgetId,
            drag.sourceSlotId, drag.itemId, drag.targetSlotId,
            drag.targetIndex, change, error, "host.pointer");
    if (!committed)
    {
        RuntimeRecordError(widgetId,
            "logical slot pointer move: " + error);
    }
    return true;
}

namespace
{
float LogicalSlotDropOpacity(
    const snowdesktop::widget_runtime::ViewStyle& style) noexcept
{
    return std::clamp(style.opacity.value_or(1.0f), 0.0f, 1.0f);
}

void DrawLogicalSlotDropSurface(D2DState* state,
    const LogicalSlotHostSurface& surface)
{
    if (!state || !state->ctx ||
        (!surface.dropStyle.background &&
         !surface.dropStyle.borderColor))
        return;
    const D2D1_RECT_F bounds = D2D1::RectF(
        static_cast<float>(surface.bounds.left),
        static_cast<float>(surface.bounds.top),
        static_cast<float>(surface.bounds.right),
        static_cast<float>(surface.bounds.bottom));
    const float radius = std::max(
        0.0f, surface.dropStyle.cornerRadius.value_or(0.0f));
    const D2D1_ROUNDED_RECT rounded =
        D2D1::RoundedRect(bounds, radius, radius);
    const float opacity = LogicalSlotDropOpacity(surface.dropStyle);
    if (surface.dropStyle.background)
        if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                static_cast<int>(*surface.dropStyle.background), opacity))
            state->ctx->FillRoundedRectangle(rounded, fill);
    const float borderWidth = std::max(
        0.0f, surface.dropStyle.borderWidth.value_or(1.0f));
    if (surface.dropStyle.borderColor && borderWidth > 0.0f)
        if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                static_cast<int>(*surface.dropStyle.borderColor), opacity))
            state->ctx->DrawRoundedRectangle(
                rounded, border, borderWidth);
}
}

void WidgetEngine::DrawHostViewInteractionOverlays(
    const LuaWidget& widget,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions,
    std::string_view focusedKey, bool drawLogicalSlotDrag)
{
    if (!d2dState_ || !d2dState_->ctx) return;
    const std::string_view surface = drawLogicalSlotDrag
        ? std::string_view("desktop")
        : std::string_view(widget.panelSurface);
    if (hostInputDropCaret_ && hostInputDropSurface_ == surface &&
        hostInputDropWidgetId_ == widget.widgetId)
    {
        const auto& caret = *hostInputDropCaret_;
        const auto& frame = d2dState_->widgetRect;
        const auto& clip = caret.clip;
        auto* context = d2dState_->ctx;
        context->PushAxisAlignedClip(D2D1::RectF(
            frame.left + static_cast<float>(clip.left),
            frame.top + static_cast<float>(clip.top),
            frame.left + static_cast<float>(clip.right),
            frame.top + static_cast<float>(clip.bottom)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const auto from = D2D1::Point2F(frame.left + caret.left, frame.top + caret.top);
        const auto to = D2D1::Point2F(frame.left + caret.right, frame.top + caret.bottom);
        if (auto* halo = GetCachedBrush(d2dState_, 0x0B1220, 0.72f))
            context->DrawLine(from, to, halo, 4.0f);
        if (auto* accent = GetCachedBrush(d2dState_, 0x72C7FF))
            context->DrawLine(from, to, accent, 2.0f);
        context->PopAxisAlignedClip();
    }
    const bool declarativeSurface = drawLogicalSlotDrag
        ? widget.viewTree.has_value() : widget.panelViewTree.has_value();
    if (!declarativeSurface &&
        ViewFocusCueForSurface(widget, surface) && !focusedKey.empty() &&
        (drawLogicalSlotDrag
            ? RuntimeIsWidgetSelected(widget.widgetId)
            : widget.panelActive))
    {
        const auto* region = regions.Find(focusedKey);
        if (region && region->enabled)
        {
            bool clipped = false;
            if (region->clip)
            {
                d2dState_->ctx->PushAxisAlignedClip(D2D1::RectF(
                    d2dState_->widgetRect.left + region->clip->x,
                    d2dState_->widgetRect.top + region->clip->y,
                    d2dState_->widgetRect.left + region->clip->x +
                        region->clip->width,
                    d2dState_->widgetRect.top + region->clip->y +
                        region->clip->height),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                clipped = true;
            }
            ID2D1SolidColorBrush* halo = GetCachedBrush(
                d2dState_, 0x0B1220, 0.72f);
            ID2D1SolidColorBrush* focus = GetCachedBrush(
                d2dState_, 0x72C7FF, 1.0f);
            if (region->shape.type == snowdesktop::widget_runtime::
                    InteractionShapeType::Circle)
            {
                const D2D1_ELLIPSE ellipse = D2D1::Ellipse(
                    D2D1::Point2F(
                        d2dState_->widgetRect.left + region->shape.x,
                        d2dState_->widgetRect.top + region->shape.y),
                    std::max(0.0f, region->shape.radius - 1.0f),
                    std::max(0.0f, region->shape.radius - 1.0f));
                if (halo) d2dState_->ctx->DrawEllipse(ellipse, halo, 3.5f);
                if (focus) d2dState_->ctx->DrawEllipse(ellipse, focus, 2.0f);
            }
            else
            {
                const D2D1_RECT_F bounds = D2D1::RectF(
                    d2dState_->widgetRect.left + region->shape.x + 1.0f,
                    d2dState_->widgetRect.top + region->shape.y + 1.0f,
                    d2dState_->widgetRect.left + region->shape.x +
                        region->shape.width - 1.0f,
                    d2dState_->widgetRect.top + region->shape.y +
                        region->shape.height - 1.0f);
                const float radius = region->shape.type ==
                        snowdesktop::widget_runtime::
                            InteractionShapeType::RoundedRect
                    ? std::max(0.0f, region->shape.radius - 1.0f) : 3.0f;
                const D2D1_ROUNDED_RECT rounded =
                    D2D1::RoundedRect(bounds, radius, radius);
                if (halo)
                    d2dState_->ctx->DrawRoundedRectangle(
                        rounded, halo, 3.5f);
                if (focus)
                    d2dState_->ctx->DrawRoundedRectangle(
                        rounded, focus, 2.0f);
            }
            if (clipped) d2dState_->ctx->PopAxisAlignedClip();
        }
    }
    if (!drawLogicalSlotDrag || !widget.logicalSlotPointerDrag ||
        !widget.logicalSlotPointerDrag->moved)
        return;
    const auto& drag = *widget.logicalSlotPointerDrag;
    const auto dropSurface = drag.targetSlotId.empty()
        ? std::optional<LogicalSlotHostSurface>{}
        : RuntimeLogicalSlotSurface(widget.widgetId, drag.targetSlotId);
    if (dropSurface)
        DrawLogicalSlotDropSurface(d2dState_, *dropSurface);
    const D2D1_RECT_F source = D2D1::RectF(
        static_cast<float>(drag.sourceBounds.left),
        static_cast<float>(drag.sourceBounds.top),
        static_cast<float>(drag.sourceBounds.right),
        static_cast<float>(drag.sourceBounds.bottom));
    if (ID2D1SolidColorBrush* fill = GetCachedBrush(
            d2dState_, 0x4C9AFF, 0.10f))
        d2dState_->ctx->FillRoundedRectangle(
            D2D1::RoundedRect(source, 6.0f, 6.0f), fill);
    if (ID2D1SolidColorBrush* outline = GetCachedBrush(
            d2dState_, 0x72C7FF, 0.76f))
        d2dState_->ctx->DrawRoundedRectangle(
            D2D1::RoundedRect(source, 6.0f, 6.0f), outline, 1.5f);
    if (drag.targetSlotId.empty() || IsRectEmpty(&drag.indicatorBounds))
        return;
    const D2D1_RECT_F indicator = D2D1::RectF(
        static_cast<float>(drag.indicatorBounds.left),
        static_cast<float>(drag.indicatorBounds.top),
        static_cast<float>(drag.indicatorBounds.right),
        static_cast<float>(drag.indicatorBounds.bottom));
    const int indicatorColor = dropSurface &&
            dropSurface->dropStyle.foreground
        ? static_cast<int>(*dropSurface->dropStyle.foreground)
        : 0x4C9AFF;
    const float indicatorOpacity = dropSurface &&
            dropSurface->dropStyle.foreground
        ? LogicalSlotDropOpacity(dropSurface->dropStyle) : 1.0f;
    if (ID2D1SolidColorBrush* brush = GetCachedBrush(
            d2dState_, indicatorColor, indicatorOpacity))
        d2dState_->ctx->FillRoundedRectangle(
            D2D1::RoundedRect(indicator, 2.0f, 2.0f), brush);
}

bool WidgetEngine::DispatchHostViewKeyEvent(WPARAM key, bool pressed,
    bool repeated, bool ctrl, bool shift, bool alt)
{
    std::wstring widgetId;
    std::string nodeKey;
    std::string surface;
    if (!pressed)
    {
        const auto paired = pressedViewKeyTargets_.find(key);
        if (paired != pressedViewKeyTargets_.end())
        {
            widgetId = paired->second.widgetId;
            nodeKey = paired->second.nodeKey;
            surface = paired->second.surface;
            pressedViewKeyTargets_.erase(paired);
        }
    }
    if (widgetId.empty())
    {
        const auto focused = std::find_if(widgets_.begin(), widgets_.end(),
            [this](const LuaWidget& candidate) {
                return candidate.valid && !candidate.preview &&
                    ((candidate.panelActive &&
                        !candidate.panelViewKeyboardFocusKey.empty()) ||
                     (candidate.hostVisible &&
                        !candidate.viewKeyboardFocusKey.empty() &&
                        RuntimeIsWidgetSelected(candidate.widgetId)));
            });
        if (focused == widgets_.end()) return false;
        widgetId = focused->widgetId;
        surface = focused->panelActive &&
                !focused->panelViewKeyboardFocusKey.empty()
            ? NormalizeWidgetSurface(focused->panelSurface)
            : "desktop";
        nodeKey = ViewFocusForSurface(*focused, surface);
    }

    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    auto& widget = widgets_[index];
    auto& interactionRegions =
        InteractionRegionsForSurface(widget, surface);
    if (!widget.valid || widget.preview ||
        (IsPanelSurface(surface)
            ? !widget.panelActive
            : (!widget.hostVisible || !RuntimeIsWidgetSelected(widgetId))) ||
        !interactionRegions.IsKeyboardFocusable(nodeKey))
        return false;

    const char* eventName = pressed ? "keyDown" : "keyUp";
    const auto* actionPointer = interactionRegions.FindAction(
        nodeKey, eventName);
    if (pressed &&
        (interactionRegions.FindAction(nodeKey, "keyDown") ||
            interactionRegions.FindAction(nodeKey, "keyUp")))
    {
        pressedViewKeyTargets_.insert_or_assign(
            key, PressedViewKeyTarget{ widgetId, nodeKey, surface });
    }
    if (!actionPointer) return false;

    const auto action = *actionPointer;
    const std::string keyName = ViewVirtualKeyName(key);
    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, true);
    WidgetSurfaceScope surfaceScope(d2dState_, surface.c_str());
    return InvokeLifecycleEvent(widget, "action",
        [&action, &nodeKey, &keyName, key, eventName, repeated,
            ctrl, shift, alt, &surface](lua_State* eventState) {
            lua_pushlstring(eventState, action.id.data(), action.id.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, action.value);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, nodeKey.data(), nodeKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushstring(eventState, eventName);
            lua_setfield(eventState, -2, "action");
            lua_pushlstring(eventState, keyName.data(), keyName.size());
            lua_setfield(eventState, -2, "key");
            lua_pushinteger(eventState, static_cast<lua_Integer>(key));
            lua_setfield(eventState, -2, "virtualKey");
            lua_pushboolean(eventState, repeated ? 1 : 0);
            lua_setfield(eventState, -2, "repeat");
            lua_pushboolean(eventState, ctrl ? 1 : 0);
            lua_setfield(eventState, -2, "ctrlKey");
            lua_pushboolean(eventState, shift ? 1 : 0);
            lua_setfield(eventState, -2, "shiftKey");
            lua_pushboolean(eventState, alt ? 1 : 0);
            lua_setfield(eventState, -2, "altKey");
            lua_pushboolean(eventState, 1);
            lua_setfield(eventState, -2, "trustedGesture");
            lua_pushliteral(eventState, "keyboard");
            lua_setfield(eventState, -2, "source");
            lua_pushlstring(eventState, surface.data(), surface.size());
            lua_setfield(eventState, -2, "surface");
        });
}

void WidgetEngine::ClearHostViewKeyState() noexcept
{
    pressedViewKeyTargets_.clear();
}

bool WidgetEngine::HasHostViewKeyboardFocus() const
{
    return std::any_of(widgets_.begin(), widgets_.end(), [this](const auto& widget) {
        return widget.valid && !widget.preview &&
            (widget.panelActive || (!widget.viewKeyboardFocusKey.empty() &&
                RuntimeIsWidgetSelected(widget.widgetId)));
    });
}

namespace
{
std::optional<std::size_t> FindNextHostViewFocus(
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions,
    std::span<const std::string> focusKeys, std::size_t current, WPARAM key)
{
    using namespace snowdesktop::widget_runtime;
    LogicalSlotFocusDirection direction;
    switch (key)
    {
    case VK_LEFT: direction = LogicalSlotFocusDirection::Left; break;
    case VK_RIGHT: direction = LogicalSlotFocusDirection::Right; break;
    case VK_UP: direction = LogicalSlotFocusDirection::Up; break;
    case VK_DOWN: direction = LogicalSlotFocusDirection::Down; break;
    default: return std::nullopt;
    }

    std::vector<LogicalSlotFocusRect> bounds;
    bounds.reserve(focusKeys.size());
    for (const auto& candidateKey : focusKeys)
    {
        const auto* candidate = regions.Find(candidateKey);
        if (!candidate) return std::nullopt;
        const auto& shape = candidate->shape;
        const bool circle = shape.type == InteractionShapeType::Circle;
        const float left = circle ? shape.x - shape.radius : shape.x;
        const float top = circle ? shape.y - shape.radius : shape.y;
        const float right = shape.x + (circle ? shape.radius : shape.width);
        const float bottom = shape.y + (circle ? shape.radius : shape.height);
        bounds.push_back({
            static_cast<int>(std::floor(left)),
            static_cast<int>(std::floor(top)),
            static_cast<int>(std::ceil(right)),
            static_cast<int>(std::ceil(bottom)) });
    }
    return FindLogicalSlotSpatialFocus(bounds, current, direction);
}
}

bool WidgetEngine::HandleHostViewKey(const std::wstring& widgetId,
    WPARAM key, bool ctrl, bool shift, bool alt,
    std::string_view surface, bool repeated)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || ctrl) return false;
    auto& widget = widgets_[index];
    if (widget.preview || !widget.valid) return false;
    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);
    WidgetSurfaceScope surfaceScope(d2dState_, normalizedSurface.c_str());
    const bool panelSurface = IsPanelSurface(normalizedSurface);
    if ((panelSurface && !widget.panelActive) ||
        (!panelSurface && !RuntimeIsWidgetSelected(widgetId)))
        return false;
    auto& surfaceRegions = InteractionRegionsForSurface(
        widget, normalizedSurface);
    auto& surfaceFocus = ViewFocusForSurface(widget, normalizedSurface);
    // A pointer target can be the next navigation starting point without a cue.
    const auto showFocusCue = [&] {
        if (!std::exchange(ViewFocusCueForSurface(widget, normalizedSurface), true))
            RuntimeInvalidateHost(widgetId, std::nullopt, normalizedSurface);
    };
    if (!alt)
    {
        const auto* select = surfaceRegions.ExpandedSelectForFocus(surfaceFocus);
        if (select && key == VK_TAB)
        {
            const auto parent = select->key;
            auto keys = surfaceRegions.KeyboardFocusableKeys();
            std::erase_if(keys, [&](const auto& id) {
                const auto* region = surfaceRegions.Find(id);
                return region && !region->keyboardNavigationGroup.empty();
            });
            const auto found = std::find(keys.begin(), keys.end(), parent);
            const auto current = found == keys.end() ? std::optional<std::size_t>{} :
                std::optional<std::size_t>(static_cast<std::size_t>(found - keys.begin()));
            const auto next = snowdesktop::widget_runtime::CycleLogicalSlotFocus(keys.size(), current, shift);
            const std::string target = next ? keys[*next] : parent;
            surfaceFocus = target;
            ViewFocusCueForSurface(widget, normalizedSurface) = true;
            snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(trustedGestureState_, true);
            DispatchInteractionAction(widget, parent, "click", 0, 0, 1, 0, 1,
                false, "keyboard", 0, std::nullopt, std::nullopt, normalizedSurface);
            RuntimeFocusViewTarget(widgetId, target, "keyboard");
            RuntimeInvalidateHost(widgetId);
            return true;
        }
        if (select && key == VK_ESCAPE)
        {
            const auto target = select->key;
            surfaceFocus = target;
            ViewFocusCueForSurface(widget, normalizedSurface) = true;
            snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(trustedGestureState_, true);
            DispatchInteractionAction(widget, target, "click", 0, 0, 1, 0, 1,
                false, "keyboard", 0, std::nullopt, std::nullopt, normalizedSurface);
            RuntimeInvalidateHost(widgetId);
            return true;
        }
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
            key == VK_HOME || key == VK_END)
        {
            const auto target = surfaceRegions.SelectKeyboardTarget(surfaceFocus,
                key == VK_LEFT || key == VK_UP || key == VK_HOME ? -1 : 1,
                key == VK_HOME || key == VK_END);
            if (target) return RuntimeFocusViewTarget(widgetId, *target, "keyboard");
        }
    }
    if (alt && !repeated && key <= 0x7f)
    {
        const auto* foundAccessTarget = surfaceRegions.FindAccessKey(
            static_cast<char>(key));
        if (foundAccessTarget)
        {
            const auto accessTarget = *foundAccessTarget;
            const std::string targetKey = accessTarget.key;
            const auto input = std::find_if(widget.hostControls.rbegin(),
                widget.hostControls.rend(), [&](const auto& control) {
                    return control.type ==
                            LuaWidget::HostControl::Type::Input &&
                        control.enabled && control.id == targetKey &&
                        HostControlBelongsToSurface(
                            control, normalizedSurface);
                });
            if (input != widget.hostControls.rend())
            {
                if (!RuntimeFocusHostInput(
                        widgetId, targetKey, "accessKey"))
                    return false;
            }
            else
            {
                if (focusedHostInput_.active &&
                    focusedHostInput_.widgetId == widgetId)
                    BlurHostInput(false);
                surfaceFocus = targetKey;
                ViewFocusCueForSurface(widget, normalizedSurface) = true;
            }
            if (!panelSurface)
            {
                widget.logicalSlotFocus.reset();
                const auto slotItems =
                    CollectHostLogicalSlotKeyboardItems(*this, widget);
                const auto slot = std::find_if(
                    slotItems.begin(), slotItems.end(),
                    [&targetKey](const auto& item) {
                        return item.itemId == targetKey;
                    });
                if (slot != slotItems.end())
                    widget.logicalSlotFocus = LuaWidget::LogicalSlotFocus{
                        slot->slotId, slot->itemId };
            }
            if (accessTarget.controlKind !=
                    snowdesktop::widget_runtime::
                        InteractionControlKind::Slider &&
                surfaceRegions.ResolveAction(targetKey, "click"))
            {
                const auto& shape = accessTarget.shape;
                const int x = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.x : shape.x + shape.width * 0.5f));
                const int y = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.y : shape.y + shape.height * 0.5f));
                snowdesktop::widget_runtime::WidgetTrustedGestureScope
                    gestureScope(trustedGestureState_, true);
                DispatchInteractionAction(widget, targetKey, "click",
                    x, y, 0, 0, 1, false, "accessKey", 0,
                    std::nullopt, std::nullopt, normalizedSurface);
            }
            RuntimeInvalidateHost(widgetId);
            return true;
        }
    }
    if (IsPanelSurface(normalizedSurface))
    {
        auto& regions = widget.panelInteractionRegions;
        auto& focusKey = widget.panelViewKeyboardFocusKey;
        const auto focusKeys = regions.KeyboardFocusableKeys(focusKey);
        if (focusKeys.empty())
        {
            focusKey.clear();
            widget.panelViewFocusCueVisible = false;
            return false;
        }
        const auto focusedIndex = [&]() -> std::optional<std::size_t> {
            if (focusKey.empty()) return std::nullopt;
            const auto found = std::find(
                focusKeys.begin(), focusKeys.end(), focusKey);
            if (found == focusKeys.end()) return std::nullopt;
            return static_cast<std::size_t>(
                std::distance(focusKeys.begin(), found));
        }();
        const auto focusTarget = [&](const std::string& targetKey) {
            const auto input = std::find_if(widget.hostControls.rbegin(),
                widget.hostControls.rend(), [&](const auto& control) {
                    return control.type ==
                            LuaWidget::HostControl::Type::Input &&
                        control.enabled && control.id == targetKey &&
                        HostControlBelongsToSurface(
                            control, normalizedSurface);
                });
            if (input != widget.hostControls.rend())
            {
                if (!RuntimeFocusHostInput(
                        widgetId, targetKey, "keyboard"))
                    return false;
            }
            else
            {
                if (focusedHostInput_.active &&
                    focusedHostInput_.widgetId == widgetId)
                    BlurHostInput(false);
                focusKey = targetKey;
                widget.panelViewFocusCueVisible = true;
            }
            RevealHostViewKeyboardFocus(widget, normalizedSurface);
            RuntimeInvalidateHost(widgetId);
            return true;
        };
        if (key == VK_TAB && !alt)
        {
            const auto next = snowdesktop::widget_runtime::
                CycleLogicalSlotFocus(
                    focusKeys.size(), focusedIndex, shift);
            return next ? focusTarget(focusKeys[*next]) : false;
        }
        if (!focusedIndex)
        {
            if (!alt && (key == VK_RETURN || key == VK_SPACE ||
                    key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN))
                return focusTarget(focusKeys.front());
            return false;
        }
        const std::string focusedKey = focusKeys[*focusedIndex];
        const auto* focusedRegion = regions.Find(focusedKey);
        if (!focusedRegion) return false;
        if (key == VK_ESCAPE)
        {
            if (focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId &&
                focusedHostInput_.id == focusedKey &&
                IsPanelSurface(focusedHostInput_.surface))
                BlurHostInput(true);
            // Panel Escape belongs to its dismissal policy. Retain the logical
            // keyboard session when a non-dismissible panel remains open.
            return false;
        }
        if (key == VK_LEFT || key == VK_RIGHT ||
            key == VK_UP || key == VK_DOWN)
        {
            if (alt) return false;
            showFocusCue();
            if (focusedRegion->controlKind ==
                snowdesktop::widget_runtime::
                    InteractionControlKind::Slider)
            {
                const int direction =
                    key == VK_LEFT || key == VK_DOWN ? -1 : 1;
                const auto stepped = regions.ResolveKeyboardStep(
                    focusedKey, direction);
                if (stepped && stepped->controlValue &&
                    stepped->previousControlValue &&
                    std::abs(*stepped->controlValue -
                        *stepped->previousControlValue) > 0.000001f)
                {
                    const auto& shape = focusedRegion->shape;
                    const int x = static_cast<int>(std::lround(
                        shape.type == snowdesktop::widget_runtime::
                                InteractionShapeType::Circle
                            ? shape.x
                            : shape.x + shape.width * 0.5f));
                    const int y = static_cast<int>(std::lround(
                        shape.type == snowdesktop::widget_runtime::
                                InteractionShapeType::Circle
                            ? shape.y
                            : shape.y + shape.height * 0.5f));
                    snowdesktop::widget_runtime::WidgetTrustedGestureScope
                        gestureScope(trustedGestureState_, true);
                    DispatchInteractionAction(widget, focusedKey, "change",
                        x, y, 0, 0, 0, false, "keyboard", direction,
                        std::nullopt, std::nullopt,
                        normalizedSurface);
                    RuntimeInvalidateHost(widgetId);
                }
                return true;
            }
            const auto next = FindNextHostViewFocus(
                regions, focusKeys, *focusedIndex, key);
            if (next) focusTarget(focusKeys[*next]);
            return true;
        }
        if ((key == VK_RETURN || key == VK_SPACE) && !alt)
        {
            if (repeated) return true;
            showFocusCue();
            const auto input = std::find_if(widget.hostControls.rbegin(),
                widget.hostControls.rend(), [&](const auto& control) {
                    return control.type ==
                            LuaWidget::HostControl::Type::Input &&
                        control.enabled && control.id == focusedKey &&
                        HostControlBelongsToSurface(
                            control, normalizedSurface);
                });
            if (input != widget.hostControls.rend())
                return RuntimeFocusHostInput(
                    widgetId, focusedKey, "keyboard");
            if (focusedRegion->controlKind !=
                snowdesktop::widget_runtime::
                    InteractionControlKind::Slider)
            {
                const auto& shape = focusedRegion->shape;
                const int x = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.x
                        : shape.x + shape.width * 0.5f));
                const int y = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.y
                        : shape.y + shape.height * 0.5f));
                snowdesktop::widget_runtime::WidgetTrustedGestureScope
                    gestureScope(trustedGestureState_, true);
                DispatchInteractionAction(widget, focusedKey, "click",
                    x, y, 0, 0, 1, false, "keyboard", 0,
                    std::nullopt, std::nullopt,
                    normalizedSurface);
                RuntimeInvalidateHost(widgetId);
            }
            return true;
        }
        return false;
    }
    const auto focusKeys = widget.interactionRegions.KeyboardFocusableKeys(widget.viewKeyboardFocusKey);
    if (focusKeys.empty())
    {
        widget.viewKeyboardFocusKey.clear();
        widget.viewFocusCueVisible = false;
        widget.logicalSlotFocus.reset();
        return false;
    }
    const auto slotItems = CollectHostLogicalSlotKeyboardItems(*this, widget);
    const auto focusedIndex = [&]() -> std::optional<std::size_t> {
        if (widget.viewKeyboardFocusKey.empty()) return std::nullopt;
        const auto found = std::find(
            focusKeys.begin(), focusKeys.end(),
            widget.viewKeyboardFocusKey);
        if (found == focusKeys.end()) return std::nullopt;
        return static_cast<std::size_t>(
            std::distance(focusKeys.begin(), found));
    }();

    auto synchronizeSlotFocus = [&](std::string_view targetKey) {
        const auto slot = std::find_if(slotItems.begin(), slotItems.end(),
            [targetKey](const auto& item) {
                return item.itemId == targetKey;
            });
        if (slot == slotItems.end())
            widget.logicalSlotFocus.reset();
        else
        {
            widget.logicalSlotFocus = LuaWidget::LogicalSlotFocus{
                slot->slotId, slot->itemId };
        }
    };
    auto focusTarget = [&](const std::string& targetKey) {
        const auto input = std::find_if(widget.hostControls.rbegin(),
            widget.hostControls.rend(), [&](const auto& control) {
                return control.type == LuaWidget::HostControl::Type::Input &&
                    control.enabled && control.id == targetKey &&
                    HostControlBelongsToSurface(control, normalizedSurface);
            });
        if (input != widget.hostControls.rend())
        {
            if (!RuntimeFocusHostInput(widgetId, targetKey, "keyboard"))
                return false;
        }
        else
        {
            if (focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId)
                BlurHostInput(false);
            widget.viewKeyboardFocusKey = targetKey;
            widget.viewFocusCueVisible = true;
        }
        synchronizeSlotFocus(targetKey);
        RevealHostViewKeyboardFocus(widget, normalizedSurface);
        RuntimeInvalidateHost(widgetId);
        return true;
    };
    auto focusedSlot = [&]() -> const HostLogicalSlotKeyboardItem* {
        if (!widget.logicalSlotFocus) return nullptr;
        const auto found = std::find_if(slotItems.begin(), slotItems.end(),
            [&](const auto& item) {
                return item.slotId == widget.logicalSlotFocus->slotId &&
                    item.itemId == widget.logicalSlotFocus->itemId;
            });
        return found == slotItems.end() ? nullptr : &*found;
    };

    if (key == VK_RETURN)
    {
        const auto entry = snowdesktop::widget_runtime::
            EnterWidgetKeyboardFocus(
                focusKeys.size(), focusedIndex, shift, alt, repeated);
        if (entry)
            return focusTarget(focusKeys[*entry]);
    }
    if (key == VK_TAB && !alt)
    {
        const auto next = snowdesktop::widget_runtime::CycleLogicalSlotFocus(
            focusKeys.size(), focusedIndex, shift);
        if (!next) return false;
        return focusTarget(focusKeys[*next]);
    }
    if (!focusedIndex) return false;
    const std::string focusedKey = focusKeys[*focusedIndex];
    const auto* focusedRegion =
        widget.interactionRegions.Find(focusedKey);
    if (!focusedRegion) return false;
    if (key == VK_ESCAPE)
    {
        if (focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId &&
            focusedHostInput_.id == focusedKey)
            BlurHostInput(true);
        widget.logicalSlotFocus.reset();
        widget.viewKeyboardFocusKey.clear();
        widget.viewFocusCueVisible = false;
        RuntimeInvalidateHost(widgetId);
        return true;
    }
    if (key == VK_LEFT || key == VK_RIGHT ||
        key == VK_UP || key == VK_DOWN)
    {
        if (!alt && focusedRegion->controlKind ==
                snowdesktop::widget_runtime::InteractionControlKind::Slider)
        {
            showFocusCue();
            const int direction = key == VK_LEFT || key == VK_DOWN ? -1 : 1;
            const auto stepped = widget.interactionRegions.ResolveKeyboardStep(
                focusedKey, direction);
            if (stepped && stepped->controlValue &&
                stepped->previousControlValue &&
                std::abs(*stepped->controlValue -
                    *stepped->previousControlValue) > 0.000001f)
            {
                const auto& shape = focusedRegion->shape;
                const int x = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.x : shape.x + shape.width * 0.5f));
                const int y = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.y : shape.y + shape.height * 0.5f));
                snowdesktop::widget_runtime::WidgetTrustedGestureScope
                    gestureScope(trustedGestureState_, true);
                DispatchInteractionAction(widget, focusedKey, "change",
                    x, y, 0, 0, 0, false, "keyboard", direction);
                RuntimeInvalidateHost(widgetId);
            }
            return true;
        }
        if (alt)
        {
            const auto* slot = focusedSlot();
            if (!slot) return false;
            showFocusCue();
            if (slot->kind != snowdesktop::widget_runtime::
                    LogicalSlotKind::Collection)
                return true;
            const int direction = key == VK_LEFT || key == VK_UP ? -1 : 1;
            const auto target = snowdesktop::widget_runtime::
                MoveLogicalSlotItemTarget(
                    slot->itemCount, slot->modelIndex, direction);
            if (!target) return true;
            snowdesktop::widget_runtime::LogicalSlotChange change;
            std::string error;
            if (!RuntimeMoveHostLogicalSlotItem(widgetId, slot->slotId,
                    slot->itemId, *target, change, error,
                    "host.keyboard"))
            {
                RuntimeRecordError(widgetId,
                    "logical slot keyboard reorder: " + error);
            }
            return true;
        }
        showFocusCue();
        const auto next = FindNextHostViewFocus(
            widget.interactionRegions, focusKeys, *focusedIndex, key);
        if (next)
            focusTarget(focusKeys[*next]);
        return true;
    }
    if (key == VK_DELETE && !alt)
    {
        const auto* slot = focusedSlot();
        if (!slot) return false;
        showFocusCue();
        const bool removable = slot->kind == snowdesktop::widget_runtime::
                LogicalSlotKind::Collection || slot->allowClear;
        if (!removable) return true;
        std::string fallback;
        if (focusKeys.size() > 1)
        {
            const std::size_t next = *focusedIndex + 1 < focusKeys.size()
                ? *focusedIndex + 1 : *focusedIndex - 1;
            fallback = focusKeys[next];
        }
        snowdesktop::widget_runtime::LogicalSlotChange change;
        std::string error;
        if (RuntimeRemoveHostLogicalSlotItem(widgetId, slot->slotId,
                slot->itemId, change, error, "host.keyboard"))
        {
            widget.viewKeyboardFocusKey = std::move(fallback);
            synchronizeSlotFocus(widget.viewKeyboardFocusKey);
        }
        else
            RuntimeRecordError(widgetId,
                "logical slot keyboard remove: " + error);
        RuntimeInvalidateHost(widgetId);
        return true;
    }
    if ((key == VK_RETURN || key == VK_SPACE) && !alt)
    {
        if (repeated) return true;
        showFocusCue();
        const auto input = std::find_if(widget.hostControls.rbegin(),
            widget.hostControls.rend(), [&](const auto& control) {
                return control.type == LuaWidget::HostControl::Type::Input &&
                    control.enabled && control.id == focusedKey &&
                    HostControlBelongsToSurface(control, normalizedSurface);
            });
        if (input != widget.hostControls.rend())
            return RuntimeFocusHostInput(widgetId, focusedKey, "keyboard");
        if (focusedRegion->controlKind != snowdesktop::widget_runtime::
                InteractionControlKind::Slider)
        {
            const auto& shape = focusedRegion->shape;
            const int x = static_cast<int>(std::lround(
                shape.type == snowdesktop::widget_runtime::
                        InteractionShapeType::Circle
                    ? shape.x : shape.x + shape.width * 0.5f));
            const int y = static_cast<int>(std::lround(
                shape.type == snowdesktop::widget_runtime::
                        InteractionShapeType::Circle
                    ? shape.y : shape.y + shape.height * 0.5f));
            snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
                trustedGestureState_, true);
            DispatchInteractionAction(widget, focusedKey, "click",
                x, y, 0, 0, 1, false, "keyboard");
            RuntimeInvalidateHost(widgetId);
        }
        return true;
    }
    return false;
}

bool WidgetEngine::HandleHostInputPointerMove(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface)
{
    if (HandleHostScrollbarPointer(widgetId, x, y, surface, false))
        return true;
    const int widgetIndex = FindWidget(widgetId);
    if (!IsPanelSurface(surface) && widgetIndex >= 0 &&
        UpdateHostLogicalSlotPointer(widgets_[widgetIndex], x, y))
        return true;
    if (!focusedHostInput_.active ||
        !focusedHostInput_.pointerSelecting ||
        focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.surface != surface)
        return false;
    if (focusedHostInput_.selectionDragArmed)
        return true;
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    auto& widget = widgets_[index];
    auto control = std::find_if(
        widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& item) {
            return item.type ==
                LuaWidget::HostControl::Type::Input &&
                item.id == focusedHostInput_.id &&
                HostControlBelongsToSurface(item, surface);
        });
    if (control == widget.hostControls.rend())
        return false;
    if (control->multiline)
    {
        int offset = RuntimeGetScrollOffset(
            widgetId, control->id, surface);
        if (y < control->rect.top +
            static_cast<int>(std::lround(control->padding.top)))
            offset -= 24;
        else if (y > control->rect.bottom -
            static_cast<int>(std::lround(control->padding.bottom)))
            offset += 24;
        RuntimeSetScrollOffset(widgetId,
            control->id, offset, surface);
    }
    focusedHostInput_.cursor = HitTestHostInputPosition(
        *control, widgetId, x, y);
    focusedHostInput_.caretVisibility.Request();
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::HandleHostInputPointerUp(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface)
{
    if (HandleHostScrollbarPointer(widgetId, x, y, surface, true))
        return true;
    if (!IsPanelSurface(surface) && EndHostLogicalSlotPointer(widgetId))
        return true;
    if (!focusedHostInput_.active ||
        !focusedHostInput_.pointerSelecting ||
        focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.surface != surface)
        return false;
    const bool dragWasArmed = std::exchange(focusedHostInput_.selectionDragArmed, false);
    const int index = FindWidget(widgetId);
    if (index >= 0)
    {
        auto& controls = widgets_[index].hostControls;
        auto control = std::find_if(controls.rbegin(),
            controls.rend(), [&](const auto& item) {
                return item.type ==
                    LuaWidget::HostControl::Type::Input &&
                    item.id == focusedHostInput_.id &&
                    HostControlBelongsToSurface(item, surface);
            });
        if (control != controls.rend())
        {
            const auto dropPosition = dragWasArmed
                ? ResolveHostInputTextDropPosition(
                    widgets_[index], *control, {x, y}, focusedHostInput_.text, nullptr)
                : std::nullopt;
            focusedHostInput_.cursor = dropPosition ? *dropPosition :
                HitTestHostInputPosition(*control, widgetId, x, y);
            if (dragWasArmed)
                focusedHostInput_.selectionAnchor = focusedHostInput_.cursor;
            focusedHostInput_.caretVisibility.Request();
        }
    }
    DispatchHostInputSelectionChange(widgetId,
        focusedHostInput_.id,
        focusedHostInput_.selectionChangeAction,
        focusedHostInput_.text,
        focusedHostInput_.pointerSelectionStartAnchor,
        focusedHostInput_.pointerSelectionStartCursor,
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.cursor, "pointer");
    focusedHostInput_.pointerSelecting = false;
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeIsWidgetSelected(
    const std::wstring& widgetId) const
{
    return widgetSelectedProvider_ &&
        widgetSelectedProvider_(widgetId);
}

std::vector<LuaWidgetAccessibilitySnapshot>
WidgetEngine::RuntimeAccessibilitySnapshots() const
{
    std::vector<LuaWidgetAccessibilitySnapshot> result;
    for (const auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview || !widget.hostVisible)
            continue;
        LuaWidgetAccessibilitySnapshot snapshot;
        snapshot.widgetId = widget.widgetId;
        snapshot.packageId = widget.packageId;
        snapshot.name = widget.name;
        snapshot.bounds = widget.lastBounds;
        snapshot.selected = RuntimeIsWidgetSelected(widget.widgetId);
        const std::string_view desktopFocus = focusedHostInput_.active &&
                focusedHostInput_.widgetId == widget.widgetId &&
                focusedHostInput_.surface != "desktop"
            ? std::string_view{} : std::string_view(widget.viewKeyboardFocusKey);
        const bool collected = widget.viewTree
            ? snowdesktop::widget_runtime::CollectViewAccessibilityNodes(
                *widget.viewTree, desktopFocus,
                snapshot.nodes, snapshot.error)
            : snowdesktop::widget_runtime::
                CollectInteractionAccessibilityNodes(
                    widget.interactionRegions.AccessibilityRegions(),
                    static_cast<float>(std::max<LONG>(0,
                        widget.lastBounds.right - widget.lastBounds.left)),
                    static_cast<float>(std::max<LONG>(0,
                        widget.lastBounds.bottom - widget.lastBounds.top)),
                    desktopFocus,
                    snapshot.nodes, snapshot.error);
        (void)collected;
        for (auto& node : snapshot.nodes)
        {
            if (node.key.empty()) continue;
            const auto projected = snowdesktop::widget_runtime::
                ProjectHostInputAccessibility(widget.hostControls,
                    focusedHostInput_, widget.widgetId, node.key, "desktop",
                    [this, &widget](const std::string& key) {
                        return RuntimeGetStorageValue(widget.widgetId, key);
                    }, [](const std::wstring& value) { return WideToUtf8(value); });
            if (!projected) continue;
            const auto* control = projected->control;
            node.valueText = projected->valueText;
            node.focused = projected->focused;
            node.valueReadOnly = control->readOnly;
            if (d2dState_)
            {
                text_input::AccessibleDocument document;
                document.text=Utf8ToWide(node.valueText);
                document.cursor=document.anchor=document.text.size();
                if(projected->focused)
                {document.cursor=projected->cursor;document.anchor=projected->anchor;}
                document.enabled=node.enabled;document.readOnly=control->readOnly;
                const float innerWidth=std::max(1.f,node.bounds.width-control->padding.left-control->padding.right-(control->multiline?8.f:0.f));
                const float innerHeight=std::max(1.f,node.bounds.height-control->padding.top-control->padding.bottom);
                auto layout=control->multiline
                    ? CreateHostMultilineTextLayout(d2dState_,document.text,control->fontSize,innerWidth)
                    : CreateHostSingleLineTextLayout(d2dState_,document.text,control->fontSize,innerWidth,innerHeight);
                const float left=static_cast<float>(widget.lastBounds.left)+node.bounds.x+control->padding.left;
                const float top=static_cast<float>(widget.lastBounds.top)+node.bounds.y+control->padding.top;
                const float scroll=control->multiline?static_cast<float>(RuntimeGetScrollOffset(widget.widgetId,node.key)):0.f;
                const D2D1_RECT_F clip=D2D1::RectF(left,top,left+innerWidth,top+innerHeight);
                auto access=std::make_shared<text_input::TextAccess>();
                access->document=[document]{return std::optional<text_input::AccessibleDocument>{document};};
                access->rectangles=[layout,left,top,scroll,clip](std::size_t start,std::size_t end){
                    std::vector<text_input::Rectangle> boxesResult;if(!layout)return boxesResult;
                    UINT32 count=0;layout->HitTestTextRange(static_cast<UINT32>(start),static_cast<UINT32>(end-start),left,top-scroll,nullptr,0,&count);
                    std::vector<DWRITE_HIT_TEST_METRICS> boxes(count);
                    if(count&&SUCCEEDED(layout->HitTestTextRange(static_cast<UINT32>(start),static_cast<UINT32>(end-start),left,top-scroll,boxes.data(),count,&count)))
                        for(const auto& box:boxes){const float x=std::max(clip.left,box.left),y=std::max(clip.top,box.top);
                            const float right=std::min(clip.right,box.left+box.width),bottom=std::min(clip.bottom,box.top+box.height);
                            if(right>=x&&bottom>y)boxesResult.push_back({x,y,right-x,bottom-y});}
                    return boxesResult;
                };
                access->hit=[layout,left,top,scroll,text=document.text](text_input::Point point)->std::optional<std::size_t>{
                    if(!layout)return {};BOOL trailing=FALSE,inside=FALSE;DWRITE_HIT_TEST_METRICS hit{};
                    if(FAILED(layout->HitTestPoint(static_cast<float>(point.x)-left,static_cast<float>(point.y)-top+scroll,&trailing,&inside,&hit)))return {};
                    return text_input::SnapBoundary(text,hit.textPosition+(trailing?hit.length:0));
                };
                access->unitBoundaries=[layout,length=document.text.size()](text_input::Unit unit){
                    std::vector<std::size_t> positions;if(unit!=text_input::Unit::Line||!layout)return positions;
                    UINT32 count=0;layout->GetLineMetrics(nullptr,0,&count);std::vector<DWRITE_LINE_METRICS> lines(count);
                    if(count&&SUCCEEDED(layout->GetLineMetrics(lines.data(),count,&count))){std::size_t at=0;positions.push_back(0);
                        for(const auto& line:lines){at=std::min(length,at+line.length);positions.push_back(at);}}
                    return positions;
                };
                access->visibleRanges=[layout,scroll,innerHeight,length=document.text.size()]{
                    std::vector<std::pair<std::size_t,std::size_t>> ranges;if(!layout)return ranges;
                    UINT32 count=0;layout->GetLineMetrics(nullptr,0,&count);std::vector<DWRITE_LINE_METRICS> lines(count);
                    if(count&&SUCCEEDED(layout->GetLineMetrics(lines.data(),count,&count))){std::size_t at=0;float y=-scroll;
                        for(const auto& line:lines){const auto end=std::min(length,at+line.length);if(y+line.height>0&&y<innerHeight)ranges.emplace_back(at,end);at=end;y+=line.height;}}
                    return ranges;
                };
                node.textAccess=std::move(access);node.textAccessUsesScreenCoordinates=false;
            }
            if (control->numeric)
            {
                node.minimum = control->minimum;
                node.maximum = control->maximum;
                node.step = control->step;
                node.rangeValueReadOnly = control->readOnly;
                float parsed = 0.0f;
                const auto parsedResult = std::from_chars(
                    node.valueText.data(),
                    node.valueText.data() + node.valueText.size(), parsed);
                if (parsedResult.ec == std::errc{} &&
                    parsedResult.ptr == node.valueText.data() +
                        node.valueText.size() && std::isfinite(parsed))
                    node.value = parsed;
            }
        }
        result.push_back(std::move(snapshot));
    }
    return result;
}

bool WidgetEngine::RuntimeSetAccessibilityFocus(
    const std::wstring& widgetId, const std::string& nodeKey)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || !RuntimeIsWidgetSelected(widgetId)) return false;
    auto& widget = widgets_[index];
    if (!widget.valid || widget.preview || !widget.hostVisible ||
        !widget.interactionRegions.IsKeyboardFocusable(nodeKey))
        return false;
    WidgetSurfaceScope surfaceScope(d2dState_, "desktop");

    const auto input = std::find_if(widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& control) {
            return control.type == LuaWidget::HostControl::Type::Input &&
                control.enabled && control.id == nodeKey &&
                HostControlBelongsToSurface(control, "desktop");
        });
    if (input != widget.hostControls.rend())
    {
        if (!RuntimeFocusHostInput(widgetId, nodeKey, "accessibility"))
            return false;
    }
    else
    {
        if (focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId)
            BlurHostInput(false);
        widget.viewKeyboardFocusKey = nodeKey;
        widget.viewFocusCueVisible = true;
    }

    widget.logicalSlotFocus.reset();
    const auto slotItems = CollectHostLogicalSlotKeyboardItems(*this, widget);
    const auto slot = std::find_if(slotItems.begin(), slotItems.end(),
        [&](const auto& item) { return item.itemId == nodeKey; });
    if (slot != slotItems.end())
        widget.logicalSlotFocus = LuaWidget::LogicalSlotFocus{
            slot->slotId, slot->itemId };
    RevealHostViewKeyboardFocus(widget, "desktop");
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::HandleHostScrollbarPointer(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface, bool finish)
{
    const std::string normalizedSurface = NormalizeWidgetSurface(surface);
    if (!hostScrollbarDrag_.active ||
        hostScrollbarDrag_.widgetId != widgetId ||
        hostScrollbarDrag_.surface != normalizedSurface)
        return false;

    const HostScrollbarDrag drag = hostScrollbarDrag_;
    if (finish)
        hostScrollbarDrag_ = {};
    const int index = FindWidget(widgetId);
    if (index < 0)
        return true;
    auto& widget = widgets_[index];
    const auto control = std::find_if(
        widget.hostControls.rbegin(), widget.hostControls.rend(),
        [&](const auto& item) {
            return item.enabled && item.id == drag.id &&
                HostControlBelongsToSurface(item, normalizedSurface) &&
                (item.type == LuaWidget::HostControl::Type::Scroll ||
                    (item.type == LuaWidget::HostControl::Type::Input &&
                        item.multiline));
        });
    if (control == widget.hostControls.rend())
        return true;

    const int contentExtent = drag.horizontal
        ? control->contentWidth : control->contentHeight;
    const int visibleExtent = drag.horizontal
        ? control->viewportWidth : control->viewportHeight;
    const int viewportStart = drag.horizontal
        ? control->rect.left : control->rect.top;
    const int viewportEnd = drag.horizontal
        ? control->rect.right : control->rect.bottom;
    const float scale = CalculateWidgetCellScale(
        widget.layoutMetrics.gridCellWidth,
        widget.layoutMetrics.gridCellHeight);
    const auto geometry = snowdesktop::widget_scroll_rules::
        ResolveScrollbarAxisGeometry(
            viewportStart, viewportEnd, contentExtent,
            visibleExtent, drag.offsetStart, scale);
    const int pointer = drag.horizontal ? x : y;
    const int previous = RuntimeGetScrollOffset(
        widgetId, drag.id, normalizedSurface);
    const int current = snowdesktop::widget_scroll_rules::
        ApplyScrollbarThumbDrag(
            drag.offsetStart, pointer - drag.pointerStart, geometry);
    ScrollOffsetsForSurface(widget, normalizedSurface)[drag.id] = current;
    if (current != previous)
    {
        if (focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId &&
            focusedHostInput_.id == drag.id &&
            focusedHostInput_.surface == normalizedSurface)
        {
            focusedHostInput_.caretVisibility.PreserveManualScroll();
        }
        if (snowdesktop::widget_scroll_rules::ReachedScrollEnd(
                previous, current, geometry.maximum))
        {
            snowdesktop::widget_runtime::WidgetTrustedGestureScope
                gestureScope(trustedGestureState_, true);
            DispatchInteractionAction(widget, drag.id, "scrollEnd",
                x, y, 0, 0, 0, false, "pointer", 0,
                std::nullopt, std::nullopt, normalizedSurface);
        }
        RuntimeInvalidateHost(widgetId);
    }
    return true;
}

bool WidgetEngine::RuntimePerformAccessibilityAction(
    const LuaWidgetAccessibilityActionRequest& request)
{
    const int index = FindWidget(request.widgetId);
    if (index < 0 || !RuntimeIsWidgetSelected(request.widgetId))
        return false;
    auto& widget = widgets_[index];
    if (!widget.valid || widget.preview || !widget.hostVisible ||
        request.nodeKey.empty())
        return false;
    WidgetSurfaceScope surfaceScope(d2dState_, "desktop");

    const auto setHostValue = [&](std::wstring value) {
        const auto control = std::find_if(widget.hostControls.rbegin(),
            widget.hostControls.rend(), [&](const auto& candidate) {
                return candidate.type ==
                        LuaWidget::HostControl::Type::Input &&
                    candidate.id == request.nodeKey && HostControlBelongsToSurface(candidate, "desktop");
            });
        if (control == widget.hostControls.rend() ||
            !snowdesktop::widget_runtime::HostInputAllowsMutation(
                control->enabled, control->readOnly) ||
            (!control->controlled && control->storageKey.empty()) ||
            !snowdesktop::widget_runtime::HostTextReplacementFits(
                {}, 0, 0, value, control->maximumUtf8Bytes))
            return false;
        if (control->numeric)
        {
            if (value.empty()) return false;
            wchar_t* end = nullptr;
            const double parsed = std::wcstod(value.c_str(), &end);
            if (!end || end != value.c_str() + value.size() ||
                !std::isfinite(parsed) || parsed < control->minimum ||
                parsed > control->maximum)
                return false;
        }

        std::wstring previous = Utf8ToWide(control->controlled
            ? control->controlledText
            : RuntimeGetStorageValue(
                request.widgetId, control->storageKey));
        if (focusedHostInput_.active &&
            focusedHostInput_.widgetId == request.widgetId &&
            focusedHostInput_.id == request.nodeKey && focusedHostInput_.surface == "desktop")
            previous = focusedHostInput_.text;
        if (previous == value) return true;

        const bool focused = focusedHostInput_.active &&
            focusedHostInput_.widgetId == request.widgetId &&
            focusedHostInput_.id == request.nodeKey && focusedHostInput_.surface == "desktop";
        if (focused)
        {
            focusedHostInput_.text = value;
            focusedHostInput_.history.Clear();
            focusedHostInput_.originalText = value;
            focusedHostInput_.cursor = value.size();
            focusedHostInput_.selectionAnchor = value.size();
            focusedHostInput_.compositionText.clear();
            focusedHostInput_.compositionCursor = 0;
            focusedHostInput_.caretVisibility.Request();
        }

        if (control->controlled)
        {
            DispatchHostInputChange(request.widgetId, request.nodeKey,
                control->changeAction, previous, value,
                control->numeric, control->minimum, control->maximum,
                true, false, "accessibility");
        }
        else
        {
            RuntimeSetStorageValue(request.widgetId,
                control->storageKey, WideToUtf8(value));
        }
        RuntimeInvalidateHost(request.widgetId);
        return true;
    };

    if (request.kind == LuaWidgetAccessibilityActionKind::SetValue)
        return setHostValue(request.textValue);
    if (request.kind == LuaWidgetAccessibilityActionKind::SetTextSelection ||
        request.kind == LuaWidgetAccessibilityActionKind::RevealTextPosition)
    {
        const auto control=std::find_if(widget.hostControls.rbegin(),widget.hostControls.rend(),[&](const auto& candidate){
            return candidate.type==LuaWidget::HostControl::Type::Input&&candidate.enabled&&candidate.id==request.nodeKey&&HostControlBelongsToSurface(candidate,"desktop");});
        if(control==widget.hostControls.rend())return false;
        if(request.kind==LuaWidgetAccessibilityActionKind::SetTextSelection)
        {
            if(!focusedHostInput_.active||focusedHostInput_.widgetId!=request.widgetId||focusedHostInput_.id!=request.nodeKey||focusedHostInput_.surface!="desktop")
                if(!RuntimeFocusHostInput(request.widgetId,request.nodeKey,"accessibility"))return false;
            if(focusedHostInput_.composing)return false;
            const auto previousAnchor=focusedHostInput_.selectionAnchor,previousCursor=focusedHostInput_.cursor;
            focusedHostInput_.selectionAnchor=text_input::SnapBoundary(focusedHostInput_.text,request.textAnchor);
            focusedHostInput_.cursor=text_input::SnapBoundary(focusedHostInput_.text,request.textCursor);
            focusedHostInput_.caretVisibility.Request();
            if(focusedHostInput_.controlledSelection)
                DispatchHostInputSelectionChange(request.widgetId,request.nodeKey,focusedHostInput_.selectionChangeAction,
                    focusedHostInput_.text,previousAnchor,previousCursor,focusedHostInput_.selectionAnchor,focusedHostInput_.cursor,"accessibility");
        }
        else if(control->multiline&&d2dState_)
        {
            const auto text=focusedHostInput_.active&&focusedHostInput_.widgetId==request.widgetId&&focusedHostInput_.id==request.nodeKey&&focusedHostInput_.surface=="desktop"
                ?focusedHostInput_.text:Utf8ToWide(control->controlled?control->controlledText:RuntimeGetStorageValue(request.widgetId,control->storageKey));
            const auto layout=CreateHostMultilineTextLayout(d2dState_,text,control->fontSize,
                std::max(1.f,static_cast<float>(control->rect.right-control->rect.left)-control->padding.left-control->padding.right-8.f));
            if(!layout)return false;float x=0,y=0;DWRITE_HIT_TEST_METRICS hit{};
            if(FAILED(layout->HitTestTextPosition(static_cast<UINT32>(std::min(request.textCursor,text.size())),FALSE,&x,&y,&hit)))return false;
            RuntimeSetScrollOffset(request.widgetId,request.nodeKey,static_cast<int>(std::lround(y)));
        }
        RuntimeInvalidateHost(request.widgetId);return true;
    }
    if (request.kind ==
        LuaWidgetAccessibilityActionKind::SetScrollOffset)
    {
        if (!std::isfinite(request.numericValue) ||
            request.numericValue < 0.0 ||
            request.numericValue > 1'000'000.0)
            return false;
        const auto scroll = std::find_if(widget.hostControls.rbegin(),
            widget.hostControls.rend(), [&](const auto& control) {
                return control.type == LuaWidget::HostControl::Type::Scroll &&
                    control.enabled && control.id == request.nodeKey && HostControlBelongsToSurface(control, "desktop");
            });
        if (scroll == widget.hostControls.rend()) return false;
        const int maximum = scroll->horizontal
            ? std::max(0, scroll->contentWidth - scroll->viewportWidth)
            : std::max(0, scroll->contentHeight - scroll->viewportHeight);
        const int previousOffset = RuntimeGetScrollOffset(
            request.widgetId, request.nodeKey);
        RuntimeSetScrollOffset(request.widgetId, request.nodeKey,
            static_cast<int>(std::lround(request.numericValue)));
        const int currentOffset = RuntimeGetScrollOffset(
            request.widgetId, request.nodeKey);
        if (snowdesktop::widget_scroll_rules::ReachedScrollEnd(
                previousOffset, currentOffset, maximum))
            DispatchInteractionAction(widget, request.nodeKey, "scrollEnd",
                0, 0, 0, 0, 0, false, "accessibility");
        RuntimeInvalidateHost(request.widgetId);
        return true;
    }
    if (request.kind ==
        LuaWidgetAccessibilityActionKind::SetRangeValue)
    {
        if (!std::isfinite(request.numericValue)) return false;
        const auto input = std::find_if(widget.hostControls.rbegin(),
            widget.hostControls.rend(), [&](const auto& control) {
                return control.type ==
                        LuaWidget::HostControl::Type::Input &&
                    control.numeric && control.id == request.nodeKey && HostControlBelongsToSurface(control, "desktop");
            });
        if (input != widget.hostControls.rend())
        {
            char buffer[64]{};
            const auto converted = std::to_chars(buffer,
                buffer + sizeof(buffer), request.numericValue,
                std::chars_format::general,
                std::numeric_limits<double>::max_digits10);
            if (converted.ec != std::errc{}) return false;
            return setHostValue(Utf8ToWide(std::string(
                buffer, converted.ptr)));
        }
    }

    const auto* region =
        widget.interactionRegions.Find(request.nodeKey);
    if (!region || !region->enabled) return false;
    const auto& shape = region->shape;
    const int x = static_cast<int>(std::lround(
        shape.type == snowdesktop::widget_runtime::
                InteractionShapeType::Circle
            ? shape.x : shape.x + shape.width * 0.5f));
    const int y = static_cast<int>(std::lround(
        shape.type == snowdesktop::widget_runtime::
                InteractionShapeType::Circle
            ? shape.y : shape.y + shape.height * 0.5f));

    if (request.kind ==
        LuaWidgetAccessibilityActionKind::SetRangeValue)
    {
        const float value = static_cast<float>(request.numericValue);
        if (!widget.interactionRegions.ResolveRangeValue(
                request.nodeKey, value))
            return false;
        DispatchInteractionAction(widget, request.nodeKey, "change",
            x, y, 0, 0, 0, false, "accessibility", 0, value);
        RuntimeInvalidateHost(request.widgetId);
        return true;
    }

    const char* eventName = "click";
    if (request.kind == LuaWidgetAccessibilityActionKind::Select ||
        request.kind == LuaWidgetAccessibilityActionKind::AddToSelection ||
        request.kind ==
            LuaWidgetAccessibilityActionKind::RemoveFromSelection)
    {
        const bool single = region->controlKind ==
            snowdesktop::widget_runtime::
                InteractionControlKind::SelectionSingle;
        const bool multiple = region->controlKind ==
            snowdesktop::widget_runtime::
                InteractionControlKind::SelectionMultiple;
        if (single || multiple)
        {
            if (!widget.interactionRegions.FindAction(
                    request.nodeKey, "change"))
                return false;
            std::vector<std::string> selectedKeys =
                region->currentSelectedKeys;
            const auto selected = std::find(selectedKeys.begin(),
                selectedKeys.end(), region->proposedSelectedKey);
            if (request.kind == LuaWidgetAccessibilityActionKind::Select)
                selectedKeys = { region->proposedSelectedKey };
            else if (!multiple)
                return false;
            else if (request.kind ==
                LuaWidgetAccessibilityActionKind::AddToSelection)
            {
                if (selected == selectedKeys.end())
                    selectedKeys.push_back(region->proposedSelectedKey);
            }
            else if (selected != selectedKeys.end())
                selectedKeys.erase(selected);
            if (selectedKeys == region->currentSelectedKeys) return true;
            DispatchInteractionAction(widget, request.nodeKey, "change",
                x, y, 0, 0, 0, false, "accessibility", 0,
                std::nullopt, std::move(selectedKeys));
            RuntimeInvalidateHost(request.widgetId);
            return true;
        }
        if (request.kind != LuaWidgetAccessibilityActionKind::Select)
            return false;
    }
    if (request.kind == LuaWidgetAccessibilityActionKind::Toggle)
    {
        if ((region->controlKind != snowdesktop::widget_runtime::
                    InteractionControlKind::Toggle &&
                region->controlKind != snowdesktop::widget_runtime::
                    InteractionControlKind::Checkbox) ||
            !widget.interactionRegions.FindAction(
                request.nodeKey, "change"))
            return false;
    }
    else if (request.kind == LuaWidgetAccessibilityActionKind::Select)
    {
        if (region->controlKind == snowdesktop::widget_runtime::
                InteractionControlKind::Radio)
        {
            if (!widget.interactionRegions.FindAction(
                    request.nodeKey, "change"))
                return false;
        }
        else if (!widget.interactionRegions.FindAction(
                     request.nodeKey, "click"))
            return false;
    }
    else if (request.kind == LuaWidgetAccessibilityActionKind::Expand ||
        request.kind == LuaWidgetAccessibilityActionKind::Collapse)
    {
        const bool expand = request.kind ==
            LuaWidgetAccessibilityActionKind::Expand;
        if (!region->hasExpandedProposal ||
            !widget.interactionRegions.FindAction(
                request.nodeKey, "click"))
            return false;
        if (region->expanded == expand) return true;
    }
    else if (request.kind != LuaWidgetAccessibilityActionKind::Invoke ||
        !widget.interactionRegions.FindAction(
            request.nodeKey, "click"))
        return false;

    DispatchInteractionAction(widget, request.nodeKey, eventName,
        x, y, 0, 0, 1, false, "accessibility");
    RuntimeInvalidateHost(request.widgetId);
    return true;
}

std::wstring WidgetEngine::RuntimeSelectedWidgetPackageId() const
{
    return selectedWidgetPackageProvider_
        ? selectedWidgetPackageProvider_()
        : std::wstring{};
}

bool WidgetEngine::HasFocusedHostInput() const
{
    return focusedHostInput_.active;
}

bool WidgetEngine::IsHostInputAt(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const POINT point{ x, y };
    for (const auto& control : widgets_[index].hostControls)
        if (control.selectPopup && control.enabled &&
            HostControlBelongsToSurface(control, surface) &&
            HostControlContainsPoint(control, point))
            return false;
    for (auto it = widgets_[index].hostControls.rbegin();
        it != widgets_[index].hostControls.rend(); ++it)
    {
        if (it->type == LuaWidget::HostControl::Type::Input &&
            it->enabled && HostControlBelongsToSurface(*it, surface) &&
            HostControlContainsPoint(*it, point))
            return true;
    }
    return false;
}

const LuaWidget::HostControl* WidgetEngine::HitTestHostInputTextDrop(
    const LuaWidget& widget, POINT point, std::string_view surface) const
{
    const bool visible = IsPanelSurface(surface)
        ? widget.panelActive && widget.panelSurface == surface : widget.desktopVisible;
    if (!widget.valid || !visible || widget.preview)
        return nullptr;
    for (const auto& control : widget.hostControls)
        if (control.selectPopup && control.enabled &&
            HostControlBelongsToSurface(control, surface) &&
            HostControlContainsPoint(control, point))
            return nullptr;
    for (auto it = widget.hostControls.rbegin(); it != widget.hostControls.rend(); ++it)
        if (it->type == LuaWidget::HostControl::Type::Input &&
            HostControlBelongsToSurface(*it, surface) &&
            HostControlContainsPoint(*it, point))
            return &*it;
    return nullptr;
}

bool WidgetEngine::HostInputAcceptsTextDrop(
    const LuaWidget::HostControl& control) const
{
    return d2dState_ && !IsHostInputComposing() && control.focusable &&
        snowdesktop::widget_runtime::HostInputAllowsMutation(
            control.enabled, control.readOnly) &&
        (control.controlled || !control.storageKey.empty());
}

std::wstring WidgetEngine::HostInputTextForDrop(
    const LuaWidget& widget, const LuaWidget::HostControl& control) const
{
    if (focusedHostInput_.active && focusedHostInput_.surface == control.surface &&
        focusedHostInput_.widgetId == widget.widgetId &&
        focusedHostInput_.id == control.id)
        return focusedHostInput_.text;
    return Utf8ToWide(control.controlled ? control.controlledText :
        RuntimeGetStorageValue(widget.widgetId, control.storageKey));
}

std::optional<std::string> WidgetEngine::FindHostInputTextDropTarget(
    const std::wstring& widgetId, POINT point, std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return std::nullopt;
    const auto* control = HitTestHostInputTextDrop(widgets_[index], point, surface);
    if (!control) return std::nullopt;
    return HostInputAcceptsTextDrop(*control) ? control->id : std::string{};
}

std::optional<size_t> WidgetEngine::ResolveHostInputTextDropPosition(
    const LuaWidget& widget, const LuaWidget::HostControl& control,
    POINT point, const std::wstring& text, HostInputDropCaret* caret,
    std::optional<size_t>* hitCharacter) const
{
    using namespace snowdesktop::widget_runtime;
    ViewRect frame{static_cast<float>(control.rect.left),
        static_cast<float>(control.rect.top),
        static_cast<float>(control.rect.right - control.rect.left),
        static_cast<float>(control.rect.bottom - control.rect.top)};
    ViewRect content{frame.x + control.padding.left,
        frame.y + (control.multiline ? control.padding.top : 0.0f),
        frame.width - control.padding.left - control.padding.right,
        frame.height - (control.multiline
            ? control.padding.top + control.padding.bottom : 0.0f)};
    D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Identity();
    const auto* tree = DeclarativeViewForSurface(widget, control.surface);
    const auto* node = tree ? FindViewNodeByKey(*tree, control.id) : nullptr;
    if (node && IsDeclarativeInputNode(node->type))
    {
        frame = node->frame;
        content = ViewNodeContentRect(*node);
        const auto t = ResolveViewTransformForKey(*tree, control.id);
        transform = D2D1::Matrix3x2F(t.m11, t.m12, t.m21, t.m22, t.dx, t.dy);
    }
    else
        node = nullptr;

    auto inverse = transform;
    if (!inverse.Invert() || content.width <= 0.0f || content.height <= 0.0f)
        return std::nullopt;
    const auto local = inverse.TransformPoint(D2D1::Point2F(
        static_cast<float>(point.x), static_cast<float>(point.y)));
    if (local.x < frame.x || local.x >= frame.x + frame.width ||
        local.y < frame.y || local.y >= frame.y + frame.height)
        return std::nullopt;

    const float fontSize = node ? node->fontSize : control.fontSize;
    const float innerWidth = std::max(1.0f,
        content.width - (control.multiline ? 8.0f : 0.0f));
    auto layout = control.multiline
        ? CreateHostMultilineTextLayout(d2dState_, text, fontSize, innerWidth)
        : CreateHostSingleLineTextLayout(d2dState_, text, fontSize, innerWidth,
            content.height);
    if (!layout) return std::nullopt;
    if (node)
    {
        ApplyViewTextLocaleAndDirection(layout.Get(), *node, text);
        SetViewTextLayoutAlignment(layout.Get(), node->textAlign);
    }
    const float scaleY = std::hypot(transform._21, transform._22);
    const float scrollOffset = control.multiline && scaleY > 0.0f
        ? static_cast<float>(RuntimeGetScrollOffset(
            widget.widgetId, control.id, control.surface)) / scaleY : 0.0f;
    const float originY = content.y - scrollOffset;
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS hit{};
    if (FAILED(layout->HitTestPoint(local.x - content.x, local.y - originY,
            &trailing, &inside, &hit)))
        return std::nullopt;

    const size_t rawPosition = static_cast<size_t>(hit.textPosition) +
        (trailing ? static_cast<size_t>(hit.length) : 0);
    if (hitCharacter && inside && hit.isText && hit.textPosition < text.size())
        *hitCharacter = hit.textPosition;
    const size_t position = text_input::SnapBoundary(text, rawPosition);
    if (!caret) return position;

    // Keep DirectWrite's edge at a bidi boundary. For an empty field or a
    // snapped grapheme, hit the actual insertion position instead.
    const bool useHitEdge = position == rawPosition && !text.empty();
    float x = 0.0f;
    float y = 0.0f;
    if (FAILED(layout->HitTestTextPosition(
            useHitEdge ? hit.textPosition : static_cast<UINT32>(position),
            useHitEdge && trailing, &x, &y, &hit)))
        return std::nullopt;
    x = std::clamp(content.x + x, content.x, content.x + content.width);
    const float top = std::max(content.y, originY + y);
    const float bottom = std::min(content.y + content.height,
        originY + y + std::max(hit.height, fontSize));
    const auto first = transform.TransformPoint(D2D1::Point2F(x, top));
    const auto last = transform.TransformPoint(D2D1::Point2F(x, std::max(top, bottom)));
    RECT clip = control.rect;
    if (control.clipRect)
        IntersectRect(&clip, &clip, &*control.clipRect);
    *caret = {first.x, first.y, last.x, last.y, clip};
    return position;
}

std::optional<bool> WidgetEngine::UpdateHostInputTextDropPreview(
    const std::wstring& widgetId, POINT point, std::string_view surface,
    bool copyAllowed, const text_input::DragSelection* move)
{
    const int index = FindWidget(widgetId);
    const auto* control = index >= 0
        ? HitTestHostInputTextDrop(widgets_[index], point, surface) : nullptr;
    if (!control || !copyAllowed || !HostInputAcceptsTextDrop(*control))
    {
        ClearHostInputTextDropPreview();
        return control ? std::optional<bool>(false) : std::nullopt;
    }

    const auto& widget = widgets_[index];
    HostInputDropCaret caret{};
    const auto text = HostInputTextForDrop(widget, *control);
    const auto position = ResolveHostInputTextDropPosition(
        widget, *control, point, text, &caret);
    if (!position || (move && !move->CanMove(text, *position)))
    {
        ClearHostInputTextDropPreview();
        return false;
    }
    if (hostInputDropWidgetId_ != widgetId || hostInputDropSurface_ != surface)
    {
        ClearHostInputTextDropPreview();
        hostInputDropWidgetId_ = widgetId;
        hostInputDropSurface_ = surface;
    }
    if (hostInputDropCaret_ != caret)
    {
        hostInputDropCaret_ = caret;
        RuntimeInvalidateHost(widgetId, std::nullopt, surface);
    }
    return true;
}

bool WidgetEngine::ClearHostInputTextDropPreview()
{
    if (!hostInputDropCaret_) return false;
    hostInputDropCaret_.reset();
    const auto widgetId = std::exchange(hostInputDropWidgetId_, {});
    const auto surface = std::exchange(hostInputDropSurface_, {});
    RuntimeInvalidateHost(widgetId, std::nullopt, surface);
    return true;
}

bool WidgetEngine::DropHostInputText(const std::wstring& widgetId,
    std::string_view target, POINT point, std::string_view surface,
    std::wstring_view text, const text_input::DragSelection* move)
{
    using snowdesktop::widget_runtime::TryApplyHostTextReplacement;
    int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto* control = HitTestHostInputTextDrop(widgets_[index], point, surface);
    if (!control || control->id != target || !HostInputAcceptsTextDrop(*control))
        return false;

    const std::string id = control->id;
    const auto normalized = move ? std::wstring{} : text_input::NormalizeTransfer(
        text, control->multiline, control->maximumUtf8Bytes);
    if (!move && normalized.empty()) return false;
    const auto nextText = HostInputTextForDrop(widgets_[index], *control);
    const auto position = ResolveHostInputTextDropPosition(
        widgets_[index], *control, point, nextText, nullptr);
    if (!position || (move ? !move->CanMove(nextText, *position) :
            !snowdesktop::widget_runtime::HostTextReplacementFits(
                nextText, *position, *position, normalized, control->maximumUtf8Bytes)))
        return false;

    const std::string surfaceName(surface);
    WidgetSurfaceScope surfaceScope(d2dState_, surfaceName.c_str());
    if (!RuntimeFocusHostInput(widgetId, id, "pointer") ||
        !focusedHostInput_.active || focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.id != id || focusedHostInput_.surface != surface)
        return false;

    // Focus/blur actions can change the control or the value. Recheck both
    // before using the insertion offset from the visible text.
    index = FindWidget(widgetId);
    if (index < 0) return false;
    control = HitTestHostInputTextDrop(widgets_[index], point, surface);
    if (!control || control->id != id || !HostInputAcceptsTextDrop(*control) ||
        focusedHostInput_.readOnly)
        return false;
    const auto updatedPosition = ResolveHostInputTextDropPosition(
        widgets_[index], *control, point, focusedHostInput_.text, nullptr);
    if (!updatedPosition) return false;
    const text_input::Snapshot before{
        focusedHostInput_.text, focusedHostInput_.cursor, focusedHostInput_.selectionAnchor};
    size_t nextCursor = 0;
    if (move)
    {
        const auto moved = move->Move(focusedHostInput_.text, *updatedPosition);
        if (!moved) return false;
        nextCursor = *moved;
    }
    else if (!TryApplyHostTextReplacement(focusedHostInput_.text, *updatedPosition,
            *updatedPosition, normalized, focusedHostInput_.maximumUtf8Bytes, nextCursor))
        return false;
    focusedHostInput_.cursor = focusedHostInput_.selectionAnchor = nextCursor;
    focusedHostInput_.pointerSelecting = false;
    focusedHostInput_.selectionDragArmed = false;
    focusedHostInput_.pendingHighSurrogate = 0;
    focusedHostInput_.duplicateImeResult.clear();
    FinishHostInputEdit(before, "pointer");
    return true;
}

std::optional<text_input::DragSelection> WidgetEngine::TakeHostInputTextDrag(
    std::wstring& widgetId, std::string& controlId, std::string& surface)
{
    if (!focusedHostInput_.active || !focusedHostInput_.pointerSelecting ||
        !std::exchange(focusedHostInput_.selectionDragArmed, false) ||
        focusedHostInput_.composing)
        return {};
    const auto start = std::min(focusedHostInput_.cursor, focusedHostInput_.selectionAnchor);
    const auto end = std::max(focusedHostInput_.cursor, focusedHostInput_.selectionAnchor);
    if (start == end || end > focusedHostInput_.text.size())
        return {};
    widgetId = focusedHostInput_.widgetId;
    controlId = focusedHostInput_.id;
    surface = focusedHostInput_.surface;
    focusedHostInput_.pointerSelecting = false;
    return text_input::DragSelection{focusedHostInput_.text, start, end};
}

bool WidgetEngine::PrepareHostInputContextMenu(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface, bool clipboardHasText,
    snowdesktop::widget_runtime::HostInputContextMenuState& state)
{
    state = {};
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;

    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);
    auto& controls = widgets_[index].hostControls;
    const POINT point{ x, y };
    auto target = std::find_if(
        controls.rbegin(), controls.rend(), [&](const auto& control) {
            return control.type == LuaWidget::HostControl::Type::Input &&
                control.enabled &&
                HostControlBelongsToSurface(control, normalizedSurface) &&
                HostControlContainsPoint(control, point);
        });
    if (target == controls.rend())
        return false;

    const std::string targetId = target->id;
    const bool alreadyFocused = focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == targetId &&
        focusedHostInput_.surface == normalizedSurface;
    WidgetSurfaceScope surfaceScope(
        d2dState_, normalizedSurface.c_str());
    if (!RuntimeFocusHostInput(widgetId, targetId, "pointer"))
        return false;

    target = std::find_if(
        controls.rbegin(), controls.rend(), [&](const auto& control) {
            return control.type == LuaWidget::HostControl::Type::Input &&
                control.id == targetId &&
                HostControlBelongsToSurface(control, normalizedSurface);
        });
    if (target == controls.rend())
        return false;

    focusedHostInput_.pointerSelecting = false;
    if (!alreadyFocused)
    {
        const size_t previousCursor = focusedHostInput_.cursor;
        const size_t previousAnchor =
            focusedHostInput_.selectionAnchor;
        focusedHostInput_.cursor = HitTestHostInputPosition(
            *target, widgetId, x, y);
        focusedHostInput_.selectionAnchor =
            focusedHostInput_.cursor;
        focusedHostInput_.caretVisibility.Request();
        if (focusedHostInput_.controlledSelection &&
            (previousCursor != focusedHostInput_.cursor ||
                previousAnchor != focusedHostInput_.selectionAnchor))
        {
            DispatchHostInputSelectionChange(
                focusedHostInput_.widgetId,
                focusedHostInput_.id,
                focusedHostInput_.selectionChangeAction,
                focusedHostInput_.text,
                previousAnchor, previousCursor,
                focusedHostInput_.selectionAnchor,
                focusedHostInput_.cursor,
                "contextMenu");
        }
    }

    state = snowdesktop::widget_runtime::
        ResolveHostInputContextMenuState(
            focusedHostInput_.text.size(),
            focusedHostInput_.cursor,
            focusedHostInput_.selectionAnchor,
            target->enabled,
            focusedHostInput_.readOnly,
            clipboardHasText);
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::GetFocusedHostInputCaretRect(RECT& rect) const
{
    rect = {};
    if (!focusedHostInput_.active || !d2dState_)
        return false;

    const int index = FindWidget(focusedHostInput_.widgetId);
    if (index < 0)
        return false;
    const LuaWidget& widget = widgets_[index];
    const auto control = std::find_if(
        widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& item) {
            return item.type == LuaWidget::HostControl::Type::Input &&
                item.id == focusedHostInput_.id &&
                HostControlBelongsToSurface(
                    item, focusedHostInput_.surface);
        });
    if (control == widget.hostControls.rend())
        return false;
    const HostInputDisplayText display =
        BuildHostInputDisplayText(focusedHostInput_.text,
            focusedHostInput_.cursor,
            focusedHostInput_.selectionAnchor,
            focusedHostInput_.compositionText,
            focusedHostInput_.compositionCursor);

    const float width = static_cast<float>(
        control->rect.right - control->rect.left);
    const float height = static_cast<float>(
        control->rect.bottom - control->rect.top);
    ComPtr<IDWriteTextLayout> layout;
    int scrollOffset = 0;
    if (control->multiline)
    {
        constexpr float scrollbarReserve = 8.0f;
        const float innerWidth = std::max(1.0f,
            width - control->padding.left - control->padding.right -
                scrollbarReserve);
        layout = CreateHostMultilineTextLayout(d2dState_,
            display.text, control->fontSize,
            innerWidth);
        scrollOffset = RuntimeGetScrollOffset(
            focusedHostInput_.widgetId, control->id,
            focusedHostInput_.surface);
    }
    else
    {
        const float innerWidth = std::max(1.0f,
            width - control->padding.left - control->padding.right);
        layout = CreateHostSingleLineTextLayout(d2dState_,
            display.text, control->fontSize,
            innerWidth, std::max(1.0f,
                height - control->padding.top - control->padding.bottom));
    }
    if (!layout)
        return false;

    const size_t safeCursor = std::min(
        display.cursor, display.text.size());
    UINT32 hitPosition = 0;
    BOOL trailing = FALSE;
    if (!display.text.empty())
    {
        if (control->multiline &&
            safeCursor >= display.text.size() &&
            (display.text.back() == L'\n' ||
                display.text.back() == L'\r'))
        {
            hitPosition = static_cast<UINT32>(
                display.text.size());
        }
        else if (safeCursor >= display.text.size())
        {
            hitPosition = static_cast<UINT32>(
                display.text.size() - 1);
            trailing = TRUE;
        }
        else
        {
            hitPosition = static_cast<UINT32>(safeCursor);
        }
    }

    float caretX = 0.0f;
    float caretY = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestTextPosition(
            hitPosition, trailing, &caretX, &caretY,
            &metrics)))
        return false;

    const float localX = static_cast<float>(control->rect.left) +
        control->padding.left + caretX;
    const float localY = static_cast<float>(control->rect.top) +
        control->padding.top +
        caretY - static_cast<float>(scrollOffset);
    const float caretHeight = std::max(
        metrics.height, control->fontSize);
    const RECT surfaceBounds = BoundsForSurface(
        widget, focusedHostInput_.surface);
    rect.left = surfaceBounds.left +
        static_cast<LONG>(std::lround(localX));
    rect.top = surfaceBounds.top +
        static_cast<LONG>(std::lround(localY));
    rect.right = rect.left + 1;
    rect.bottom = rect.top +
        std::max<LONG>(1,
            static_cast<LONG>(std::lround(caretHeight)));
    return true;
}

void WidgetEngine::BlurHostInput(bool cancel, bool preserveViewFocus)
{
    if (!focusedHostInput_.active) return;

    const std::wstring widgetId = focusedHostInput_.widgetId;
    const std::string inputId = focusedHostInput_.id;
    const std::string inputSurface = focusedHostInput_.surface;
    if (focusedHostInput_.controlled)
    {
        if (cancel && focusedHostInput_.liveUpdate &&
            focusedHostInput_.text != focusedHostInput_.originalText)
        {
            DispatchHostInputChange(widgetId, focusedHostInput_.id,
                focusedHostInput_.changeAction, focusedHostInput_.text,
                focusedHostInput_.originalText,
                focusedHostInput_.numeric, focusedHostInput_.minimum,
                focusedHostInput_.maximum, true, true, "keyboard");
        }
        else if (!cancel && !focusedHostInput_.liveUpdate &&
            focusedHostInput_.text != focusedHostInput_.originalText)
        {
            DispatchHostInputChange(widgetId, focusedHostInput_.id,
                focusedHostInput_.changeAction,
                focusedHostInput_.originalText, focusedHostInput_.text,
                focusedHostInput_.numeric, focusedHostInput_.minimum,
                focusedHostInput_.maximum, true, false, "commit");
        }
    }
    else if (cancel)
    {
        if (focusedHostInput_.liveUpdate)
            RuntimeSetStorageValue(widgetId, focusedHostInput_.storageKey,
                WideToUtf8(focusedHostInput_.originalText));
    }
    else
    {
        RuntimeSetStorageValue(widgetId, focusedHostInput_.storageKey,
            WideToUtf8(focusedHostInput_.text));
    }
    DispatchHostInputAction(widgetId, focusedHostInput_.id,
        focusedHostInput_.blurAction, "blur",
        cancel ? focusedHostInput_.originalText : focusedHostInput_.text,
        cancel, "keyboard");
    if (const int index = FindWidget(widgetId); index >= 0 &&
        ViewFocusForSurface(widgets_[index], inputSurface) == inputId)
    {
        if (!preserveViewFocus) ViewFocusForSurface(widgets_[index], inputSurface).clear();
        ViewFocusCueForSurface(widgets_[index], inputSurface) = preserveViewFocus;
    }
    focusedHostInput_ = {};
    RuntimeInvalidateHost(widgetId);
}

bool WidgetEngine::SetHostInputComposition(
    const std::wstring& text, size_t cursor)
{
    if (!focusedHostInput_.active)
        return false;
    if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
            true, focusedHostInput_.readOnly))
    {
        focusedHostInput_.compositionText.clear();
        focusedHostInput_.compositionCursor = 0;
        return true;
    }
    focusedHostInput_.pendingHighSurrogate = 0;
    focusedHostInput_.composing = true;
    const size_t boundedCursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    const size_t boundedAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());
    const size_t selectionStart = std::min(
        boundedCursor, boundedAnchor);
    const size_t selectionEnd = std::max(
        boundedCursor, boundedAnchor);
    if (!snowdesktop::widget_runtime::HostTextReplacementFits(
            focusedHostInput_.text, selectionStart, selectionEnd,
            text, focusedHostInput_.maximumUtf8Bytes))
    {
        focusedHostInput_.compositionText.clear();
        focusedHostInput_.compositionCursor = 0;
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    }
    focusedHostInput_.compositionText = text;
    focusedHostInput_.compositionCursor =
        std::min(cursor, text.size());
    focusedHostInput_.caretVisibility.Request();
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
    return true;
}

bool WidgetEngine::CommitHostInputComposition(
    const std::wstring& text)
{
    if (!focusedHostInput_.active)
        return false;
    if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
            true, focusedHostInput_.readOnly))
    {
        focusedHostInput_.compositionText.clear();
        focusedHostInput_.compositionCursor = 0;
        return true;
    }
    focusedHostInput_.pendingHighSurrogate = 0;

    const snowdesktop::text_input::Snapshot before{
        focusedHostInput_.text, focusedHostInput_.cursor,
        focusedHostInput_.selectionAnchor};

    const std::wstring previousText = focusedHostInput_.text;
    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.selectionAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());
    const size_t selectionStart = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.cursor);
    const size_t selectionEnd = std::max(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.cursor);
    size_t nextCursor = focusedHostInput_.cursor;
    if (!snowdesktop::widget_runtime::TryApplyHostTextReplacement(
            focusedHostInput_.text, selectionStart, selectionEnd,
            text, focusedHostInput_.maximumUtf8Bytes, nextCursor))
    {
        focusedHostInput_.compositionText.clear();
        focusedHostInput_.compositionCursor = 0;
        focusedHostInput_.caretVisibility.Request();
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    }
    focusedHostInput_.cursor = nextCursor;
    focusedHostInput_.selectionAnchor =
        focusedHostInput_.cursor;
    focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor = 0;
    focusedHostInput_.caretVisibility.Request();
    focusedHostInput_.history.Record(before, {focusedHostInput_.text,
        focusedHostInput_.cursor, focusedHostInput_.selectionAnchor});
    focusedHostInput_.duplicateImeResult = text;
    if (focusedHostInput_.liveUpdate)
    {
        if (focusedHostInput_.controlled)
            DispatchHostInputChange(focusedHostInput_.widgetId,
                focusedHostInput_.id, focusedHostInput_.changeAction,
                previousText, focusedHostInput_.text,
                focusedHostInput_.numeric, focusedHostInput_.minimum,
                focusedHostInput_.maximum, false, false, "ime");
        else
            RuntimeSetStorageValue(focusedHostInput_.widgetId,
                focusedHostInput_.storageKey,
                WideToUtf8(focusedHostInput_.text));
    }
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
    return true;
}

void WidgetEngine::ClearHostInputComposition()
{
    if (!focusedHostInput_.active) return;
    focusedHostInput_.composing = false;
    focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor = 0;
    if(focusedHostInput_.deferredModelText)
    {
        focusedHostInput_.text=std::move(*focusedHostInput_.deferredModelText);focusedHostInput_.deferredModelText.reset();
        focusedHostInput_.originalText=focusedHostInput_.text;
        focusedHostInput_.cursor=text_input::SnapBoundary(focusedHostInput_.text,focusedHostInput_.cursor);
        focusedHostInput_.selectionAnchor=text_input::SnapBoundary(focusedHostInput_.text,focusedHostInput_.selectionAnchor);
        focusedHostInput_.history.Clear();
    }
    if(focusedHostInput_.deferredSelection&&focusedHostInput_.controlledSelection)
    {
        const auto anchor=snowdesktop::widget_runtime::HostTextOffsetFromUtf8ByteOffset(focusedHostInput_.text,focusedHostInput_.controlledSelection->start);
        const auto cursor=snowdesktop::widget_runtime::HostTextOffsetFromUtf8ByteOffset(focusedHostInput_.text,focusedHostInput_.controlledSelection->finish);
        if(anchor&&cursor){focusedHostInput_.selectionAnchor=*anchor;focusedHostInput_.cursor=*cursor;}
    }
    focusedHostInput_.deferredSelection=false;
    focusedHostInput_.caretVisibility.Request();
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
}
void WidgetEngine::BeginHostInputComposition()
{
    if(!focusedHostInput_.active)return;
    focusedHostInput_.composing=true;focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor=0;focusedHostInput_.duplicateImeResult.clear();
}

bool WidgetEngine::HandleHostInputChar(wchar_t ch)
{
    if (!focusedHostInput_.active || ch < 0x20 || ch == 0x7F)
        return false;
    if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
            true, focusedHostInput_.readOnly)) return true;
    if(!focusedHostInput_.duplicateImeResult.empty()&&focusedHostInput_.duplicateImeResult.front()==ch)
    {focusedHostInput_.duplicateImeResult.erase(0,1);return true;}
    focusedHostInput_.duplicateImeResult.clear();
    const snowdesktop::text_input::Snapshot before{
        focusedHostInput_.text, focusedHostInput_.cursor, focusedHostInput_.selectionAnchor};
    if (ch >= 0xD800 && ch <= 0xDBFF)
    {
        focusedHostInput_.pendingHighSurrogate = ch;
        return true;
    }
    std::wstring replacement;
    if (ch >= 0xDC00 && ch <= 0xDFFF &&
        focusedHostInput_.pendingHighSurrogate != 0)
    {
        replacement.push_back(
            focusedHostInput_.pendingHighSurrogate);
        replacement.push_back(ch);
    }
    else
    {
        replacement.push_back(ch);
    }
    focusedHostInput_.pendingHighSurrogate = 0;
    focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor = 0;

    const std::wstring previousText = focusedHostInput_.text;
    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.selectionAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());
    const size_t selectionStart = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.cursor);
    const size_t selectionEnd = std::max(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.cursor);
    size_t nextCursor = focusedHostInput_.cursor;
    if (!snowdesktop::widget_runtime::TryApplyHostTextReplacement(
            focusedHostInput_.text, selectionStart, selectionEnd,
            replacement,
            focusedHostInput_.maximumUtf8Bytes, nextCursor))
    {
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    }
    focusedHostInput_.cursor = nextCursor;
    focusedHostInput_.selectionAnchor =
        focusedHostInput_.cursor;
    focusedHostInput_.caretVisibility.Request();
    focusedHostInput_.history.Record(before, {focusedHostInput_.text,
        focusedHostInput_.cursor, focusedHostInput_.selectionAnchor});
    if (focusedHostInput_.liveUpdate)
    {
        if (focusedHostInput_.controlled)
            DispatchHostInputChange(focusedHostInput_.widgetId,
                focusedHostInput_.id, focusedHostInput_.changeAction,
                previousText, focusedHostInput_.text,
                focusedHostInput_.numeric, focusedHostInput_.minimum,
                focusedHostInput_.maximum, false, false, "keyboard");
        else
            RuntimeSetStorageValue(focusedHostInput_.widgetId,
                focusedHostInput_.storageKey,
                WideToUtf8(focusedHostInput_.text));
    }
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
    return true;
}

bool WidgetEngine::ExecuteHostInputEditCommand(
    snowdesktop::widget_runtime::HostInputEditCommand command,
    const char* source)
{
    using snowdesktop::widget_runtime::HostInputEditCommand;
    if (!focusedHostInput_.active)
        return false;

    const char* eventSource = source && *source
        ? source : "contextMenu";
    focusedHostInput_.pendingHighSurrogate = 0;
    focusedHostInput_.pointerSelecting = false;
    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.selectionAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());

    const std::wstring previousText = focusedHostInput_.text;
    const size_t previousCursor = focusedHostInput_.cursor;
    const size_t previousSelectionAnchor =
        focusedHostInput_.selectionAnchor;
    const auto hasSelection = [&]() {
        return focusedHostInput_.selectionAnchor !=
            focusedHostInput_.cursor;
    };
    const auto selectionStart = [&]() {
        return std::min(focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor);
    };
    const auto selectionEnd = [&]() {
        return std::max(focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor);
    };
    const auto copySelection = [&]() {
        if (!hasSelection() || !OpenClipboard(nullptr))
            return false;
        bool copied = false;
        EmptyClipboard();
        const std::wstring selectedText =
            focusedHostInput_.text.substr(
                selectionStart(),
                selectionEnd() - selectionStart());
        const SIZE_T bytes =
            (selectedText.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory)
        {
            if (void* target = GlobalLock(memory))
            {
                memcpy(target, selectedText.c_str(), bytes);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory))
                    copied = true;
                else
                    GlobalFree(memory);
            }
            else
                GlobalFree(memory);
        }
        CloseClipboard();
        return copied;
    };
    const auto dispatchSelectionChange = [&]() {
        if (!focusedHostInput_.controlledSelection)
            return;
        DispatchHostInputSelectionChange(
            focusedHostInput_.widgetId,
            focusedHostInput_.id,
            focusedHostInput_.selectionChangeAction,
            focusedHostInput_.text,
            previousSelectionAnchor, previousCursor,
            focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor, eventSource);
    };

    bool changed = false;
    switch (command)
    {
    case HostInputEditCommand::SelectAll:
        focusedHostInput_.selectionAnchor = 0;
        focusedHostInput_.cursor = focusedHostInput_.text.size();
        focusedHostInput_.caretVisibility.Request();
        dispatchSelectionChange();
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;

    case HostInputEditCommand::Copy:
        (void)copySelection();
        return true;

    case HostInputEditCommand::Cut:
        if (snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly) &&
            copySelection())
        {
            const size_t start = selectionStart();
            const size_t end = selectionEnd();
            focusedHostInput_.text.erase(start, end - start);
            focusedHostInput_.cursor = start;
            focusedHostInput_.selectionAnchor = start;
            changed = true;
        }
        break;

    case HostInputEditCommand::Paste:
    {
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly))
            return true;
        std::wstring pasted;
        if (OpenClipboard(nullptr))
        {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
            {
                if (const wchar_t* clipboardText =
                        static_cast<const wchar_t*>(GlobalLock(data)))
                {
                    const auto* end = clipboardText + GlobalSize(data) / sizeof(wchar_t);
                    const auto* terminator = std::find(clipboardText, end, L'\0');
                    if (terminator != end)
                        pasted = text_input::NormalizeTransfer(
                            std::wstring_view(clipboardText,
                                static_cast<size_t>(terminator - clipboardText)),
                            focusedHostInput_.multiline,
                            focusedHostInput_.maximumUtf8Bytes);
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        if (!pasted.empty())
        {
            const size_t start = selectionStart();
            const size_t end = selectionEnd();
            size_t nextCursor = focusedHostInput_.cursor;
            if (snowdesktop::widget_runtime::
                    TryApplyHostTextReplacement(
                        focusedHostInput_.text, start, end,
                        pasted,
                        focusedHostInput_.maximumUtf8Bytes,
                        nextCursor))
            {
                focusedHostInput_.cursor = nextCursor;
                focusedHostInput_.selectionAnchor = nextCursor;
                changed = true;
            }
        }
        break;
    }
    }

    if (changed)
        FinishHostInputEdit({previousText, previousCursor, previousSelectionAnchor}, eventSource);
    return true;
}

void WidgetEngine::FinishHostInputEdit(const text_input::Snapshot& before, const char* source)
{
    const auto widgetId = focusedHostInput_.widgetId;
    focusedHostInput_.history.Record(before,
        {focusedHostInput_.text, focusedHostInput_.cursor, focusedHostInput_.selectionAnchor});
    focusedHostInput_.caretVisibility.Request();
    if (focusedHostInput_.liveUpdate)
    {
        if (focusedHostInput_.controlled)
        {
            DispatchHostInputChange(widgetId, focusedHostInput_.id,
                focusedHostInput_.changeAction, before.text, focusedHostInput_.text,
                focusedHostInput_.numeric, focusedHostInput_.minimum,
                focusedHostInput_.maximum, false, false, source);
        }
        else
        {
            RuntimeSetStorageValue(widgetId, focusedHostInput_.storageKey,
                WideToUtf8(focusedHostInput_.text));
        }
    }
    RuntimeInvalidateHost(widgetId);
}

bool WidgetEngine::HandleHostInputKey(WPARAM key)
{
    if (!focusedHostInput_.active) return false;
    focusedHostInput_.duplicateImeResult.clear();
    if (focusedHostInput_.composing &&
        (key == VK_RETURN || key == VK_ESCAPE)) return true;
    focusedHostInput_.pendingHighSurrogate = 0;

    const bool ctrl = (snowdesktop::private_pointer::KeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (snowdesktop::private_pointer::KeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (snowdesktop::private_pointer::KeyState(VK_MENU) & 0x8000) != 0;
    if (key == VK_TAB && !ctrl && !alt)
    {
        const std::wstring widgetId = focusedHostInput_.widgetId;
        (void)HandleHostViewKey(widgetId, key, false, shift, false,
            focusedHostInput_.surface);
        return true;
    }
    if (ctrl && key == 'A')
        return ExecuteHostInputEditCommand(
            snowdesktop::widget_runtime::
                HostInputEditCommand::SelectAll,
            "keyboard");
    if (ctrl && key == 'X')
        return ExecuteHostInputEditCommand(
            snowdesktop::widget_runtime::HostInputEditCommand::Cut,
            "keyboard");
    if (ctrl && key == 'C')
        return ExecuteHostInputEditCommand(
            snowdesktop::widget_runtime::HostInputEditCommand::Copy,
            "keyboard");
    if (ctrl && key == 'V')
        return ExecuteHostInputEditCommand(
            snowdesktop::widget_runtime::HostInputEditCommand::Paste,
            "keyboard");
    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.selectionAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());
    focusedHostInput_.pointerSelecting = false;
    const std::wstring previousText = focusedHostInput_.text;
    const size_t previousCursor = focusedHostInput_.cursor;
    const size_t previousSelectionAnchor =
        focusedHostInput_.selectionAnchor;
    const auto dispatchSelectionChange = [&]() {
        if (!focusedHostInput_.controlledSelection) return;
        DispatchHostInputSelectionChange(
            focusedHostInput_.widgetId, focusedHostInput_.id,
            focusedHostInput_.selectionChangeAction,
            focusedHostInput_.text,
            previousSelectionAnchor, previousCursor,
            focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor, "keyboard");
    };
    auto hasSelection = [&]() {
        return focusedHostInput_.selectionAnchor !=
            focusedHostInput_.cursor;
    };
    auto selectionStart = [&]() {
        return std::min(focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor);
    };
    auto selectionEnd = [&]() {
        return std::max(focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor);
    };
    auto eraseSelection = [&]() {
        if (!hasSelection())
            return false;
        const size_t start = selectionStart();
        const size_t end = selectionEnd();
        focusedHostInput_.text.erase(start, end - start);
        focusedHostInput_.cursor = start;
        focusedHostInput_.selectionAnchor = start;
        return true;
    };
    auto finishMovement = [&](size_t nextCursor) {
        focusedHostInput_.cursor = std::min(
            nextCursor, focusedHostInput_.text.size());
        if (!shift)
            focusedHostInput_.selectionAnchor =
                focusedHostInput_.cursor;
        focusedHostInput_.caretVisibility.Request();
        dispatchSelectionChange();
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    };

    bool changed = false;
    const bool historyNavigation = ctrl && (key == 'Z' || key == 'Y');
    if (historyNavigation)
    {
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly)) return true;
        changed = key == 'Y' || shift
            ? focusedHostInput_.history.Redo(focusedHostInput_.text,
                focusedHostInput_.cursor, focusedHostInput_.selectionAnchor)
            : focusedHostInput_.history.Undo(focusedHostInput_.text,
                focusedHostInput_.cursor, focusedHostInput_.selectionAnchor);
    }
    else if (key == VK_ESCAPE)
    {
        BlurHostInput(true, true);
        return true;
    }
    else if (key == VK_RETURN)
    {
        if (!focusedHostInput_.multiline || ctrl)
        {
            if (!focusedHostInput_.controlled &&
                !focusedHostInput_.submitAction.id.empty())
            {
                // Commit the storage-bound editor before the callback consumes
                // or clears its value. A later blur must not restore the draft.
                const auto widgetId = focusedHostInput_.widgetId;
                const auto inputId = focusedHostInput_.id;
                const auto inputSurface = focusedHostInput_.surface;
                const auto submitAction = focusedHostInput_.submitAction;
                const auto text = focusedHostInput_.text;
                BlurHostInput(false, true);
                WidgetSurfaceScope surfaceScope(d2dState_, inputSurface.c_str());
                snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
                    trustedGestureState_, true);
                snowdesktop::widget_runtime::HostInputSubmitFocusScope focusScope(
                    hostInputSubmitFocus_, widgetId, inputId, inputSurface);
                DispatchHostInputAction(widgetId, inputId,
                    submitAction, "submit", text, false, "keyboard");
                return true;
            }
            DispatchHostInputAction(focusedHostInput_.widgetId,
                focusedHostInput_.id, focusedHostInput_.submitAction,
                "submit", focusedHostInput_.text, false, "keyboard");
            BlurHostInput(false, true);
            return true;
        }
        const size_t start = selectionStart();
        const size_t end = selectionEnd();
        size_t nextCursor = focusedHostInput_.cursor;
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly)) return true;
        if (!snowdesktop::widget_runtime::TryApplyHostTextReplacement(
                focusedHostInput_.text, start, end, L"\n",
                focusedHostInput_.maximumUtf8Bytes, nextCursor))
        {
            RuntimeInvalidateHost(focusedHostInput_.widgetId);
            return true;
        }
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly)) return true;
        focusedHostInput_.cursor = nextCursor;
        focusedHostInput_.selectionAnchor =
            focusedHostInput_.cursor;
        changed = true;
    }
    else if (key == VK_BACK)
    {
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly)) return true;
        changed = eraseSelection();
        if (!changed && focusedHostInput_.cursor > 0)
        {
            const auto start = ctrl ? snowdesktop::text_input::WordBoundary(
                focusedHostInput_.text, focusedHostInput_.cursor, false)
                : snowdesktop::text_input::PreviousBoundary(focusedHostInput_.text, focusedHostInput_.cursor);
            focusedHostInput_.text.erase(start, focusedHostInput_.cursor - start);
            focusedHostInput_.cursor = start;
            focusedHostInput_.selectionAnchor =
                focusedHostInput_.cursor;
            changed = true;
        }
    }
    else if (key == VK_DELETE)
    {
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly)) return true;
        changed = eraseSelection();
        if (!changed &&
            focusedHostInput_.cursor < focusedHostInput_.text.size())
        {
            const auto end = ctrl ? snowdesktop::text_input::WordBoundary(
                focusedHostInput_.text, focusedHostInput_.cursor, true)
                : snowdesktop::text_input::NextBoundary(focusedHostInput_.text, focusedHostInput_.cursor);
            focusedHostInput_.text.erase(focusedHostInput_.cursor, end - focusedHostInput_.cursor);
            focusedHostInput_.selectionAnchor =
                focusedHostInput_.cursor;
            changed = true;
        }
    }
    else if (snowdesktop::widget_runtime::HostInputAllowsMutation(
            true, focusedHostInput_.readOnly) && focusedHostInput_.numeric &&
        (key == VK_UP || key == VK_DOWN))
    {
        wchar_t* end = nullptr;
        double current = std::wcstod(
            focusedHostInput_.text.c_str(), &end);
        if (!end || end != focusedHostInput_.text.c_str() +
                focusedHostInput_.text.size() || !std::isfinite(current))
            current = focusedHostInput_.minimum;
        const double direction = key == VK_UP ? 1.0 : -1.0;
        const double next = std::clamp(
            current + direction * focusedHostInput_.step,
            static_cast<double>(focusedHostInput_.minimum),
            static_cast<double>(focusedHostInput_.maximum));
        focusedHostInput_.text = std::to_wstring(next);
        while (focusedHostInput_.text.size() > 1 &&
            focusedHostInput_.text.back() == L'0')
            focusedHostInput_.text.pop_back();
        if (!focusedHostInput_.text.empty() &&
            focusedHostInput_.text.back() == L'.')
            focusedHostInput_.text.pop_back();
        focusedHostInput_.cursor = focusedHostInput_.text.size();
        focusedHostInput_.selectionAnchor = focusedHostInput_.cursor;
        changed = focusedHostInput_.text != previousText;
    }
    else if (focusedHostInput_.multiline &&
        (key == VK_UP || key == VK_DOWN))
    {
        const int widgetIndex = FindWidget(
            focusedHostInput_.widgetId);
        if (widgetIndex >= 0 && d2dState_)
        {
            const auto& controls =
                widgets_[widgetIndex].hostControls;
            const auto control = std::find_if(
                controls.rbegin(), controls.rend(),
                [&](const auto& item) {
                    return item.type ==
                            LuaWidget::HostControl::Type::Input &&
                        item.multiline &&
                        item.id == focusedHostInput_.id &&
                        HostControlBelongsToSurface(
                            item, focusedHostInput_.surface);
                });
            if (control != controls.rend())
            {
                constexpr float scrollbarReserve = 8.0f;
                const float width = static_cast<float>(
                    control->rect.right - control->rect.left);
                const float innerWidth = std::max(1.0f,
                    width - control->padding.left -
                        control->padding.right - scrollbarReserve);
                const auto layout = CreateHostMultilineTextLayout(
                    d2dState_, focusedHostInput_.text,
                    control->fontSize, innerWidth);
                const auto visualCursor = snowdesktop::widget_runtime::
                    ResolveHostInputVerticalCaretPosition(
                        layout.Get(), focusedHostInput_.text,
                        focusedHostInput_.cursor,
                        key == VK_UP
                            ? snowdesktop::widget_runtime::
                                HostInputVerticalDirection::Up
                            : snowdesktop::widget_runtime::
                                HostInputVerticalDirection::Down);
                if (visualCursor)
                    return finishMovement(*visualCursor);
            }
        }

        // Keep explicit-newline navigation available if DirectWrite cannot
        // provide a layout, for example while the graphics device is reset.
        const size_t textSize = focusedHostInput_.text.size();
        const size_t cursor = std::min(
            focusedHostInput_.cursor, textSize);
        const size_t currentLineStart = cursor == 0
            ? 0
            : [&]() {
                const size_t newline =
                    focusedHostInput_.text.rfind(
                        L'\n', cursor - 1);
                return newline == std::wstring::npos
                    ? size_t{0} : newline + 1;
            }();
        const size_t column = cursor - currentLineStart;
        if (key == VK_UP && currentLineStart > 0)
        {
            const size_t previousLineEnd =
                currentLineStart - 1;
            const size_t previousLineStart =
                previousLineEnd == 0
                ? 0
                : [&]() {
                    const size_t newline =
                        focusedHostInput_.text.rfind(
                            L'\n', previousLineEnd - 1);
                    return newline == std::wstring::npos
                        ? size_t{0} : newline + 1;
                }();
            focusedHostInput_.cursor = std::min(
                previousLineStart + column,
                previousLineEnd);
        }
        else if (key == VK_DOWN)
        {
            const size_t currentLineEnd =
                focusedHostInput_.text.find(
                    L'\n', cursor);
            if (currentLineEnd != std::wstring::npos)
            {
                const size_t nextLineStart =
                    currentLineEnd + 1;
                const size_t nextLineEnd =
                    focusedHostInput_.text.find(
                        L'\n', nextLineStart);
                focusedHostInput_.cursor = std::min(
                    nextLineStart + column,
                    nextLineEnd == std::wstring::npos
                        ? textSize : nextLineEnd);
            }
        }
        return finishMovement(focusedHostInput_.cursor);
    }
    else if (key == VK_LEFT || key == VK_HOME)
    {
        if (!shift && hasSelection() && key == VK_LEFT)
            return finishMovement(selectionStart());
        else if (key == VK_HOME &&
            focusedHostInput_.multiline && !ctrl)
        {
            if (focusedHostInput_.cursor == 0)
                focusedHostInput_.cursor = 0;
            else
            {
                const size_t newline =
                    focusedHostInput_.text.rfind(
                        L'\n',
                        focusedHostInput_.cursor - 1);
                focusedHostInput_.cursor =
                    newline == std::wstring::npos
                    ? 0 : newline + 1;
            }
        }
        else
            focusedHostInput_.cursor = key == VK_HOME
                ? 0 : (ctrl ? snowdesktop::text_input::WordBoundary(focusedHostInput_.text, focusedHostInput_.cursor, false)
                    : snowdesktop::text_input::PreviousBoundary(focusedHostInput_.text, focusedHostInput_.cursor));
        return finishMovement(focusedHostInput_.cursor);
    }
    else if (key == VK_RIGHT || key == VK_END)
    {
        if (!shift && hasSelection() && key == VK_RIGHT)
            return finishMovement(selectionEnd());
        else if (key == VK_END &&
            focusedHostInput_.multiline && !ctrl)
        {
            const size_t newline =
                focusedHostInput_.text.find(
                    L'\n', focusedHostInput_.cursor);
            focusedHostInput_.cursor =
                newline == std::wstring::npos
                ? focusedHostInput_.text.size() : newline;
        }
        else
            focusedHostInput_.cursor = key == VK_END
                ? focusedHostInput_.text.size()
                : (ctrl ? snowdesktop::text_input::WordBoundary(focusedHostInput_.text, focusedHostInput_.cursor, true)
                    : snowdesktop::text_input::NextBoundary(focusedHostInput_.text, focusedHostInput_.cursor));
        return finishMovement(focusedHostInput_.cursor);
    }
    else if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU ||
        key == VK_CAPITAL || key == VK_TAB)
    {
        return true;
    }
    else
    {
        // Printable keys arrive through WM_CHAR; consume the keydown so desktop
        // shortcuts do not run while the host-rendered field owns focus.
        return true;
    }

    if (changed)
    {
        if (!historyNavigation)
            focusedHostInput_.history.Record({previousText, previousCursor, previousSelectionAnchor},
                {focusedHostInput_.text, focusedHostInput_.cursor, focusedHostInput_.selectionAnchor});
        focusedHostInput_.caretVisibility.Request();
        if (focusedHostInput_.liveUpdate)
        {
            if (focusedHostInput_.controlled)
                DispatchHostInputChange(focusedHostInput_.widgetId,
                    focusedHostInput_.id, focusedHostInput_.changeAction,
                    previousText, focusedHostInput_.text,
                    focusedHostInput_.numeric, focusedHostInput_.minimum,
                    focusedHostInput_.maximum, false, false, "keyboard");
            else
                RuntimeSetStorageValue(focusedHostInput_.widgetId,
                    focusedHostInput_.storageKey,
                    WideToUtf8(focusedHostInput_.text));
        }
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
    }
    return true;
}

void WidgetEngine::ResolveDeferredHostInputFocus(
    const std::wstring& widgetId, std::string_view surface)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& request = widgets_[index].deferredHostInputFocus;
    if (!request.MatchesSurface(surface)) return;
    const std::string controlId = request.ControlId();
    request.Clear();
    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);
    WidgetSurfaceScope surfaceScope(d2dState_, normalizedSurface.c_str());
    if (!RuntimeFocusViewTarget(widgetId, controlId, "programmatic"))
    {
        RuntimeRecordError(widgetId,
            "Deferred control.focus target was not submitted: " + controlId);
    }
}

bool WidgetEngine::RuntimeFocusHostInputFromTrustedGesture(
    const std::wstring& widgetId, const std::string& id,
    std::string& error)
{
    error.clear();
    if (!trustedGestureState_.Active() &&
        !(d2dState_ && d2dState_->openingFocusAllowed))
    {
        error = "trustedGestureRequired";
        return false;
    }
    const std::string submitSurface(CurrentWidgetSurface(d2dState_));
    if (hostInputSubmitFocus_ &&
        hostInputSubmitFocus_->Matches(widgetId, id, submitSurface))
    {
        const int submittedIndex = FindWidget(widgetId);
        if (submittedIndex < 0)
        {
            error = "hostUnavailable";
            return false;
        }
        const auto& submittedWidget = widgets_[submittedIndex];
        // Input-field clicks clear the containing widget's selection. Restore
        // only this live editor, using the callback's fresh storage value.
        if (hostInputSubmitFocus_->AllowsFocus(widgetId, id, submitSurface,
                submittedWidget.valid, submittedWidget.preview,
                IsPanelSurface(submitSurface) ? submittedWidget.panelActive
                                             : submittedWidget.desktopVisible) &&
            RuntimeFocusHostInput(widgetId, id, "programmatic"))
            return true;
        error = "controlNotFound";
        return false;
    }
    if (RuntimeFocusViewTarget(widgetId, id, "programmatic"))
        return true;

    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "hostUnavailable";
        return false;
    }
    const auto& widget = widgets_[index];
    const auto& controls = widget.hostControls;
    const std::string surface(CurrentWidgetSurface(d2dState_));
    const bool submittedWithKey = std::any_of(
        controls.begin(), controls.end(), [&](const auto& control) {
            return HostControlBelongsToSurface(control, surface) &&
                control.id == id;
        });
    if (submittedWithKey ||
        InteractionRegionsForSurface(widget, surface).Find(id))
    {
        error = "controlNotFound";
        return false;
    }
    widgets_[index].deferredHostInputFocus.Request(id, surface);
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeBlurHostInputFromTrustedGesture(
    const std::wstring& widgetId, const std::string& id,
    std::string& error)
{
    error.clear();
    if (!trustedGestureState_.Active())
    {
        error = "trustedGestureRequired";
        return false;
    }
    if (FindWidget(widgetId) < 0)
    {
        error = "hostUnavailable";
        return false;
    }
    if (!focusedHostInput_.active ||
        focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.id != id ||
        focusedHostInput_.surface != CurrentWidgetSurface(d2dState_))
    {
        error = "controlNotFocused";
        return false;
    }
    BlurHostInput(false);
    return true;
}

bool WidgetEngine::HasFocusedHostInputFor(const std::wstring& widgetId) const noexcept
{
    return focusedHostInput_.active && focusedHostInput_.widgetId == widgetId;
}

void WidgetEngine::ResetFocusedHostInput()
{
    focusedHostInput_ = {};
}

void WidgetEngine::ResetHostInputForWidget(const std::wstring& widgetId)
{
    if (focusedHostInput_.active && focusedHostInput_.widgetId == widgetId)
        focusedHostInput_ = {};
    if (hostScrollbarDrag_.active &&
        hostScrollbarDrag_.widgetId == widgetId)
        hostScrollbarDrag_ = {};
}

void WidgetEngine::CancelHostInputPointerState(std::string_view surface)
{
    if (surface.empty() || focusedHostInput_.surface == NormalizeWidgetSurface(surface))
    {
        focusedHostInput_.pointerSelecting = false;
        focusedHostInput_.selectionDragArmed = false;
    }
    if (hostScrollbarDrag_.active &&
        (surface.empty() || hostScrollbarDrag_.surface ==
            NormalizeWidgetSurface(surface)))
        hostScrollbarDrag_ = {};
}

void WidgetEngine::CloseHostInputPanel(const std::wstring& widgetId)
{
    if (focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId &&
        IsPanelSurface(focusedHostInput_.surface))
    {
        BlurHostInput(false);
    }
    if (hostScrollbarDrag_.active &&
        hostScrollbarDrag_.widgetId == widgetId &&
        IsPanelSurface(hostScrollbarDrag_.surface))
        hostScrollbarDrag_ = {};
}
