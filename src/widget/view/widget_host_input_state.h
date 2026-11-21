#pragma once

#include "ui/input/text_input_state.h"
#include "widget/view/widget_interaction_region.h"
#include "widget/view/widget_text_input_rules.h"
#include "widget/view/widget_view_tree.h"

#include <cstddef>
#include <optional>
#include <string>

namespace snowdesktop::widget_runtime
{
struct WidgetFocusedHostInput
{
    snowdesktop::text_input::History history;
    bool active = false;
    std::wstring widgetId;
    std::string id;
    std::string surface = "desktop";
    std::string storageKey;
    snowdesktop::widget_runtime::InteractionAction changeAction;
    snowdesktop::widget_runtime::InteractionAction focusAction;
    snowdesktop::widget_runtime::InteractionAction blurAction;
    snowdesktop::widget_runtime::InteractionAction submitAction;
    std::wstring text;
    std::wstring originalText;
    std::wstring modelText;
    std::optional<std::wstring> deferredModelText;
    size_t cursor = 0;
    size_t selectionAnchor = 0;
    std::optional<snowdesktop::widget_runtime::ViewTextSelection>
        controlledSelection;
    snowdesktop::widget_runtime::InteractionAction
        selectionChangeAction;
    size_t pointerSelectionStartCursor = 0;
    size_t pointerSelectionStartAnchor = 0;
    std::wstring compositionText;
    std::wstring duplicateImeResult;
    bool composing = false;
    bool deferredSelection = false;
    size_t compositionCursor = 0;
    wchar_t pendingHighSurrogate = 0;
    bool pointerSelecting = false;
    bool selectionDragArmed = false;
    bool controlled = false;
    bool readOnly = false;
    bool numeric = false;
    bool liveUpdate = true;
    bool multiline = false;
    snowdesktop::widget_runtime::HostInputCaretVisibilityRequest
        caretVisibility;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float step = 0.01f;
    std::size_t maximumUtf8Bytes = 0;
};

struct WidgetHostScrollbarDrag
{
    bool active = false;
    std::wstring widgetId;
    std::string id;
    std::string surface = "desktop";
    bool horizontal = false;
    int pointerStart = 0;
    int offsetStart = 0;
};
}
