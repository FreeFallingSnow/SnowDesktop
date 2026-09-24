#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

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

/** Passive popup opening is opt-in and independent of drag dwell. */
class PopupHoverController
{
public:
    static constexpr DWORD DelayMs = 600;

    void Track(const std::wstring& token, DWORD now)
    {
        if (token_ == token) return;
        token_ = token;
        startedAt_ = now;
        consumed_ = false;
    }

    bool IsReady(DWORD now) const
    {
        return Pending() && now - startedAt_ >= DelayMs;
    }

    bool Consume(DWORD now)
    {
        if (!IsReady(now)) return false;
        consumed_ = true;
        return true;
    }

    // Clicking or dismissing must not reopen the same target until re-entry.
    void SuppressUntilLeave() { consumed_ = true; }
    void Reset() { token_.clear(); startedAt_ = 0; consumed_ = false; }
    bool Pending() const { return !token_.empty() && !consumed_; }

private:
    std::wstring token_;
    DWORD startedAt_ = 0;
    bool consumed_ = false;
};
