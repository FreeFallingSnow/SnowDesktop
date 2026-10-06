#pragma once
#include "ui/preview/native_component_preview_export.h"
#include "theme/personalization.h"
namespace snowdesktop::winui
{
native_component_preview::Result ExportSystemPanelPreview(
    const native_component_preview::Request& request, PersonalizationSettings appearance);
}
