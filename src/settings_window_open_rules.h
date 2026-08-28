#pragma once

#include "settings_route.h"

#include <utility>

namespace snowdesktop::settings_window_open_rules
{
class RequestState
{
public:
    void Request(SettingsRoute route = {})
    {
        pending_ = true;
        retryCount_ = 0;
        route_ = std::move(route);
    }

    bool Pending() const { return pending_; }
    unsigned RetryCount() const { return retryCount_; }
    const SettingsRoute& Route() const { return route_; }

    void MarkShown()
    {
        pending_ = false;
        retryCount_ = 0;
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
};
}
