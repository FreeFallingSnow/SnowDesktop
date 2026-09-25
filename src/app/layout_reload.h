#pragma once

#include <utility>

namespace snowdesktop::layout_reload
{
enum class SynchronizeResult
{
    Pending,
    Succeeded,
    Failed,
};

// UI-thread handoff between a layout replacement request, asynchronous Shell
// snapshots, and rebuilding the containers that consume the loaded settings.
class State
{
public:
    void Request(bool reload = true) noexcept { readPending_ |= reload; }
    [[nodiscard]] bool Pending() const noexcept
    {
        return readPending_ || synchronizePending_;
    }

    template<class Load>
    bool ApplyPendingRead(Load&& load)
    {
        if (!std::exchange(readPending_, false)) return false;
        load();
        synchronizePending_ = true;
        modelReady_ = false;
        return true;
    }

    // A partial startup snapshot must not release the disk-write barrier.
    void MarkCompleteModel() noexcept
    {
        if (synchronizePending_) modelReady_ = true;
    }

    template<class Synchronize>
    SynchronizeResult ApplyAfterRebuild(Synchronize&& synchronize)
    {
        if (!synchronizePending_ || !modelReady_)
            return SynchronizeResult::Pending;
        // Consume before calling out so a reentrant Request remains queued.
        synchronizePending_ = false;
        modelReady_ = false;
        if (synchronize()) return SynchronizeResult::Succeeded;
        synchronizePending_ = true;
        modelReady_ = true;
        return SynchronizeResult::Failed;
    }

private:
    bool readPending_ = false;
    bool synchronizePending_ = false;
    bool modelReady_ = false;
};
}
