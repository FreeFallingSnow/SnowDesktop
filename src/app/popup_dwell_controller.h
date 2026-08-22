#pragma once

#include <windows.h>

#include <cstddef>

/** Owns hover-candidate timing for drag-opened collection popups. */
class PopupDwellController
{
public:
    static constexpr std::size_t NoCandidate =
        static_cast<std::size_t>(-1);

    bool Track(std::size_t candidate, DWORD now)
    {
        if (candidate_ == candidate)
            return false;
        candidate_ = candidate;
        startedAt_ = now;
        return true;
    }

    void Reset()
    {
        candidate_ = NoCandidate;
        startedAt_ = 0;
    }

    /**
     * Cancels the pending candidate when a foreground surface owns the
     * pointer. Returning true lets the caller stop the associated timer.
     */
    bool CancelIfOccluded(bool occluded)
    {
        if (!occluded)
            return false;
        Reset();
        return true;
    }

    bool IsReady(DWORD now, DWORD delay) const
    {
        return candidate_ != NoCandidate &&
            now - startedAt_ >= delay;
    }

    std::size_t Candidate() const { return candidate_; }
    bool IsIdle() const
    {
        return candidate_ == NoCandidate && startedAt_ == 0;
    }

private:
    std::size_t candidate_ = NoCandidate;
    DWORD startedAt_ = 0;
};
