#pragma once

#include "common/background_work.h"
#include <atomic>
#include <cstdint>
#include <exception>
#include <optional>

namespace snowdesktop
{
// One writer owns immutable layout snapshots. Replacing pending work never
// cancels an active atomic write; synchronous boundaries wait for that write
// and supersede older queued snapshots before committing their own document.
class LayoutSaveQueue final
{
public:
    struct Result
    {
        bool saved = false;
        std::string error;
    };
    using Write = std::function<Result()>;

    ~LayoutSaveQueue() { Flush(); background_.Stop(); }

    template<class Apply>
    bool Submit(Write write, Apply apply, HWND window, UINT message)
    {
        background_.Cancel();
        Job job{++state_->latest, std::move(write)};
        pending_ = job;
        return background_.Submit(L"layout", [state = state_, job] {
            return Run(state, job);
        }, std::move(apply), window, message);
    }

    Result SaveNow(Write write)
    {
        background_.Cancel();
        Job job{++state_->latest, std::move(write)};
        pending_.reset();
        return Run(state_, job);
    }

    std::optional<Result> Flush()
    {
        background_.Cancel();
        if (!pending_) return {};
        const auto job = std::move(*pending_);
        pending_.reset();
        return Run(state_, job);
    }

    void Drain() { background_.Drain(); }

private:
    struct Job { std::uint64_t revision; Write write; };
    struct State
    {
        std::mutex mutex;
        std::atomic<std::uint64_t> latest{0};
        std::uint64_t completed = 0;
        Result result;
    };
    static Result Run(const std::shared_ptr<State>& state, const Job& job)
    {
        std::lock_guard lock(state->mutex);
        if (job.revision != state->latest) return {};
        if (job.revision == state->completed) return state->result;
        Result result;
        try { result = job.write(); }
        catch (const std::exception& error) { result.error = error.what(); }
        catch (...) { result.error = "unexpected layout writer failure"; }
        state->completed = job.revision;
        state->result = result;
        return result;
    }

    std::shared_ptr<State> state_ = std::make_shared<State>();
    BackgroundWork background_{1, 1};
    std::optional<Job> pending_;
};
}
