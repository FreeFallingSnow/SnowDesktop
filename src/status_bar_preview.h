#pragma once
#include "native_component_preview_export.h"
#include "status_bar_view.h"

namespace snowdesktop
{
using StatusBarPreviewBackground = std::function<void(ID2D1DeviceContext*, RECT,
    const PersonalizationSettings&, float, DockPosition)>;
native_component_preview::Result ExportStatusBarPreview(const native_component_preview::Request&,
    ID2D1Device*, IDWriteFactory*, const PersonalizationSettings&, const StatusBarPreviewBackground&);
}
