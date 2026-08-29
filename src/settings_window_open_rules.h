#pragma once

#include "settings_route.h"

#include <utility>

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
        pending_ = true;
        retryCount_ = 0;
        route_ = std::move(route);
        postOpenAction_ = postOpenAction;
    }

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
    unsigned retryCount_ = 0;
    SettingsRoute route_;
    PostOpenAction postOpenAction_ = PostOpenAction::None;
};
}
