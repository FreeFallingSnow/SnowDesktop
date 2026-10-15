#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace snowdesktop::shell_extensions
{
// Reset only before launching a scan. Foreground demand remains latched across
// its phases; the change sequence wakes both promotion and worker completion.
class CatalogueForegroundDemand
{
public:
    using PauseProbe = std::function<bool()>;
    explicit CatalogueForegroundDemand(bool foreground = false, PauseProbe probe = {})
        : foreground_(foreground), pauseProbe_(std::move(probe)) {}
    void Reset(bool foreground)
    {
        std::lock_guard lock(pauseMutex_);
        foreground_.store(foreground, std::memory_order_release);
        cancelled_.store(false, std::memory_order_release);
        paused_ = false;
        pausedTime_ = {};
        nextProbe_ = {};
    }
    void Promote()
    {
        if (!foreground_.exchange(true, std::memory_order_acq_rel))
        {
            { std::lock_guard lock(pauseMutex_); SetPaused(false); }
            Notify();
        }
    }
    void Cancel()
    {
        cancelled_.store(true, std::memory_order_release);
        { std::lock_guard lock(pauseMutex_); SetPaused(false); }
        Notify();
    }
    void WaitUntilAllowed()
    {
        while (true)
        {
            if (cancelled_.load(std::memory_order_acquire))
                throw std::runtime_error("catalogue scan cancelled");
            if (Requested() || !pauseProbe_ || !BackgroundPaused()) return;
            std::unique_lock lock(pauseMutex_);
            pauseChanged_.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return Requested() || cancelled_.load(std::memory_order_acquire);
            });
        }
    }
    std::uint64_t PausedMilliseconds() const
    {
        std::lock_guard lock(pauseMutex_);
        const auto total = pausedTime_ + (paused_ ? Clock::now() - pauseStarted_ : Clock::duration{});
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(total).count());
    }
    bool Requested() const { return foreground_.load(std::memory_order_acquire); }
    std::uint64_t ChangeSequence() const { return sequence_.load(std::memory_order_acquire); }
    void WaitForChange(std::uint64_t previous) const { sequence_.wait(previous, std::memory_order_acquire); }
    void Notify()
    {
        sequence_.fetch_add(1, std::memory_order_release);
        sequence_.notify_all();
        // Pair with the pause predicate lock so promotion/shutdown cannot be
        // lost between checking the atomics and entering the timed wait.
        { std::lock_guard lock(pauseMutex_); }
        pauseChanged_.notify_all();
    }
private:
    std::atomic_bool foreground_{false};
    std::atomic_bool cancelled_{false};
    std::atomic<std::uint64_t> sequence_{0};
    using Clock = std::chrono::steady_clock;
    PauseProbe pauseProbe_;
    mutable std::mutex pauseMutex_;
    std::condition_variable pauseChanged_;
    bool paused_ = false;
    Clock::duration pausedTime_{};
    Clock::time_point pauseStarted_{}, nextProbe_{};
    void SetPaused(bool paused)
    {
        if (paused_ == paused) return;
        const auto now = Clock::now();
        if (paused_) pausedTime_ += now - pauseStarted_;
        else pauseStarted_ = now;
        paused_ = paused;
    }
    bool BackgroundPaused()
    {
        const auto now = Clock::now();
        {
            std::lock_guard lock(pauseMutex_);
            if (now < nextProbe_) return paused_;
            nextProbe_ = now + std::chrono::milliseconds(250);
        }
        // Window observation may cross a system boundary. Never hold the
        // wake/predicate mutex while calling it; foreground promotion stays fast.
        const bool blocked = pauseProbe_();
        std::lock_guard lock(pauseMutex_);
        SetPaused(blocked && !Requested() && !cancelled_.load(std::memory_order_acquire));
        return paused_;
    }
};

namespace detail
{
// Registry refreshes use one worker until a foreground menu needs the result.
// The caller's task owns the work queue; adding workers never repeats an item.
template <class Task>
bool RunCatalogueWorkers(unsigned maximum, CatalogueForegroundDemand *foreground, Task task)
{
    if (!maximum) return false;
    std::vector<std::future<void>> workers;
    std::atomic_uint finished{0};
    auto launch = [&](unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            workers.push_back(std::async(std::launch::async, [&, work = task]() mutable {
                struct Completion
                {
                    std::atomic_uint &finished;
                    CatalogueForegroundDemand *demand;
                    ~Completion()
                    {
                        finished.fetch_add(1, std::memory_order_release);
                        if (demand) demand->Notify();
                    }
                } completion{finished, foreground};
                work();
            }));
    };
    try
    {
        const unsigned initial = foreground && !foreground->Requested()
            ? 1u : maximum;
        launch(initial);
        if (initial < maximum)
        {
            while (finished.load(std::memory_order_acquire) < initial)
            {
                const auto sequence = foreground->ChangeSequence();
                if (foreground->Requested())
                {
                    launch(maximum - initial);
                    break;
                }
                if (finished.load(std::memory_order_acquire) >= initial) break;
                // Sampling the sequence before both predicates prevents a
                // completion or promotion between the check and wait being lost.
                foreground->WaitForChange(sequence);
            }
        }
        for (auto &worker : workers) worker.get();
        return true;
    }
    catch (...)
    {
        // std::async futures drain before callers discard their work buffers.
        workers.clear();
        return false;
    }
}
}
}
