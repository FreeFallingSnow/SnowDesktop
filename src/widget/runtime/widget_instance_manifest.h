#pragma once

#include "widget_engine.h"

namespace snowdesktop::widget_runtime
{
// Borrow only while the instance tree is stable. A successful reload replaces
// the manifest together with the VM, including its limits and slot declarations.
inline const LuaWidgetManifest* LoadedManifest(const WidgetEngine* engine, const std::wstring& id)
{
    if (!engine) return nullptr;
    const auto& widgets = engine->GetWidgets();
    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it)
        if (it->widgetId == id) return &it->manifest;
    return nullptr;
}
}
