#pragma once

#include "control_settings.h"
#include "widget/settings/widget_settings_service.h"
#include <functional>

namespace snowdesktop::control_settings
{
struct WidgetThemeControl
{
    // Read-only, named binding/choice state participates in optimistic
    // concurrency, including detach operations that do not change appearance.
    std::function<Value(std::wstring_view)> read;
    std::function<widget_runtime::WidgetSettingMutationResult(
        const widget_runtime::WidgetSettingMutationGuard&, std::string_view, bool)> apply;
};
// Direct declarative component settings use the same guarded service as the
// settings editor. This adapter never accesses widget storage files.
Value HandleWidgetSettings(std::string_view method, const Value& params,
    const Target& target, widget_runtime::IWidgetSettingsService& service,
    const std::function<bool(std::wstring_view)>& ensure,
    const WidgetThemeControl& themes = {});
}
