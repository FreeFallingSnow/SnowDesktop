#pragma once

#include "control_settings.h"
#include "widget/settings/widget_settings_service.h"
#include <functional>

namespace snowdesktop::control_settings
{
// Direct declarative component settings use the same guarded service as the
// settings editor. This adapter never accesses widget storage files.
Value HandleWidgetSettings(std::string_view method, const Value& params,
    const Target& target, widget_runtime::IWidgetSettingsService& service,
    const std::function<bool(std::wstring_view)>& ensure);
}
