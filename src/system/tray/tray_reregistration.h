#pragma once
#include <windows.h>

namespace snowdesktop::tray
{
enum class ReregistrationPost { Skipped, Posted, Failed };

// A compatibility re-registration request must not tell the collector host or
// Explorer itself that its Shell has restarted. Query each actual recipient.
inline ReregistrationPost PostReregistration(HWND window, DWORD host, DWORD explorer, UINT message)
{
    DWORD process = 0;
    if (!window || !host || !explorer || !message ||
        !GetWindowThreadProcessId(window, &process) || !process ||
        process == host || process == explorer) return ReregistrationPost::Skipped;
    return PostMessageW(window, message, 0, 0) ? ReregistrationPost::Posted : ReregistrationPost::Failed;
}
}
