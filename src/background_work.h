#pragma once

#include <windows.h>
#include <objbase.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace snowdesktop
{
// Shell providers cannot be cancelled reliably. A fixed number of workers own
// only copied requests and a mailbox. UI callbacks are invoked solely by Drain;
// destruction cancels delivery without joining a blocked provider or retaining
// the application. Keys coalesce requests and reject cancelled/obsolete results.
class BackgroundWork final
{
public:
    using Completion = std::function<void()>;
    explicit BackgroundWork(unsigned workers = 4, size_t capacity = 2048)
        : state_(std::make_shared<State>()), workers_(workers), capacity_(capacity) {}
    ~BackgroundWork() { Stop(); }
    BackgroundWork(const BackgroundWork&) = delete;
    BackgroundWork& operator=(const BackgroundWork&) = delete;

    template<class Work, class Apply>
    bool Submit(std::wstring key, Work work, Apply apply, HWND window, UINT message)
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopped) return false;
        state_->window = window;
        state_->message = message;
        if (state_->live.contains(key)) return true;
        if (state_->live.size() >= capacity_) return false;
        while (started_ < workers_)
        {
            try { std::thread([state = state_] { Run(state); }).detach(); }
            catch (...) { if (!started_) return false; break; }
            ++started_;
        }
        const auto id = ++state_->serial;
        state_->queue.push_back({key, id,
            [work = std::move(work), apply = std::move(apply)]() mutable -> Completion {
                using Result = decltype(work());
                auto result = std::make_shared<Result>();
                try { *result = work(); } catch (...) { /* deliver an empty failure */ }
                return [result, apply = std::move(apply)]() mutable { apply(std::move(*result)); };
            }});
        state_->live.emplace(std::move(key), id);
        state_->changed.notify_one();
        return true;
    }

    void Cancel(const std::wstring& prefix = {})
    {
        std::lock_guard lock(state_->mutex);
        std::erase_if(state_->live, [&](const auto& entry) { return entry.first.starts_with(prefix); });
        std::erase_if(state_->queue, [&](const auto& entry) { return entry.key.starts_with(prefix); });
        std::erase_if(state_->ready, [&](const auto& entry) { return entry.key.starts_with(prefix); });
    }

    void Stop()
    {
        std::lock_guard lock(state_->mutex);
        state_->stopped = true;
        state_->window = nullptr;
        state_->live.clear();
        state_->queue.clear();
        state_->ready.clear();
        state_->changed.notify_all();
    }

    // Bound batches so a burst of completed icons cannot monopolize a frame.
    void Drain(std::chrono::milliseconds budget = std::chrono::milliseconds(4))
    {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        for (unsigned count = 0; count < 32; ++count)
        {
            Completion apply;
            {
                std::lock_guard lock(state_->mutex);
                if (state_->stopped || state_->ready.empty()) break;
                auto result = std::move(state_->ready.front());
                state_->ready.pop_front();
                const auto live = state_->live.find(result.key);
                if (live == state_->live.end() || live->second != result.id) continue;
                state_->live.erase(live);
                apply = std::move(result.apply);
            }
            if (apply) apply();
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
        std::lock_guard lock(state_->mutex);
        if (!state_->ready.empty() && state_->window)
            PostMessageW(state_->window, state_->message, 0, 0);
    }

private:
    struct Job { std::wstring key; unsigned long long id; std::function<Completion()> run; };
    struct Ready { std::wstring key; unsigned long long id; Completion apply; };
    struct State
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<Job> queue;
        std::deque<Ready> ready;
        std::unordered_map<std::wstring, unsigned long long> live;
        unsigned long long serial = 0;
        bool stopped = false;
        HWND window = nullptr;
        UINT message = 0;
    };
    static void Run(const std::shared_ptr<State>& state)
    {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;)
        {
            Job job;
            {
                std::unique_lock lock(state->mutex);
                state->changed.wait(lock, [&] { return state->stopped || !state->queue.empty(); });
                if (state->stopped) break;
                job = std::move(state->queue.front());
                state->queue.pop_front();
            }
            auto completion = job.run();
            std::lock_guard lock(state->mutex);
            const auto live = state->live.find(job.key);
            if (state->stopped || live == state->live.end() || live->second != job.id) continue;
            state->ready.push_back({std::move(job.key), job.id, std::move(completion)});
            if (state->window) PostMessageW(state->window, state->message, 0, 0);
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }
    std::shared_ptr<State> state_;
    unsigned workers_, started_ = 0;
    size_t capacity_;
};

struct BackgroundBitmap
{
    HBITMAP bitmap = nullptr;
    SIZE size{};
    ~BackgroundBitmap() { if (bitmap) DeleteObject(bitmap); }
};
}
