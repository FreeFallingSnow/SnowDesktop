#pragma once

#include "settings_ipc_channel.h"
#include <windows.h>
#include <memory>
#include <string>

namespace snowdesktop::settings_ipc
{
class SettingsProcess final
{
public:
    SettingsProcess();
    ~SettingsProcess();
    SettingsProcess(const SettingsProcess&) = delete;
    SettingsProcess& operator=(const SettingsProcess&) = delete;
    void Start(Channel& channel);
    void Stop() noexcept;
    bool Running() const noexcept;
    DWORD ProcessId() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Called before single-instance discovery, watchdog or DesktopApp startup.
// A recognized but invalid child command is handled with an error; it must
// never fall through and accidentally start a second desktop runtime.
bool IsSettingsProcessCommand();
void OpenInheritedSettingsChannel(Channel& channel);
std::string ExecutableIdentity();
int RunSettingsProcess(HINSTANCE instance);
}
