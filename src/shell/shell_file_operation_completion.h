#pragma once

#include <windows.h>
#include <functional>
#include <memory>

namespace snowdesktop
{
// Both file operations and content reads use this ownership protocol. A failed
// post or a shutdown drain destroys the callback and its outstanding OLE lease.
struct ShellFileOperationUiCompletion
{
    bool succeeded = false;
    std::function<void(bool)> callback;
    bool fileOperation = true;
};

inline void PostShellFileOperationCompletion(HWND window, UINT message,
    std::unique_ptr<ShellFileOperationUiCompletion> completion, bool succeeded)
{
    completion->succeeded = succeeded;
    if (PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(completion.get())))
        completion.release();
}

inline std::unique_ptr<ShellFileOperationUiCompletion> TakeShellFileOperationCompletion(
    LPARAM value)
{
    return std::unique_ptr<ShellFileOperationUiCompletion>(
        reinterpret_cast<ShellFileOperationUiCompletion*>(value));
}
}
