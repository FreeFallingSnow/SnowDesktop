#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <vector>

namespace snowdesktop::shell_extensions::detail
{
// Registry refreshes use two workers until a foreground menu needs the result.
// The caller's task owns the work queue; adding workers never repeats an item.
template <class Task>
bool RunCatalogueWorkers(unsigned maximum, const std::atomic_bool *foreground, Task task)
{
    if (!maximum) return false;
    std::vector<std::future<void>> workers;
    auto launch = [&](unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            workers.push_back(std::async(std::launch::async, task));
    };
    try
    {
        const unsigned initial = foreground && !foreground->load(std::memory_order_acquire)
            ? std::min(2u, maximum) : maximum;
        launch(initial);
        if (initial < maximum)
        {
            for (;;)
            {
                if (foreground->load(std::memory_order_acquire))
                    launch(maximum - static_cast<unsigned>(workers.size()));
                auto pending = std::find_if(workers.begin(), workers.end(), [](auto &worker) {
                    return worker.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
                });
                if (pending == workers.end()) break;
                pending->wait_for(std::chrono::milliseconds(20));
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
