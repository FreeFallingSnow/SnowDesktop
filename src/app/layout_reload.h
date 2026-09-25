#pragma once

#include <utility>

namespace snowdesktop::layout_reload
{
// UI-thread handoff between a layout replacement request, asynchronous Shell
// snapshots, and rebuilding the containers that consume the loaded settings.
class State
{
public:
    void Request(bool reload = true) noexcept { readPending_ |= reload; }

    template<class Load>
    bool ApplyPendingRead(Load&& load)
    {
        if (!std::exchange(readPending_, false)) return false;
        load();
        synchronizePending_ = true;
        return true;
    }

    template<class Synchronize>
    void ApplyAfterRebuild(Synchronize&& synchronize)
    {
        if (std::exchange(synchronizePending_, false)) synchronize();
    }

private:
    bool readPending_ = false;
    bool synchronizePending_ = false;
};
}
