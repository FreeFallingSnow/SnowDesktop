#pragma once

#include "shell_refresh_snapshot.h"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace snowdesktop::shell_refresh
{
// One startup read at a time. A Shell provider can block indefinitely, so the
// worker owns only copied input and a mailbox, never DesktopApp or its HWNDs.
// Destroying the mailbox owner must not join that uninterruptible call.
class StartupRead final
{
public:
    using Reader = std::function<bool(const Request&, Snapshot&)>;
    explicit StartupRead(Reader reader) : reader_(std::move(reader)) {}
    ~StartupRead() { Stop(); }
    StartupRead(const StartupRead&) = delete;
    StartupRead& operator=(const StartupRead&) = delete;

    bool Pending() const noexcept { return state_ != nullptr; }

    bool Start(Request request)
    {
        if (stopped_ || state_) return false;
        try
        {
            auto state = std::make_shared<State>();
            request.publishDesktopItem = [state](const DesktopItem& item) {
                auto copy = CloneReadItem(item);
                std::lock_guard lock(state->mutex);
                if (!state->abandoned) state->progress.push_back(std::move(copy));
            };
            std::thread([state, request = std::move(request), reader = reader_] {
                std::shared_ptr<Snapshot> result;
                const HRESULT initialized = CoInitializeEx(
                    nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                try
                {
                    result = std::make_shared<Snapshot>();
                    if (SUCCEEDED(initialized))
                        result->desktopComplete = reader(request, *result);
                }
                catch (...)
                {
                    if (result) result->desktopComplete = false;
                }
                if (SUCCEEDED(initialized)) CoUninitialize();
                {
                    std::lock_guard lock(state->mutex);
                    if (state->abandoned) return;
                    state->result = std::move(result);
                    state->ready = true;
                }
                state->completed.notify_one();
            }).detach();
            state_ = std::move(state);
            return true;
        }
        catch (...) { return false; }
    }

    // A timeout leaves the same read in flight. It is not an empty desktop,
    // cancellation, or permission to launch another thread for the same work.
    std::shared_ptr<Snapshot> TakeReady(std::chrono::milliseconds budget = {})
    {
        const auto state = state_;
        if (!state) return {};
        std::unique_lock lock(state->mutex);
        if (!state->completed.wait_for(lock, budget, [&] { return state->ready; }))
            return {};
        state_.reset();
        return state->result ? std::move(state->result) : std::make_shared<Snapshot>();
    }

    std::vector<DesktopItem> TakeProgress()
    {
        const auto state = state_;
        if (!state) return {};
        std::lock_guard lock(state->mutex);
        return std::exchange(state->progress, {});
    }

    void Stop() noexcept
    {
        stopped_ = true;
        const auto state = std::exchange(state_, {});
        if (!state) return;
        std::lock_guard lock(state->mutex);
        state->abandoned = true;
        state->result.reset();
        state->progress.clear();
    }

private:
    struct State
    {
        std::mutex mutex;
        std::condition_variable completed;
        bool ready = false, abandoned = false;
        std::shared_ptr<Snapshot> result;
        std::vector<DesktopItem> progress;
    };
    Reader reader_;
    std::shared_ptr<State> state_;
    bool stopped_ = false;
};
}
