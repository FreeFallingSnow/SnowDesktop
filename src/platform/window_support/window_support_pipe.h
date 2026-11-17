#pragma once

#include <windows.h>

namespace snowdesktop::window_support::pipe
{
// Overlapped operations have a bounded lifetime, including cancellation. No
// stack-backed OVERLAPPED or packet survives a returned transfer.
inline bool Transfer(HANDLE handle, void* bytes, DWORD size, bool write, DWORD* messageSize = nullptr)
{
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    OVERLAPPED operation{};
    operation.hEvent = event;
    DWORD transferred = 0;
    BOOL result = write ? WriteFile(handle, bytes, size, &transferred, &operation)
                        : ReadFile(handle, bytes, size, &transferred, &operation);
    DWORD error = result ? ERROR_SUCCESS : GetLastError();
    if (!result && error == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(event, 1500) != WAIT_OBJECT_0)
        {
            CancelIoEx(handle, &operation);
            GetOverlappedResult(handle, &operation, &transferred, TRUE);
            error = ERROR_TIMEOUT;
        }
        else
        {
            result = GetOverlappedResult(handle, &operation, &transferred, FALSE);
            error = result ? ERROR_SUCCESS : GetLastError();
        }
    }
    CloseHandle(event);
    if (error || (!messageSize && transferred != size))
    { SetLastError(error ? error : ERROR_INVALID_DATA); return false; }
    if (messageSize) *messageSize = transferred;
    return true;
}
}
