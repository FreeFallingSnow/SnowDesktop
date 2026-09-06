#pragma once

#include "settings_ipc_channel.h"
#include "settings_controller.h"
#include "widget_settings_service.h"

namespace snowdesktop::settings_ipc
{
// The application owns these services and their durable mutations. The child
// owns only proxies and immutable snapshots; it never creates a SettingsStore
// or WidgetEngine.
void BindController(Channel& channel, ISettingsController& controller);
void BindWidgetService(Channel& channel, widget_runtime::IWidgetSettingsService& service);
std::unique_ptr<ISettingsController> CreateControllerProxy(Channel& channel);
std::unique_ptr<widget_runtime::IWidgetSettingsService> CreateWidgetServiceProxy(Channel& channel);
}
