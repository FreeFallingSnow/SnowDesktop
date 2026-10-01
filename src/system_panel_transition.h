#pragma once

#include <optional>
#include <utility>

namespace snowdesktop
{
// UI-thread request handoff. Request::SameTarget compares the page and its
// originating bar, without comparing the page's changing data/appearance.
template <typename Request>
class SystemPanelTransition
{
public:
    enum class Action { Open, Close, Wait };

    Action Queue(Request request, const std::optional<Request>& current,
        bool showing, bool busy)
    {
        if ((pending_ && request.SameTarget(*pending_)) ||
            (showing && !busy && current && request.SameTarget(*current)))
        {
            pending_.reset();
            return Action::Close;
        }
        pending_ = std::move(request);
        if (busy || releasing_) return Action::Wait;
        return showing ? Action::Close : Action::Open;
    }

    void Defer(Request request) { pending_ = std::move(request); }
    void Cancel() { pending_.reset(); }
    const std::optional<Request>& Pending() const { return pending_; }
    bool Releasing() const { return releasing_; }
    std::optional<Request> Take()
    {
        // Close callbacks can pump a queued native message. That message must
        // not open a new model before the previous consumer has been removed.
        if (releasing_) return {};
        return std::exchange(pending_, {});
    }

    class ReleaseGuard
    {
    public:
        explicit ReleaseGuard(SystemPanelTransition& state)
            : state_(state.releasing_ ? nullptr : &state)
        { if (state_) state_->releasing_ = true; }
        ~ReleaseGuard() { if (state_) state_->releasing_ = false; }
        ReleaseGuard(const ReleaseGuard&) = delete;
        ReleaseGuard& operator=(const ReleaseGuard&) = delete;
        explicit operator bool() const { return state_ != nullptr; }
    private:
        SystemPanelTransition* state_;
    };
    ReleaseGuard BeginRelease() { return ReleaseGuard(*this); }

private:
    std::optional<Request> pending_;
    bool releasing_ = false;
};
}
