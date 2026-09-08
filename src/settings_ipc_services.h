#pragma once

#include "settings_ipc_channel.h"
#include "settings_controller.h"
#include "widget_settings_service.h"

namespace snowdesktop::settings_ipc
{
// The application owns these services and their durable mutations. The child
// owns only proxies and immutable snapshots; it never creates a SettingsStore
// or WidgetEngine.
using Localize = std::function<std::wstring(std::string_view)>;
void BindController(Channel& channel, ISettingsController& controller, Localize localize = {});
void BindWidgetService(Channel& channel, widget_runtime::IWidgetSettingsService& service);
std::unique_ptr<ISettingsController> CreateControllerProxy(Channel& channel, Localize localize = {});
std::unique_ptr<widget_runtime::IWidgetSettingsService> CreateWidgetServiceProxy(Channel& channel);
}
