#pragma once

#include <windows.h>

namespace snow::wallpaper_hook
{

// AcquireSync has positive non-success results. Only S_OK owns the surface;
// abandoned surfaces must be rebuilt and must never be copied or released.
template<class Acquire, class Release, class Operation, class Recreate>
HRESULT UseSharedFrame(Acquire&& acquire, Release&& release,
    Operation&& operation, Recreate&& recreate)
{
    const HRESULT result = acquire();
    if (result == static_cast<HRESULT>(WAIT_ABANDONED))
        recreate();
    if (result != S_OK)
        return result;
    struct ReleaseOwner
    {
        Release& callback;
        ~ReleaseOwner() { callback(); }
    } owner{ release };
    return operation();
}

} // namespace snow::wallpaper_hook
