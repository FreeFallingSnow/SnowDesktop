#include "settings_window_open_rules.h"

#include <iostream>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
}

int main()
{
    using snowdesktop::settings_window_open_rules::RequestState;

    RequestState state;
    Check(!state.Pending(), "new state has no pending request");

    state.Request();
    Check(state.Pending() && state.RetryCount() == 0,
        "request becomes pending and resets retry count");
    Check(state.RecordFailure(3) && state.RetryCount() == 1,
        "first failure schedules a retry");
    Check(state.RecordFailure(3) && state.RetryCount() == 2,
        "second failure schedules a retry");
    Check(state.RecordFailure(3) && state.RetryCount() == 3,
        "third failure schedules the final retry");
    Check(!state.RecordFailure(3) && state.Pending(),
        "retry exhaustion preserves the pending request");

    state.Request();
    Check(state.Pending() && state.RetryCount() == 0,
        "a new user request restores the retry budget");
    state.MarkShown();
    Check(!state.Pending() && state.RetryCount() == 0,
        "successful display clears pending state and retries");
    Check(!state.RecordFailure(3),
        "completed requests cannot schedule retries");

    if (failures == 0)
        std::cout << "All settings window open rule tests passed.\n";
    return failures == 0 ? 0 : 1;
}
