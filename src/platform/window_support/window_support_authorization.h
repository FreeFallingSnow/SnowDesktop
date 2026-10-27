#pragma once

#include "window_capabilities.h"
#include <windows.h>
#include <functional>

namespace snowdesktop::window_support
{
struct AuthorizationResult
{
    AuthorizationState state = AuthorizationState::NotRegistered;
    DWORD error = ERROR_SUCCESS;
};
AuthorizationResult QueryAuthorization() noexcept;
DWORD StartAuthorizedHelper() noexcept;
// COM-aware, bounded wait shared with authorization; never closes the handle.
DWORD WaitForAuthorizationCompletion(HANDLE process, DWORD timeoutMs = 60000) noexcept;
// Run on a worker: UAC is an explicit user decision, never a background retry.
DWORD ConfigureAuthorization(bool enabled, HWND owner, const std::function<void()>& started = {}) noexcept;
// Dedicated helper entry, before starting any privileged runtime or IPC server.
int RunAuthorizationCommand(bool enabled, DWORD requestingHost) noexcept;
}
