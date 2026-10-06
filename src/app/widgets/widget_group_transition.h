#pragma once
#include <utility>

namespace snowdesktop
{
// Layout can run inside a drop callback while its caller still holds widget
// indices. Settle the model only after the outer message/animation dispatch
// returns, and retain the previous desktop frame during that short interval.
class WidgetGroupTransition
{
public:
    void Request()
    {
        if (resolving_) return; // The restoration itself rebuilds the layout.
        pending_ = true;
        holdFrame_ = true;
    }

    bool ShouldDeferPaint() const { return holdFrame_ || resolving_; }

    // Returns whether a full repaint is needed. A long-lived interaction may
    // keep the model pending, but must not freeze unrelated desktop updates.
    template<class Restore>
    bool FinishDispatch(bool canRestore, Restore&& restore)
    {
        if (resolving_) return false;
        const bool heldFrame = std::exchange(holdFrame_, false);
        if (!pending_ || !canRestore) return heldFrame;
        pending_ = false;
        resolving_ = true;
        struct Scope
        {
            bool& active;
            ~Scope() { active = false; }
        } scope{resolving_};
        std::forward<Restore>(restore)();
        return true;
    }

private:
    bool pending_ = false;
    bool holdFrame_ = false;
    bool resolving_ = false;
};
}
