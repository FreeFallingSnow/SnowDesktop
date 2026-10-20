#pragma once

#include "wallpaper_hook_protocol.h"

namespace snow::wallpaper_hook
{

// Once callbacks can enter the DLL, neither an active count nor a finite wait
// proves that no suspended thread will later enter its prologue. The module,
// trampoline allocation and mapped state therefore stay valid for the target
// process lifetime. One worker services retries and later preview sessions.
template<class Operations>
DWORD RunCaptureHookWorker(SharedState& state, Operations& operations)
{
    for (;;)
    {
        InterlockedExchange(&state.capture_enabled, 0);
        InterlockedExchange(&state.status, static_cast<LONG>(Status::starting));
        InterlockedExchange(&state.shutdown_requested, 0);
        HRESULT result = operations.Initialize();
        if (result == S_OK)
        {
            InterlockedExchange(&state.last_hresult, S_OK);
            InterlockedExchange(&state.status, static_cast<LONG>(Status::hooked));
            ULONGLONG lastConsumer = operations.Now();
            while (state.shutdown_requested == 0)
            {
                const ULONGLONG now = operations.Now();
                InterlockedExchange64(&state.producer_heartbeat,
                    static_cast<LONG64>(now));
                const auto heartbeat = static_cast<ULONGLONG>(state.consumer_heartbeat);
                if (heartbeat)
                    lastConsumer = heartbeat;
                if (now >= lastConsumer && now - lastConsumer >= 10000)
                    break;
                if (!operations.Wait(100))
                    return 0; // Only an isolated test worker has a finite lifetime.
            }
        }
        InterlockedExchange(&state.capture_enabled, 0);
        const HRESULT disableResult = operations.Disable();
        if (disableResult != S_OK)
            result = disableResult;
        InterlockedExchange(&state.last_hresult, result);
        InterlockedExchange(&state.status, static_cast<LONG>(
            result == S_OK ? Status::stopping : Status::failed));

        // Keep a live owner on every initialization failure as well. The
        // receiver explicitly requests another attempt; it never rewrites the
        // failed status of a dead worker or waits for a stale mapping to vanish.
        while (state.shutdown_requested != kRestartRequested)
        {
            InterlockedExchange64(&state.producer_heartbeat,
                static_cast<LONG64>(operations.Now()));
            if (!operations.Wait(50))
                return 0;
        }
    }
}

} // namespace snow::wallpaper_hook
