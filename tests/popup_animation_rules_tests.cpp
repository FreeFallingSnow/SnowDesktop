#include "popup_animation_rules.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

bool NearlyEqual(float left, float right, float tolerance = 0.0001f)
{
    return std::fabs(left - right) <= tolerance;
}
}

int main()
{
    using namespace snowdesktop::popup_animation_rules;

    State state;
    Check(state.IsHidden(), "new state starts hidden");
    Check(!state.IsInteractive(), "hidden popup does not accept input");

    state.Open(1000);
    Check(state.IsAnimating(), "opening starts an animation");
    Check(state.IsInteractive(), "opening popup accepts input immediately");
    state.Advance(1000 + kOpenDurationMs / 2);
    const Visual halfOpen = state.GetVisual();
    Check(halfOpen.visible, "half-open popup is visible");
    Check(NearlyEqual(halfOpen.progress, 0.5f),
        "opening progress advances linearly");
    Check(NearlyEqual(
            EaseInOutSmooth(0.5f), 0.5f),
        "scale easing is centered");
    Check(EaseInOutSmooth(0.25f) < 0.25f &&
          EaseInOutSmooth(0.75f) > 0.75f,
        "scale easing slows at both endpoints");
    Check(kOpenDurationMs <= 90 &&
          kCloseDurationMs <= 90,
        "popup scale animation stays responsive");
    Check(ShouldUsePopupItemBounds(true, true),
        "interactive popup owns member coordinates");
    Check(!ShouldUsePopupItemBounds(true, false),
        "closing popup cannot replace exposed-item drag coordinates");
    Check(!ShouldUsePopupItemBounds(false, true),
        "missing popup source cannot provide member coordinates");
    Check(halfOpen.scale >
            kMinimumScale + 0.35f,
        "opening visibly grows instead of popping in");

    state.Advance(1000 + kOpenDurationMs);
    const Visual open = state.GetVisual();
    Check(!state.IsAnimating(), "opening finishes at its duration");
    Check(NearlyEqual(open.scale, 1.0f), "open popup reaches full scale");

    state.Close(2000);
    Check(state.IsClosing(), "closing state is reported");
    Check(!state.IsInteractive(), "closing popup stops accepting input");
    state.Advance(2000 + kCloseDurationMs / 2);
    const Visual halfClosed = state.GetVisual();
    Check(NearlyEqual(halfClosed.progress, 0.5f),
        "closing progress advances toward hidden");
    Check(halfClosed.scale >
            kMinimumScale + 0.35f,
        "closing visibly shrinks instead of disappearing");
    state.Advance(2000 + kCloseDurationMs);
    Check(state.IsHidden(), "closing reaches hidden");
    Check(!state.IsAnimating(), "closing animation stops when hidden");

    state.Open(3000);
    state.Advance(3060);
    const Visual beforeReverse = state.GetVisual();
    state.Close(3060);
    const Visual afterReverse = state.GetVisual();
    Check(NearlyEqual(beforeReverse.scale, afterReverse.scale),
        "reversing to close keeps scale continuous");
    state.Advance(3090);
    const Visual closing = state.GetVisual();
    Check(closing.progress < beforeReverse.progress,
        "reversed close moves toward hidden");
    state.Open(3090);
    const Visual reopened = state.GetVisual();
    Check(NearlyEqual(closing.scale, reopened.scale),
        "reversing back to open keeps scale continuous");
    Check(state.IsInteractive(), "reopened popup accepts input");

    state.ShowImmediately();
    Check(!state.IsAnimating(), "show-immediately is stable");
    Check(NearlyEqual(state.GetVisual().progress, 1.0f),
        "show-immediately reaches open state");
    state.ResetHidden();
    Check(state.IsHidden(), "reset-hidden clears state");

    Check(ResolveExistingSourceAction(false, true) ==
            ExistingSourceAction::OpenAtRequestedAnchor,
        "a different collection may open at its requested anchor");
    Check(ResolveExistingSourceAction(true, true) ==
            ExistingSourceAction::CloseExisting,
        "clicking an open collection from another anchor closes it");
    Check(ResolveExistingSourceAction(true, false) ==
            ExistingSourceAction::ReopenExisting,
        "clicking the same collection while closing reverses it immediately");
    Check(ResolveExistingSourceAction(true, false, true) ==
            ExistingSourceAction::KeepClosing,
        "the pointer press that started closing does not reopen the popup");
    Check(ShouldDispatchCollectionDoubleClickPress(true, false),
        "a collection toggle double click replays the second press");
    Check(!ShouldDispatchCollectionDoubleClickPress(true, true),
        "an interactive popup keeps ownership of double clicks inside it");
    Check(!ShouldDispatchCollectionDoubleClickPress(false, false),
        "unrelated desktop double clicks retain their existing behavior");

    std::cout << "popup animation rules tests passed\n";
    return 0;
}
