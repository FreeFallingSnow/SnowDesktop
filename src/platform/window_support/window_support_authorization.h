#pragma once

#include "window_capabilities.h"
#include <windows.h>

namespace snowdesktop::window_support
{
struct AuthorizationResult
{
    AuthorizationState state = AuthorizationState::NotRegistered;
    DWORD error = ERROR_SUCCESS;
};
AuthorizationResult QueryAuthorization() noexcept;
DWORD StartAuthorizedHelper() noexcept;
// Run on a worker: UAC is an explicit user decision, never a background retry.
DWORD ConfigureAuthorization(bool enabled, HWND owner) noexcept;
// Dedicated helper entry, before starting any privileged runtime or IPC server.
int RunAuthorizationCommand(bool enabled, DWORD requestingHost) noexcept;
}
