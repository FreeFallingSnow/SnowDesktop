#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace snowdesktop::hook_registration
{
// Registration failures do not become recoverable merely because the next
// window-list update arrived. Keep retries bounded until an input changes.
class Retry final
{
public:
    bool Ready(ULONGLONG now) const noexcept { return !failures_ || now >= next_; }
    void Failed(DWORD error, ULONGLONG now) noexcept
    {
        error_ = error;
        failures_ = (std::min)(failures_ + 1, 6u);
        const ULONGLONG base = error == ERROR_MOD_NOT_FOUND ? 1000 : 250;
        const ULONGLONG delay = (std::min)(base << (failures_ - 1), ULONGLONG{30000});
        next_ = now + delay;
    }
    ULONGLONG Remaining(ULONGLONG now) const noexcept { return Ready(now) ? 0 : next_ - now; }
    DWORD Error() const noexcept { return error_; }
    void Reset() noexcept { failures_ = 0; next_ = 0; error_ = ERROR_SUCCESS; }
private:
    unsigned failures_ = 0;
    ULONGLONG next_ = 0;
    DWORD error_ = ERROR_SUCCESS;
};

struct Statistics
{
    std::uint64_t attempts = 0, successes = 0, failures = 0;
    DWORD lastError = ERROR_SUCCESS, lastThread = 0;
};
}
