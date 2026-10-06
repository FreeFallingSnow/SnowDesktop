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
    template <typename Monitor>
    bool CancelForMonitor(Monitor monitor)
    {
        if (!pending_ || pending_->Monitor() != monitor) return false;
        pending_.reset();
        return true;
    }
    template <typename Owner>
    bool ShouldCancelOnDeactivation(Owner currentOwner, Owner activated) const
    { return activated != currentOwner && (!pending_ || pending_->owner != activated); }
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

    // SetWindowPos can synchronously deliver WM_DPICHANGED when a reused popup
    // moves to another monitor. Its layout already uses the destination bar's
    // DPI, so that notification belongs to placement, not outside dismissal.
    class PlacementGuard
    {
    public:
        explicit PlacementGuard(SystemPanelTransition& state)
            : state_(state), previous_(std::exchange(state.placing_, true)) {}
        ~PlacementGuard() { state_.placing_ = previous_; }
        PlacementGuard(const PlacementGuard&) = delete;
        PlacementGuard& operator=(const PlacementGuard&) = delete;
    private:
        SystemPanelTransition& state_;
        bool previous_;
    };
    PlacementGuard BeginPlacement() { return PlacementGuard(*this); }
    bool ShouldDismissForDpiChange() const { return !placing_; }

private:
    std::optional<Request> pending_;
    bool releasing_ = false;
    bool placing_ = false;
};
}
