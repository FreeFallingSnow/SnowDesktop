#pragma once

#include <string>

namespace shell_worker_boundary
{
// Only the operating-system cancellation result is supplied. Requests,
// worker STA, progress parsing, result retention and feedback stay production.
enum class Cancellation
{
    None,
    LegacyAborted,
    LegacyCode,
    TrackedAborted,
    TrackedAbortResult,
    TrackedUserCancelled,
    TrackedSkipped
};
void Arm(Cancellation cancellation, const std::wstring& source);
void Disarm();
unsigned Calls();
}
