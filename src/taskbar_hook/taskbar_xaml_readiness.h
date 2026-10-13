#pragma once

#include <windows.h>
#include <algorithm>

namespace snowdesktop::taskbar_hook
{
struct XamlReadinessResult
{
    HRESULT error = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    unsigned endpoint = 0;
    unsigned attempts = 0;
    unsigned passes = 0;
};

// Endpoint discovery and readiness are separate: an early connection name may
// become available after it was probed. Revisit names within the same budget.
// The initializer is the only substituted OS boundary in the regression tests.
template<typename Initialize, typename Clock, typename Wait, typename Cancelled>
XamlReadinessResult WaitForTaskbarXaml(Initialize&& initialize, Clock&& clock,
    Wait&& wait, Cancelled&& cancelled)
{
    constexpr unsigned endpointCount = 60;
    constexpr ULONGLONG timeoutMs = 30000;
    constexpr DWORD pollMs = 500;
    const ULONGLONG started = clock();
    XamlReadinessResult result;
    for (;;)
    {
        ++result.passes;
        for (unsigned endpoint = 1; endpoint <= endpointCount; ++endpoint)
        {
            if (cancelled())
            {
                result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                return result;
            }
            if (clock() - started >= timeoutMs) return result;
            result.endpoint = endpoint;
            ++result.attempts;
            result.error = initialize(endpoint);
            // Only a missing diagnostics endpoint means that startup may still
            // be in progress. Do not retry access/activation errors or success.
            if (result.error != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                return result;
        }
        const ULONGLONG elapsed = clock() - started;
        if (elapsed >= timeoutMs) return result;
        wait(static_cast<DWORD>(std::min<ULONGLONG>(pollMs, timeoutMs - elapsed)));
    }
}
}
