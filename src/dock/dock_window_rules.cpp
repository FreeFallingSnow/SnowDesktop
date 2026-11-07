#include "dock_window_rules.h"

#include <cwchar>
#include <iterator>

namespace snowdesktop::dock_window_rules
{
void DockWindowActivationObservation::Begin(bool restoring, ULONGLONG now, DWORD process, DWORD thread) noexcept
{
    awaitingRestore = restoring;
    deadline = now + (restoring ? RestoreTimeoutMs : ActivationTimeoutMs);
    processId = process;
    threadId = thread;
}

void DockWindowActivationObservation::Restored(ULONGLONG now) noexcept
{
    if (awaitingRestore && now + ActivationTimeoutMs < deadline)
        deadline = now + ActivationTimeoutMs;
    awaitingRestore = false;
}

bool DockWindowActivationObservation::Expired(ULONGLONG now) const noexcept
{ return now >= deadline; }

bool DockWindowActivationObservation::Matches(DWORD process, DWORD thread) const noexcept
{
    return processId != 0 && threadId != 0 &&
        processId == process && threadId == thread;
}

bool IsTaskWindowExplicitlyExcluded(HWND window) noexcept
{
    return window && GetPropW(window, L"ITaskList_Deleted") != nullptr;
}

bool IsTaskbarDocumentProxyCandidate(HWND window)
{
    DWORD processId = 0;
    if (!window || !GetWindowThreadProcessId(window, &processId) ||
        !processId || processId == GetCurrentProcessId())
        return false;
    if (!IsTaskbarDocumentProxyCandidateEligible(
            GetAncestor(window, GA_ROOT) == window,
            IsWindowVisible(window) != FALSE,
            GetWindow(window, GW_OWNER) != nullptr,
            true, GetWindowLongPtrW(window, GWL_STYLE),
            GetWindowLongPtrW(window, GWL_EXSTYLE),
            IsTaskWindowExplicitlyExcluded(window)))
        return false;
    // For a foreign top-level window User32 reads its cached caption, without
    // waiting for the target's window procedure.
    return GetWindowTextLengthW(window) > 0;
}
}
