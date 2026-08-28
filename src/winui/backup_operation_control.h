/**
 * @file backup_operation_control.h
 * @brief Atomic cancellation boundary for detached Backup & data work.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace snowdesktop::winui
{
enum class BackupOperationPhase : std::uint8_t
{
    Starting,
    Cancellable,
    CancellationRequested,
    NonInterruptible,
    Finished,
};

/**
 * @brief Arbitrates cancellation against an operation's final commit gate.
 * @details The worker waits for the initial running snapshot to be published.
 * Whichever side wins RequestCancellation/TryBeginNonInterruptible owns the
 * terminal decision, so a late Cancel click can never be reported as accepted
 * after an irreversible filesystem publication has started.
 */
class BackupOperationControl final
{
public:
    [[nodiscard]] bool EnableCancellation() noexcept
    {
        bool enabled = false;
        {
            // Pair phase publication with the wait mutex so a waiter cannot
            // miss the transition between its predicate check and wait.
            std::lock_guard lock(startedMutex_);
            BackupOperationPhase expected = BackupOperationPhase::Starting;
            enabled = phase_.compare_exchange_strong(expected,
                BackupOperationPhase::Cancellable,
                std::memory_order_acq_rel, std::memory_order_acquire);
        }
        started_.notify_all();
        return enabled;
    }

    void WaitUntilStarted() const
    {
        std::unique_lock lock(startedMutex_);
        started_.wait(lock, [this] {
            return Phase() != BackupOperationPhase::Starting;
        });
    }

    [[nodiscard]] bool RequestCancellation() noexcept
    {
        {
            std::lock_guard lock(startedMutex_);
            BackupOperationPhase expected = phase_.load(
                std::memory_order_acquire);
            while (expected == BackupOperationPhase::Starting ||
                expected == BackupOperationPhase::Cancellable)
            {
                if (phase_.compare_exchange_weak(expected,
                        BackupOperationPhase::CancellationRequested,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    started_.notify_all();
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] bool TryBeginNonInterruptible() noexcept
    {
        BackupOperationPhase expected = BackupOperationPhase::Cancellable;
        return phase_.compare_exchange_strong(expected,
            BackupOperationPhase::NonInterruptible,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void Finish() noexcept
    {
        {
            std::lock_guard lock(startedMutex_);
            phase_.store(BackupOperationPhase::Finished,
                std::memory_order_release);
        }
        started_.notify_all();
    }

    [[nodiscard]] BackupOperationPhase Phase() const noexcept
    {
        return phase_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool CancellationRequested() const noexcept
    {
        return Phase() == BackupOperationPhase::CancellationRequested;
    }

    [[nodiscard]] bool NonInterruptible() const noexcept
    {
        return Phase() == BackupOperationPhase::NonInterruptible;
    }

private:
    std::atomic<BackupOperationPhase> phase_{
        BackupOperationPhase::Starting};
    mutable std::mutex startedMutex_;
    mutable std::condition_variable started_;
};
}
