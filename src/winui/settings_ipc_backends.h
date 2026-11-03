#pragma once

#include "settings/settings_ipc_channel.h"
#include "settings_window_host_options.h"
#include "settings/settings_fwd.h"

class WidgetEngine;

namespace snowdesktop::settings_ipc
{
class BackendServer final
{
public:
    BackendServer(Channel& channel, ISettingsController& controller,
        WidgetEngine* engine, winui::SettingsWindowHostOptions options);
    ~BackendServer();
    void ClosePages() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
winui::SettingsWindowHostOptions CreateRemoteHostOptions(Channel& channel);
}
