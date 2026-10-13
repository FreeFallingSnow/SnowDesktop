#pragma once

#include "settings_route.h"

#include <Windows.h>

#include <algorithm>
#include <utility>
#include <cstdint>

namespace snowdesktop::settings_window_open_rules
{
enum class PostOpenAction
{
    None,
    ShowExitConfirmation,
};

class RequestState
{
public:
    void Request(
        SettingsRoute route = {},
        PostOpenAction postOpenAction = PostOpenAction::None)
    {
        ++revision_;
        pending_ = true;
        retryCount_ = 0;
        route_ = std::move(route);
        postOpenAction_ = postOpenAction;
    }

    void Cancel() { ++revision_; pending_ = false; retryCount_ = 0; postOpenAction_ = PostOpenAction::None; }
    std::uint64_t Revision() const { return revision_; }
    bool IsCurrent(std::uint64_t revision) const { return pending_ && revision_ == revision; }
    bool Pending() const { return pending_; }
    unsigned RetryCount() const { return retryCount_; }
    const SettingsRoute& Route() const { return route_; }

    PostOpenAction MarkShown()
    {
        pending_ = false;
        retryCount_ = 0;
        const PostOpenAction action = postOpenAction_;
        postOpenAction_ = PostOpenAction::None;
        return action;
    }

    bool RecordFailure(unsigned maximumAutomaticRetries)
    {
        if (!pending_ || retryCount_ >= maximumAutomaticRetries)
            return false;
        ++retryCount_;
        return true;
    }

private:
    bool pending_ = false;
    std::uint64_t revision_ = 0;
    unsigned retryCount_ = 0;
    SettingsRoute route_;
    PostOpenAction postOpenAction_ = PostOpenAction::None;
};
}

namespace snowdesktop::settings_window_placement
{
// All rectangles and sizes are physical screen pixels. A work area can have
// a negative origin and excludes the taskbar and registered desktop AppBars.
inline bool HasArea(const RECT& area) noexcept
{
    return area.right > area.left && area.bottom > area.top;
}

inline SIZE FitSize(SIZE requested, const RECT& work) noexcept
{
    if (!HasArea(work)) return requested;
    return {
        std::clamp(requested.cx, 1L, work.right - work.left),
        std::clamp(requested.cy, 1L, work.bottom - work.top)};
}

inline RECT Constrain(RECT requested, const RECT& work) noexcept
{
    if (!HasArea(work)) return requested;
    const auto size = FitSize(
        {requested.right - requested.left, requested.bottom - requested.top}, work);
    const LONG left = std::clamp(requested.left, work.left, work.right - size.cx);
    const LONG top = std::clamp(requested.top, work.top, work.bottom - size.cy);
    return {left, top, left + size.cx, top + size.cy};
}

inline RECT Center(SIZE requested, const RECT& work) noexcept
{
    const auto size = FitSize(requested, work);
    const LONG left = work.left + (work.right - work.left - size.cx) / 2;
    const LONG top = work.top + (work.bottom - work.top - size.cy) / 2;
    return {left, top, left + size.cx, top + size.cy};
}
} // namespace snowdesktop::settings_window_placement
