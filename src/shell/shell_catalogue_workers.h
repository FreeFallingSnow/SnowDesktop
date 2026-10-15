#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <future>
#include <vector>

namespace snowdesktop::shell_extensions
{
// Reset only before launching a scan. Foreground demand remains latched across
// its phases; the change sequence wakes both promotion and worker completion.
class CatalogueForegroundDemand
{
public:
    explicit CatalogueForegroundDemand(bool foreground = false) : foreground_(foreground) {}
    void Reset(bool foreground) { foreground_.store(foreground, std::memory_order_release); }
    void Promote()
    {
        if (!foreground_.exchange(true, std::memory_order_acq_rel)) Notify();
    }
    bool Requested() const { return foreground_.load(std::memory_order_acquire); }
    std::uint64_t ChangeSequence() const { return sequence_.load(std::memory_order_acquire); }
    void WaitForChange(std::uint64_t previous) const { sequence_.wait(previous, std::memory_order_acquire); }
    void Notify()
    {
        sequence_.fetch_add(1, std::memory_order_release);
        sequence_.notify_all();
    }
private:
    std::atomic_bool foreground_{false};
    std::atomic<std::uint64_t> sequence_{0};
};

namespace detail
{
// Registry refreshes use two workers until a foreground menu needs the result.
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
            ? std::min(2u, maximum) : maximum;
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
