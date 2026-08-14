#pragma once

#include <cstdint>
#include <limits>

namespace snowdesktop::widget_runtime
{
class RuntimeHealth
{
public:
    static constexpr std::uint32_t CircuitErrorThreshold = 5;

    bool RecordError() noexcept
    {
        if (consecutiveErrors_ <
            std::numeric_limits<std::uint32_t>::max())
        {
            ++consecutiveErrors_;
        }
        if (consecutiveErrors_ >= CircuitErrorThreshold)
            circuitOpen_ = true;
        return circuitOpen_;
    }

    void Reset() noexcept
    {
        consecutiveErrors_ = 0;
        circuitOpen_ = false;
    }

    std::uint32_t ConsecutiveErrors() const noexcept
    {
        return consecutiveErrors_;
    }

    bool CircuitOpen() const noexcept
    {
        return circuitOpen_;
    }

private:
    std::uint32_t consecutiveErrors_ = 0;
    bool circuitOpen_ = false;
};
}
