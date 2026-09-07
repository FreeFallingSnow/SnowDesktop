#pragma once

#include <windows.h>
#include <shlobj.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace snowdesktop::shell_launch_process
{

// Private, same-executable transport. This is not a supported command-line API.
enum class Action : std::uint32_t { Open, OpenWithShortcutPolicy, RunAs };

struct Request
{
    HWND owner = nullptr;
    std::wstring path;
    std::vector<unsigned char> absolutePidl;
    int showCommand = SW_SHOWNORMAL;
    Action action = Action::Open;
};

// Bounded binary payload; also checked before any Shell code runs in the child.
std::vector<unsigned char> Encode(const Request& request);
std::optional<Request> Decode(std::span<const unsigned char> bytes);

struct StartedProcess
{
    DWORD id = 0;
    explicit operator bool() const { return id != 0; }
};

// Returns after dispatch, without waiting for a Shell handler. Each request has
// its own process and deadline. The job owns only the helper, not opened apps.
StartedProcess Start(const Request& request, DWORD timeoutMs = 120000);

using Executor = bool (*)(const Request&);
bool ExecuteRequest(const Request& request);

// Must run before single-instance, desktop state, Steam and crash-watchdog setup.
// nullopt means this is not a helper command; malformed helper commands fail.
std::optional<int> TryRunCommand(Executor executor = ExecuteRequest);

} // namespace snowdesktop::shell_launch_process
