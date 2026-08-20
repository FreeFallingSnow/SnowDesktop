#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace snowdesktop::widget_runtime
{
class RuntimeHealth
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr std::uint32_t CircuitErrorThreshold = 5;
    static constexpr std::chrono::milliseconds InitialRecoveryDelay{ 1000 };
    static constexpr std::chrono::milliseconds MaximumRecoveryDelay{ 30000 };

    bool RecordError(TimePoint now = Clock::now()) noexcept
    {
        if (circuitOpen_)
            return true;

        if (recoveryProbe_)
        {
            recoveryProbe_ = false;
            if (recoveryAttempts_ <
                std::numeric_limits<std::uint32_t>::max())
            {
                ++recoveryAttempts_;
            }
            OpenCircuit(now);
            return true;
        }

        if (consecutiveErrors_ <
            std::numeric_limits<std::uint32_t>::max())
        {
            ++consecutiveErrors_;
        }
        if (consecutiveErrors_ >= CircuitErrorThreshold)
        {
            recoveryAttempts_ = 0;
            OpenCircuit(now);
        }
        return circuitOpen_;
    }

    void RecordSuccess() noexcept
    {
        consecutiveErrors_ = 0;
        circuitOpen_ = false;
        recoveryProbe_ = false;
        recoveryAttempts_ = 0;
        nextRecovery_ = {};
    }

    void Reset() noexcept
    {
        RecordSuccess();
    }

    bool RecoveryDue(TimePoint now = Clock::now()) const noexcept
    {
        return circuitOpen_ && now >= nextRecovery_;
    }

    bool BeginRecovery(TimePoint now = Clock::now()) noexcept
    {
        if (!RecoveryDue(now))
            return false;
        circuitOpen_ = false;
        recoveryProbe_ = true;
        consecutiveErrors_ = 0;
        return true;
    }

    std::uint32_t ConsecutiveErrors() const noexcept
    {
        return consecutiveErrors_;
    }

    bool CircuitOpen() const noexcept
    {
        return circuitOpen_;
    }

    bool RecoveryProbe() const noexcept
    {
        return recoveryProbe_;
    }

    std::uint32_t RecoveryAttempts() const noexcept
    {
        return recoveryAttempts_;
    }

private:
    static constexpr std::chrono::milliseconds RecoveryDelay(
        std::uint32_t attempts) noexcept
    {
        const std::uint32_t cappedAttempts = attempts > 5 ? 5 : attempts;
        const auto delay = InitialRecoveryDelay.count() << cappedAttempts;
        return std::chrono::milliseconds(
            delay > MaximumRecoveryDelay.count()
                ? MaximumRecoveryDelay.count() : delay);
    }

    void OpenCircuit(TimePoint now) noexcept
    {
        consecutiveErrors_ = CircuitErrorThreshold;
        circuitOpen_ = true;
        nextRecovery_ = now + RecoveryDelay(recoveryAttempts_);
    }

    std::uint32_t consecutiveErrors_ = 0;
    bool circuitOpen_ = false;
    bool recoveryProbe_ = false;
    std::uint32_t recoveryAttempts_ = 0;
    TimePoint nextRecovery_{};
};
}
