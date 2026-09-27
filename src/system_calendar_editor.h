#pragma once
#include "system_calendar_editor_state.h"
#include "system_control_prompt.h"
#include "personalization.h"
#include <cstdint>

namespace snowdesktop
{
bool ShowSystemCalendarEditor(HWND owner, calendar::CalendarEvent& event,
    const PersonalizationSettings& appearance,
    const std::shared_ptr<SystemControlPromptState>& state,
    SystemCalendarEditorActions actions);

// Creates only an invisible, self-owned form. The live layout, paint and native
// input controls are printed into BGRA pixels with the same rounded mask,
// without any live actions.
struct SystemCalendarEditorPreview
{
    int width=0,height=0;
    std::vector<std::uint32_t> pixels;
};
SystemCalendarEditorPreview RenderSystemCalendarEditorPreview(
    const calendar::CalendarEvent&, const PersonalizationSettings&,
    bool confirmDelete, unsigned dpi=96);
}
