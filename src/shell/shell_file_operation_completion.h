#pragma once

#include <windows.h>
#include <functional>
#include <memory>
#include "drag_drop/external_transfer_observation.h"

namespace snowdesktop
{
// Both file operations and content reads use this ownership protocol. A failed
// post or a shutdown drain destroys the callback and its outstanding OLE lease.
struct ShellFileOperationUiCompletion
{
    bool succeeded = false;
    std::function<void(bool)> callback;
    bool fileOperation = true;
    // Optional numeric diagnostic correlation; never owns the transfer.
    std::uint64_t observationTask = 0;
    std::uint64_t observationWork = 0;
};

inline void PostShellFileOperationCompletion(HWND window, UINT message,
    std::unique_ptr<ShellFileOperationUiCompletion> completion, bool succeeded)
{
    completion->succeeded = succeeded;
    const auto task = completion->observationTask;
    const auto work = completion->observationWork;
    const auto identity = reinterpret_cast<std::uintptr_t>(completion.get());
    if (PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(completion.get())))
    {
        external_transfer_observation::Record(task, external_transfer_observation::Phase::completion_posted, identity, 0, work);
        completion.release();
    }
    else external_transfer_observation::Record(task, external_transfer_observation::Phase::completion_post_failed, identity, 0, work);
}

inline std::unique_ptr<ShellFileOperationUiCompletion> TakeShellFileOperationCompletion(
    LPARAM value)
{
    return std::unique_ptr<ShellFileOperationUiCompletion>(
        reinterpret_cast<ShellFileOperationUiCompletion*>(value));
}
}
