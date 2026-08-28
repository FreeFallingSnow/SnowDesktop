#include "widget_runtime_health.h"

#include <cstdlib>
#include <iostream>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

using RuntimeHealth = snowdesktop::widget_runtime::RuntimeHealth;

void TestSuccessClearsConsecutiveErrors()
{
    RuntimeHealth health;
    Check(!health.CircuitOpen() && health.ConsecutiveErrors() == 0,
        "new runtime health must start closed without errors");

    Check(!health.RecordError(),
        "one runtime error must remain below the circuit threshold");
    Check(health.ConsecutiveErrors() == 1,
        "one runtime error must be recorded");
    health.RecordSuccess();
    Check(!health.CircuitOpen() && health.ConsecutiveErrors() == 0,
        "a successful callback must clear consecutive runtime errors");
}

void TestCircuitThresholdAndRecoveryBackoff()
{
    RuntimeHealth health;
    const auto start = RuntimeHealth::TimePoint{} + std::chrono::hours(1);

    for (std::uint32_t count = 1;
        count < RuntimeHealth::CircuitErrorThreshold;
        ++count)
    {
        Check(!health.RecordError(start),
            "circuit must remain closed below the error threshold");
        Check(health.ConsecutiveErrors() == count,
            "runtime health must count each error");
    }

    Check(health.RecordError(start),
        "circuit must open at the configured error threshold");
    Check(health.CircuitOpen() &&
            health.ConsecutiveErrors() ==
                RuntimeHealth::CircuitErrorThreshold,
        "open circuit must retain the threshold count");
    Check(health.RecordError(start) &&
            health.ConsecutiveErrors() == RuntimeHealth::CircuitErrorThreshold,
        "additional errors must not postpone recovery or inflate the count");
    Check(!health.RecoveryDue(start + std::chrono::milliseconds(999)),
        "initial automatic recovery must observe its cooldown");
    Check(health.RecoveryDue(start + RuntimeHealth::InitialRecoveryDelay),
        "initial automatic recovery must become due after one second");

    const auto firstProbe = start + RuntimeHealth::InitialRecoveryDelay;
    Check(health.BeginRecovery(firstProbe) && !health.CircuitOpen() &&
            health.RecoveryProbe(),
        "a due circuit must enter a half-open recovery probe");
    Check(health.RecordError(firstProbe),
        "a failed half-open probe must immediately reopen the circuit");
    Check(health.RecoveryAttempts() == 1,
        "a failed half-open probe must increment the backoff attempt");
    Check(!health.RecoveryDue(firstProbe + std::chrono::milliseconds(1999)),
        "the first failed probe must back off for two seconds");
    Check(health.RecoveryDue(firstProbe + std::chrono::milliseconds(2000)),
        "the first failed probe must retry after two seconds");

    const auto secondProbe = firstProbe + std::chrono::milliseconds(2000);
    Check(health.BeginRecovery(secondProbe),
        "the backed-off circuit must allow another recovery probe");
    health.RecordSuccess();
    Check(!health.CircuitOpen() && !health.RecoveryProbe() &&
            health.ConsecutiveErrors() == 0 &&
            health.RecoveryAttempts() == 0,
        "a successful recovery probe must fully close and reset the circuit");

    health.Reset();
    Check(!health.CircuitOpen() && health.ConsecutiveErrors() == 0,
        "reset must restore the new-instance health state");
}
}

int main()
{
    TestSuccessClearsConsecutiveErrors();
    TestCircuitThresholdAndRecoveryBackoff();
    std::cout << "widget runtime health tests passed\n";
    return 0;
}
