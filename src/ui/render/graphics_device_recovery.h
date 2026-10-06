#pragma once

#include <windows.h>
#include <dxgi.h>
#include <cstdint>

namespace snowdesktop
{
// Shared by the real failure handlers and outer message pump. No device is
// replaced inside a drawing transaction or a nested COM/drag message loop.
class GraphicsDeviceRecovery
{
public:
    static bool IsDeviceFailure(HRESULT operation, HRESULT removedReason)
    {
        return FAILED(removedReason) || operation == DXGI_ERROR_DEVICE_REMOVED ||
            operation == DXGI_ERROR_DEVICE_RESET || operation == DXGI_ERROR_DEVICE_HUNG ||
            operation == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }

    bool Request()
    {
        if (pending_) return false;
        pending_ = true;
        nextAttempt_ = 0;
        return true;
    }
    bool Pending() const { return pending_; }
    bool Ready(std::uint64_t now, bool drawing) const
    {
        return pending_ && !drawing && now >= nextAttempt_;
    }
    void Complete(std::uint64_t now, bool succeeded)
    {
        pending_ = !succeeded;
        nextAttempt_ = succeeded ? 0 : now + 2000;
    }
private:
    bool pending_ = false;
    std::uint64_t nextAttempt_ = 0;
};
}
