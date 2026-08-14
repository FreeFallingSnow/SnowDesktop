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

void TestCircuitThresholdAndReset()
{
    snowdesktop::widget_runtime::RuntimeHealth health;
    Check(!health.CircuitOpen() && health.ConsecutiveErrors() == 0,
        "new runtime health must start closed without errors");

    for (std::uint32_t count = 1;
        count < snowdesktop::widget_runtime::RuntimeHealth::
            CircuitErrorThreshold;
        ++count)
    {
        Check(!health.RecordError(),
            "circuit must remain closed below the error threshold");
        Check(health.ConsecutiveErrors() == count,
            "runtime health must count each error");
    }

    Check(health.RecordError(),
        "circuit must open at the configured error threshold");
    Check(health.CircuitOpen() &&
            health.ConsecutiveErrors() ==
                snowdesktop::widget_runtime::RuntimeHealth::
                    CircuitErrorThreshold,
        "open circuit must retain the threshold count");
    Check(health.RecordError() &&
            health.ConsecutiveErrors() ==
                snowdesktop::widget_runtime::RuntimeHealth::
                    CircuitErrorThreshold + 1,
        "additional errors must keep the circuit open");

    health.Reset();
    Check(!health.CircuitOpen() && health.ConsecutiveErrors() == 0,
        "reset must restore the new-instance health state");
}
}

int main()
{
    TestCircuitThresholdAndReset();
    std::cout << "widget runtime health tests passed\n";
    return 0;
}
