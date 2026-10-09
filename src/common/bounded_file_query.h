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
 * Undelivered completions survive for the caller's retry interval. Delivered
 * or expired jobs are refreshed on the next request.
 */
template<class Value> class BoundedFileQuery final
{
public:
    using Clock = std::chrono::steady_clock;
    static BoundedFileQuery& ForProcess()
    {
        // Detached read-only jobs can reach another query after callers close.
        // Keep their process registry alive through CRT static destruction;
        // Windows reclaims it together with any stalled redirector threads.
        static auto* registry = new BoundedFileQuery;
        return *registry;
    }
    struct Job
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<Value> value;
        Clock::time_point completedAt{};
        Clock::duration undeliveredLifetime = std::chrono::seconds(5);
        bool done = false;
        bool delivered = false;
    };
    using Ticket = std::shared_ptr<Job>;

    Ticket Request(std::wstring key, std::function<Value()> query,
        Clock::duration undeliveredLifetime = std::chrono::seconds(5))
    {
        std::lock_guard lock(mutex_);
        if (const auto found = jobs_.find(key); found != jobs_.end())
        {
            std::lock_guard jobLock(found->second->mutex);
            if (!found->second->delivered)
                found->second->undeliveredLifetime = std::max(
                    found->second->undeliveredLifetime, undeliveredLifetime);
            if (!found->second->done || (!found->second->delivered &&
                Clock::now() - found->second->completedAt < found->second->undeliveredLifetime))
                return found->second;
        }
        // A permanently stalled drive gets only one worker, including across
        // page reopen. Changing library metadata cannot grow workers forever.
        std::erase_if(jobs_, [](const auto& entry) {
            std::lock_guard jobLock(entry.second->mutex);
            return entry.second->done && (entry.second->delivered ||
                Clock::now() - entry.second->completedAt >= entry.second->undeliveredLifetime);
        });
        if (jobs_.size() >= 64) return {};
        auto job = std::make_shared<Job>();
        job->undeliveredLifetime = undeliveredLifetime;
        jobs_[std::move(key)] = job;
        try
        {
            std::thread([job, query = std::move(query)] {
                std::optional<Value> value;
                try { value = query(); } catch (...) {}
                {
                    std::lock_guard jobLock(job->mutex);
                    job->value = std::move(value);
                    job->completedAt = Clock::now();
                    job->done = true;
                }
                job->changed.notify_all();
            }).detach();
        }
        catch (...)
        {
            std::lock_guard jobLock(job->mutex);
            job->completedAt = Clock::now();
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
