#pragma once
#include "taskbar_hook_protocol.h"
#include "taskbar_hook_lifecycle.h"

namespace snowdesktop::taskbar_hook
{
struct AppearanceFailureKey
{
    DWORD host = 0;
    DWORD explorer = 0;
    DWORD error = 0;

    bool Matches(const SharedState& state, const Snapshot& snapshot) const noexcept
    {
        return snapshot.enabled && snapshot.appearanceEnabled &&
            snapshot.ownerProcessId == host && state.explorerProcessId == explorer &&
            ((state.status == kStatusFailed && static_cast<DWORD>(state.lastError) == error) ||
                (state.status >= kStatusIdle && state.status < kStatusApplied));
    }

    bool IsCurrent() const noexcept
    {
        const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kSharedStateName);
        if (!mapping) return false;
        const auto* state = static_cast<const SharedState*>(MapViewOfFile(mapping,
            FILE_MAP_READ, 0, 0, sizeof(SharedState)));
        Snapshot snapshot;
        bool current = state && ReadSharedSnapshot(state, snapshot) && Matches(*state, snapshot);
        if (current)
        {
            DWORD process = 0;
            GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &process);
            current = process == explorer;
        }
        if (state) UnmapViewOfFile(state);
        CloseHandle(mapping);
        return current;
    }
};
}
