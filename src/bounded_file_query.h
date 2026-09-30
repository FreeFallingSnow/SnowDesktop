#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace snowdesktop
{
/** Read-only filesystem jobs may be stuck in a network redirector. Keep their
 * input/result independently owned, bound the caller's wait, and reuse an
 * in-flight job for the same key. Never turn a timeout into an empty success.
 * Completed jobs are refreshed on the next request; no stale authority is used.
 */
template<class Value> class BoundedFileQuery final
{
public:
    using Clock = std::chrono::steady_clock;
    struct Job
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<Value> value;
        bool done = false;
        bool delivered = false;
    };
    using Ticket = std::shared_ptr<Job>;

    Ticket Request(std::wstring key, std::function<Value()> query)
    {
        std::lock_guard lock(mutex_);
        if (const auto found = jobs_.find(key); found != jobs_.end())
        {
            std::lock_guard jobLock(found->second->mutex);
            if (!found->second->done || !found->second->delivered) return found->second;
        }
        // A permanently stalled drive gets only one worker, including across
        // page reopen. Changing library metadata cannot grow workers forever.
        std::erase_if(jobs_, [](const auto& entry) {
            std::lock_guard jobLock(entry.second->mutex);
            return entry.second->done && entry.second->delivered;
        });
        if (jobs_.size() >= 64) return {};
        auto job = std::make_shared<Job>();
        jobs_[std::move(key)] = job;
        try
        {
            std::thread([job, query = std::move(query)] {
                std::optional<Value> value;
                try { value = query(); } catch (...) {}
                {
                    std::lock_guard jobLock(job->mutex);
                    job->value = std::move(value);
                    job->done = true;
                }
                job->changed.notify_all();
            }).detach();
        }
        catch (...)
        {
            std::lock_guard jobLock(job->mutex);
            job->done = true;
            job->changed.notify_all();
        }
        return job;
    }

    static std::optional<Value> Wait(const Ticket& job, Clock::time_point deadline)
    {
        if (!job) return {};
        std::unique_lock lock(job->mutex);
        if (!job->changed.wait_until(lock, deadline, [&] { return job->done; }))
            return {};
        job->delivered = true;
        return job->value;
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::wstring, Ticket> jobs_;
};
}
