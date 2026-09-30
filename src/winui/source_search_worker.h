#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace snowdesktop::winui::widgets_page_backend_detail
{
/** One serialized query with a coalesced pending request. A provider can be
 * stuck inside synchronous I/O, so closing the page cancels and abandons only
 * its independently owned worker state. It never joins I/O on the UI owner.
 * Terminal completion remains necessary to release a live page's operation.
 */
template<class Work, class Result> class SourceSearchWorker final
{
    struct State
    {
        explicit State(std::function<Result(Work&)> function)
            : query(std::move(function)) {}
        std::mutex mutex;
        std::function<Result(Work&)> query;
        std::optional<Work> pending;
        std::shared_ptr<std::atomic_bool> activeCancellation;
        std::uint64_t activeTaskId = 0;
        bool running = false;
        bool stopping = false;
    };

    static void Complete(Work& work, Result result) noexcept
    {
        try { if (work.completion) work.completion(std::move(result)); }
        catch (...) {}
    }

    static void Run(const std::shared_ptr<State>& state)
    {
        for (;;)
        {
            std::optional<Work> work;
            {
                std::lock_guard lock(state->mutex);
                if (state->stopping || !state->pending)
                {
                    state->running = false;
                    return;
                }
                work = std::exchange(state->pending, std::nullopt);
                state->activeTaskId = work->taskId;
                state->activeCancellation = work->cancellation;
            }
            Result result;
            try { result = state->query(*work); }
            catch (...) { result.cancelled = true; }
            bool stopping = false;
            {
                std::lock_guard lock(state->mutex);
                if (work->cancellation && work->cancellation->load())
                    result.cancelled = true;
                state->activeTaskId = 0;
                state->activeCancellation.reset();
                stopping = state->stopping;
            }
            if (!stopping) Complete(*work, std::move(result));
        }
    }

public:
    explicit SourceSearchWorker(std::function<Result(Work&)> query)
        : state_(std::make_shared<State>(std::move(query))) {}
    ~SourceSearchWorker() { Shutdown(); }
    SourceSearchWorker(const SourceSearchWorker&) = delete;
    SourceSearchWorker& operator=(const SourceSearchWorker&) = delete;

    bool Submit(Work work)
    {
        std::optional<Work> displaced;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopping) return false;
            displaced = std::exchange(state_->pending, std::nullopt);
            state_->pending = std::move(work);
            if (!state_->running)
            {
                state_->running = true;
                try { std::thread(&SourceSearchWorker::Run, state_).detach(); }
                catch (...)
                {
                    state_->running = false;
                    state_->pending.reset();
                    return false;
                }
            }
        }
        if (displaced)
        {
            Result result;
            result.cancelled = true;
            Complete(*displaced, std::move(result));
        }
        return true;
    }

    bool RequestCancel(std::uint64_t taskId)
    {
        std::optional<Work> cancelled;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->pending && state_->pending->taskId == taskId)
                cancelled = std::exchange(state_->pending, std::nullopt);
            else if (state_->activeTaskId == taskId && state_->activeCancellation)
                state_->activeCancellation->store(true);
            else return false;
        }
        if (cancelled)
        {
            Result result;
            result.cancelled = true;
            Complete(*cancelled, std::move(result));
        }
        return true;
    }

    void Shutdown() noexcept
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        if (state_->activeCancellation) state_->activeCancellation->store(true);
        if (state_->pending && state_->pending->cancellation)
            state_->pending->cancellation->store(true);
        state_->pending.reset();
    }

private:
    std::shared_ptr<State> state_;
};
}
