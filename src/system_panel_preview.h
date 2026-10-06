#pragma once
#include "native_component_preview_export.h"
#include "system_panel.h"
namespace snowdesktop
{
native_component_preview::Result ExportSystemPanelPreview(
    const native_component_preview::Request&, ID2D1Device*, IDWriteFactory*,
    const PersonalizationSettings&, const SystemPanel::Background&);
}
