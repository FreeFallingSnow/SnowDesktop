#pragma once
#include "../native_component_preview_export.h"
#include "../personalization.h"
namespace snowdesktop::winui
{
native_component_preview::Result ExportSystemPanelPreview(
    const native_component_preview::Request& request, PersonalizationSettings appearance);
}
