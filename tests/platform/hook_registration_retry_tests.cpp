#include "platform/hook_registration_retry.h"
#include <iostream>

int main()
{
    using snowdesktop::hook_registration::Retry;
    Retry retry;
    unsigned attempts = 0;
    // A failed Dock/collector can receive thousands of refreshes while the
    // same session condition remains unchanged. They must not all register.
    for (ULONGLONG tick = 0; tick <= 10000; ++tick)
        if (retry.Ready(tick)) { ++attempts; retry.Failed(ERROR_MOD_NOT_FOUND, tick); }
    if (attempts > 5 || attempts == 0 || retry.Ready(10000))
    { std::cerr << "FAIL repeated refreshes should not repeatedly register\n"; return 1; }
    retry.Reset(); // A new Explorer/target generation is a new opportunity.
    if (!retry.Ready(10000) || retry.Error() != ERROR_SUCCESS)
    { std::cerr << "FAIL generation change must allow an immediate attempt\n"; return 1; }
    retry.Failed(ERROR_ACCESS_DENIED, 10000);
    if (retry.Ready(10000) || !retry.Remaining(10000))
    { std::cerr << "FAIL non126 failures must also avoid a hot loop\n"; return 1; }
    if (!retry.Ready(100000))
    { std::cerr << "FAIL failure does not permanently disable reconnection\n"; return 1; }
    std::cout << "PASS bounded refresh retry, generation reset and eventual retry\n";
    return 0;
}
