#pragma once

#include <windows.h>
#include <chrono>
#include <string>
#include <vector>

namespace snowdesktop::windows_desktop_layout
{
struct ItemPosition
{
    std::wstring parsingName;
    POINT screenPosition{};
};

struct Snapshot
{
    HRESULT status = E_FAIL;
    const wchar_t* stage = L"not started";
    unsigned attempts = 0;
    POINT spacing{};
    UINT spacingDpi = 96;
    int iconSize = 0;
    std::vector<ItemPosition> items;

    bool Available() const noexcept
    {
        return status == S_OK && spacing.x > 0 && spacing.y > 0;
    }
};

// Read Explorer's desktop view on a dedicated STA. A slow Shell cannot block
// initialization beyond this budget, and at most one capture remains in flight.
// This never changes native icon positions, visibility or view settings.
Snapshot Capture(std::chrono::milliseconds budget = std::chrono::milliseconds(2500));

namespace detail
{
// Explorer may publish its desktop view after the first lookup, especially
// after Shell restart. Retry explicit not-ready results within the original
// budget. Clock/wait seams keep failure tests deterministic and Shell-free.
template<class Read, class Now, class Wait>
Snapshot ReadWhenReady(std::chrono::steady_clock::time_point deadline,
    Read read, Now now, Wait wait)
{
    Snapshot result;
    unsigned attempts = 0;
    while (now() < deadline)
    {
        result = read(deadline);
        result.attempts = ++attempts;
        if (result.Available() || (result.status != S_FALSE && result.status != E_PENDING))
            return result;
        const auto next = now() + std::chrono::milliseconds(100);
        wait(next < deadline ? next : deadline);
    }
    result.status = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return result;
}
}
}
